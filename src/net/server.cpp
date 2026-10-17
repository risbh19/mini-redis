#include "net/server.hpp"

#include <netinet/in.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "net/thread_pool.hpp"
#include "protocol/resp_writer.hpp"
#include "util/logger.hpp"
#include "util/string_utils.hpp"
namespace mr {
volatile std::sig_atomic_t interrupted = 0;
Server::Server(Config config, IClock& clock, CommandTable& commands)
    : config_(std::move(config)),
      clock_(clock),
      commands_(commands),
      listener_(listen_tcp(config_.bind, config_.port, true)) {}
bool Server::stopping() const { return interrupted || commands_.shutdown_requested(); }
bool Server::timed_out(const Connection& c) const {
  return config_.idle_timeout_ms && clock_.now_ms() - c.last_activity >= config_.idle_timeout_ms;
}
void Server::run() {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&addr), &len) < 0)
    throw std::runtime_error("getsockname failed");
  std::cout << "READY " << config_.bind << ':' << ntohs(addr.sin_port) << ' ' << config_.io_model
            << '\n'
            << std::flush;
  if (config_.io_model == "epoll")
    run_epoll();
  else
    run_thread_pool();
  commands_.flush();
  log("info", "shutdown complete");
}
void Server::process(Connection& c) {
  std::size_t consumed = 0;
  while (!c.closing && !commands_.shutdown_requested() && c.pending() < config_.output_limit) {
    const auto parsed = c.parser.parse(std::string_view(c.input).substr(consumed));
    if (parsed.status == ParseStatus::Incomplete) break;
    if (parsed.status == ParseStatus::ProtocolError) {
      resp_append(c.output, Reply::error("ERR Protocol error: " + parsed.error));
      c.closing = true;
      break;
    }
    consumed += parsed.consumed;
    if (parsed.args.empty()) continue;
    const auto name = upper(parsed.args[0]);
    const auto reply = commands_.execute(parsed.args);
    if (name != "SHUTDOWN" || reply.is_error()) resp_append(c.output, reply);
    if ((name == "QUIT" || name == "EXIT") && !reply.is_error()) c.closing = true;
  }
  if (consumed) c.input.erase(0, consumed);
}
bool Server::read(Connection& c) {
  std::array<char, 16384> bytes{};
  std::size_t total = 0;
  while (!c.closing && c.pending() < config_.output_limit && total < 256ULL * 1024) {
    const auto n = ::recv(c.fd.get(), bytes.data(), bytes.size(), 0);
    if (n == 0) {
      c.read_eof = true;
      break;
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return false;
    }
    c.last_activity = clock_.now_ms();
    total += static_cast<std::size_t>(n);
    c.input.append(bytes.data(), static_cast<std::size_t>(n));
    process(c);
    if (c.input.size() > config_.parser_limits.max_request) {
      resp_append(c.output, Reply::error("ERR Protocol error: request buffer too large"));
      c.closing = true;
    }
  }
  return true;
}
bool Server::write(Connection& c) {
  std::size_t total = 0;
  while (c.pending() && total < 256ULL * 1024) {
    const auto n = ::send(c.fd.get(), c.output.data() + c.sent, c.pending(), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
      return false;
    }
    if (n == 0) return false;
    c.sent += static_cast<std::size_t>(n);
    total += static_cast<std::size_t>(n);
    c.last_activity = clock_.now_ms();
  }
  if (c.sent == c.output.size()) {
    c.output.clear();
    c.sent = 0;
  } else if (c.sent > config_.output_limit) {
    c.output.erase(0, c.sent);
    c.sent = 0;
  }
  // Buffered pipelines must resume even when the kernel has no new readable bytes.
  if (!c.closing && c.pending() < config_.output_limit) process(c);
  return !((c.closing || c.read_eof) && !c.pending() && (c.closing || c.input.empty()));
}
void Server::run_epoll() {
  UniqueFd epoll(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll) throw std::runtime_error("epoll_create1 failed");
  epoll_event accept_event{};
  accept_event.events = EPOLLIN;
  accept_event.data.u64 = 0;
  if (::epoll_ctl(epoll.get(), EPOLL_CTL_ADD, listener_.get(), &accept_event) < 0)
    throw std::runtime_error("epoll add listener failed");
  std::unordered_map<std::uint64_t, Connection> connections;
  std::uint64_t next_id = 1;
  std::array<epoll_event, 256> events{};
  auto last_tick = clock_.now_ms();
  auto close = [&](std::uint64_t id) {
    const auto it = connections.find(id);
    if (it == connections.end()) return;
    ::epoll_ctl(epoll.get(), EPOLL_CTL_DEL, it->second.fd.get(), nullptr);
    connections.erase(it);
    --commands_.clients;
  };
  while (!stopping()) {
    const int count =
        ::epoll_wait(epoll.get(), events.data(), static_cast<int>(events.size()), 100);
    if (count < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("epoll_wait failed");
    }
    for (int i = 0; i < count && !stopping(); ++i) {
      const auto id = events[static_cast<std::size_t>(i)].data.u64;
      const auto flags = events[static_cast<std::size_t>(i)].events;
      if (!id) {
        for (int accepted = 0; accepted < 128; ++accepted) {
          UniqueFd fd(::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
          if (!fd) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) log("warn", "accept failed");
            break;
          }
          if (connections.size() >= config_.maxclients) {
            const std::string error = "-ERR max number of clients reached\r\n";
            ::send(fd.get(), error.data(), error.size(), MSG_NOSIGNAL);
            continue;
          }
          tcp_nodelay(fd.get());
          epoll_event event{};
          event.events = EPOLLIN | EPOLLRDHUP;
          event.data.u64 = next_id;
          if (::epoll_ctl(epoll.get(), EPOLL_CTL_ADD, fd.get(), &event) < 0) continue;
          connections.try_emplace(next_id++, std::move(fd), config_.parser_limits, clock_.now_ms());
          ++commands_.clients;
        }
        continue;
      }
      const auto it = connections.find(id);
      if (it == connections.end()) continue;
      auto& c = it->second;
      bool alive = !(flags & EPOLLERR);
      if (alive && (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))) alive = read(c);
      if (alive) alive = write(c);
      if (c.read_eof && !c.pending() && !c.input.empty())
        alive = false;  // Incomplete final request.
      if (!alive) {
        close(id);
        continue;
      }
      epoll_event event{};
      event.data.u64 = id;
      event.events = EPOLLRDHUP;
      if (!c.closing && !c.read_eof && c.pending() < config_.output_limit) event.events |= EPOLLIN;
      if (c.pending()) event.events |= EPOLLOUT;
      if (::epoll_ctl(epoll.get(), EPOLL_CTL_MOD, c.fd.get(), &event) < 0) close(id);
    }
    if (clock_.now_ms() - last_tick >= 100) {
      commands_.tick();
      last_tick = clock_.now_ms();
      std::vector<std::uint64_t> dead;
      for (const auto& pair : connections)
        if (timed_out(pair.second)) dead.push_back(pair.first);
      for (const auto id : dead) close(id);
    }
  }
  commands_.clients -= connections.size();
}
void Server::run_thread_pool() {
  std::atomic<bool> stop{false};
  std::mutex registry_mutex;
  struct Slot {
    std::shared_ptr<Connection> connection;
    bool busy{};
  };
  std::unordered_map<std::uint64_t, Slot> registry;
  UniqueFd wakeup(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!wakeup) throw std::runtime_error("eventfd failed");
  ThreadPool pool(config_.workers);
  std::uint64_t next_id = 1;
  struct Cleanup {
    std::function<void()> fn;
    ~Cleanup() { fn(); }
  } cleanup{[&] {
    stop = true;
    {
      const std::lock_guard<std::mutex> lock(registry_mutex);
      for (const auto& pair : registry) ::shutdown(pair.second.connection->fd.get(), SHUT_RDWR);
    }
    pool.stop();
  }};
  auto last_tick = clock_.now_ms();
  while (!stopping()) {
    std::vector<pollfd> polls{{listener_.get(), POLLIN, 0}, {wakeup.get(), POLLIN, 0}};
    std::vector<std::pair<std::uint64_t, std::shared_ptr<Connection>>> idle;
    {
      const std::lock_guard<std::mutex> lock(registry_mutex);
      for (auto it = registry.begin(); it != registry.end();) {
        auto& slot = it->second;
        if (slot.busy) {
          ++it;
          continue;
        }
        if (timed_out(*slot.connection)) {
          it = registry.erase(it);
          --commands_.clients;
          continue;
        }
        const auto& c = slot.connection;
        short interest = c->pending() ? POLLOUT : 0;
        if (!c->closing && !c->read_eof && c->pending() < config_.output_limit) interest |= POLLIN;
        polls.push_back({c->fd.get(), interest, 0});
        idle.emplace_back(it->first, c);
        ++it;
      }
    }
    const int ready = ::poll(polls.data(), polls.size(), 100);
    if (ready < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("poll failed");
    }
    if (polls[1].revents & POLLIN) {
      std::uint64_t value{};
      while (::read(wakeup.get(), &value, sizeof(value)) > 0) {
      }
    }
    if (polls[0].revents & POLLIN) {
      for (int accepted = 0; accepted < 128; ++accepted) {
        UniqueFd fd(::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (!fd) {
          if (errno == EINTR) continue;
          break;
        }
        if (commands_.clients.load() >= config_.maxclients) {
          const std::string error = "-ERR max number of clients reached\r\n";
          ::send(fd.get(), error.data(), error.size(), MSG_NOSIGNAL);
        } else {
          tcp_nodelay(fd.get());
          auto c =
              std::make_shared<Connection>(std::move(fd), config_.parser_limits, clock_.now_ms());
          const std::lock_guard<std::mutex> lock(registry_mutex);
          registry.emplace(next_id++, Slot{std::move(c), false});
          ++commands_.clients;
        }
      }
    }
    for (std::size_t i = 0; i < idle.size(); ++i) {
      const auto flags = polls[i + 2].revents;
      if (!flags) continue;
      const auto id = idle[i].first;
      const auto c = idle[i].second;
      {
        const std::lock_guard<std::mutex> lock(registry_mutex);
        registry.at(id).busy = true;
      }
      pool.submit([&, id, c, flags] {
        bool alive = !stop.load() && !(flags & (POLLERR | POLLNVAL));
        try {
          if (alive && (flags & (POLLIN | POLLHUP))) alive = read(*c);
          if (alive) alive = write(*c);
          if (c->read_eof && !c->pending()) alive = false;
        } catch (const std::exception& e) {
          log("error", e.what());
          alive = false;
        }
        {
          const std::lock_guard<std::mutex> lock(registry_mutex);
          if (alive)
            registry.at(id).busy = false;
          else {
            registry.erase(id);
            --commands_.clients;
          }
        }
        const std::uint64_t one = 1;
        const auto ignored = ::write(wakeup.get(), &one, sizeof(one));
        (void)ignored;
      });
    }
    if (clock_.now_ms() - last_tick >= 100) {
      commands_.tick();
      last_tick = clock_.now_ms();
    }
  }
}
}  // namespace mr
