#pragma once
#include <csignal>

#include "commands/command_table.hpp"
#include "config/config.hpp"
#include "core/clock.hpp"
#include "net/connection.hpp"
#include "net/socket_util.hpp"
namespace mr {
extern volatile std::sig_atomic_t interrupted;
class Server {
 public:
  Server(Config config, IClock& clock, CommandTable& commands);
  void run();

 private:
  void run_epoll();
  void run_thread_pool();
  bool read(Connection& c);
  bool write(Connection& c);
  void process(Connection& c);
  bool timed_out(const Connection& c) const;
  bool stopping() const;
  Config config_;
  IClock& clock_;
  CommandTable& commands_;
  UniqueFd listener_;
};
}  // namespace mr
