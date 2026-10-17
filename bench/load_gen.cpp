#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "net/socket_util.hpp"
#include "protocol/resp_writer.hpp"
namespace {
class Client {
 public:
  Client(const std::string& host, std::uint16_t port) : fd_(mr::connect_tcp(host, port)) {
    timeval timeout{30, 0};
    ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd_.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  }
  void send(const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto n = ::send(fd_.get(), bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
      if (n <= 0) throw std::runtime_error("send failed");
      offset += static_cast<std::size_t>(n);
    }
  }
  void reply() {
    auto line = [&] {
      for (;;) {
        const auto end = buffer_.find("\r\n");
        if (end != std::string::npos) {
          auto out = buffer_.substr(0, end);
          buffer_.erase(0, end + 2);
          return out;
        }
        read();
      }
    };
    const auto head = line();
    if (head.empty() || head.front() == '-') throw std::runtime_error("server error: " + head);
    if (head.front() == '$') {
      const auto len = std::stoll(head.substr(1));
      if (len < 0) return;
      while (buffer_.size() < static_cast<std::size_t>(len) + 2) read();
      buffer_.erase(0, static_cast<std::size_t>(len) + 2);
    } else if (head.front() != '+' && head.front() != ':')
      throw std::runtime_error("unexpected reply");
  }

 private:
  void read() {
    char data[16384];
    const auto n = ::recv(fd_.get(), data, sizeof(data), 0);
    if (n <= 0) throw std::runtime_error("receive failed or timed out");
    buffer_.append(data, static_cast<std::size_t>(n));
  }
  mr::UniqueFd fd_;
  std::string buffer_;
};
}  // namespace
int main(int argc, char** argv) {
  try {
    std::string host = "127.0.0.1", workload = "mixed";
    std::size_t port = 6380, requests = 100000, clients = 50, pipeline = 1;
    for (int i = 1; i < argc; i += 2) {
      if (i + 1 == argc) throw std::runtime_error("options require values");
      const std::string option(argv[i]), value(argv[i + 1]);
      if (option == "--host")
        host = value;
      else if (option == "--workload")
        workload = value;
      else if (option == "--port")
        port = std::stoull(value);
      else if (option == "--requests")
        requests = std::stoull(value);
      else if (option == "--clients")
        clients = std::stoull(value);
      else if (option == "--pipeline")
        pipeline = std::stoull(value);
      else
        throw std::runtime_error("unknown option: " + option);
    }
    if (!requests || requests > 10000000 || !clients || clients > 1024 || !pipeline ||
        pipeline > 100000 || port > 65535 ||
        (workload != "set" && workload != "get" && workload != "mixed"))
      throw std::runtime_error("invalid load generator options");
    std::vector<std::vector<double>> latencies(clients);
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t ready = 0;
    bool start = false;
    std::atomic<bool> failed{false};
    for (std::size_t id = 0; id < clients; ++id)
      workers.emplace_back([&, id] {
        bool announced = false;
        try {
          Client client(host, static_cast<std::uint16_t>(port));
          const auto key = "bench:" + std::to_string(id);
          client.send(mr::encode_command({"SET", key, "01234567890123456789012345678901"}));
          client.reply();
          {
            std::unique_lock<std::mutex> lock(mutex);
            ++ready;
            announced = true;
            cv.notify_all();
            cv.wait(lock, [&] { return start; });
          }
          const auto count = requests / clients + (id < requests % clients ? 1 : 0);
          latencies[id].reserve(count);
          for (std::size_t done = 0; done < count;) {
            const auto batch = std::min(pipeline, count - done);
            std::string bytes;
            for (std::size_t j = 0; j < batch; ++j) {
              const bool set = workload == "set" || (workload == "mixed" && (done + j) % 2 == 0);
              bytes += set ? mr::encode_command({"SET", key, "01234567890123456789012345678901"})
                           : mr::encode_command({"GET", key});
            }
            const auto begin = std::chrono::steady_clock::now();
            client.send(bytes);
            for (std::size_t j = 0; j < batch; ++j) {
              client.reply();
              latencies[id].push_back(std::chrono::duration<double, std::micro>(
                                          std::chrono::steady_clock::now() - begin)
                                          .count());
            }
            done += batch;
          }
        } catch (const std::exception& e) {
          failed = true;
          const std::lock_guard<std::mutex> lock(mutex);
          std::cerr << e.what() << '\n';
          if (!announced) ++ready;
          cv.notify_all();
        }
      });
    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [&] { return ready == clients; });
    }
    const auto begin = std::chrono::steady_clock::now();
    {
      const std::lock_guard<std::mutex> lock(mutex);
      start = true;
    }
    cv.notify_all();
    for (auto& worker : workers) worker.join();
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    if (failed) return 1;
    std::vector<double> all;
    all.reserve(requests);
    for (const auto& samples : latencies) all.insert(all.end(), samples.begin(), samples.end());
    std::sort(all.begin(), all.end());
    auto percentile = [&](double p) {
      return all[static_cast<std::size_t>(std::ceil(p * static_cast<double>(all.size()))) - 1];
    };
    std::cout << "{\"requests\":" << all.size() << ",\"clients\":" << clients
              << ",\"pipeline\":" << pipeline << ",\"workload\":\"" << workload
              << "\",\"seconds\":" << seconds
              << ",\"ops_per_sec\":" << static_cast<double>(all.size()) / seconds
              << ",\"p50_us\":" << percentile(.5) << ",\"p95_us\":" << percentile(.95)
              << ",\"p99_us\":" << percentile(.99) << ",\"p999_us\":" << percentile(.999) << "}\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
