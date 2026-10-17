#include <unistd.h>

#include <csignal>
#include <exception>
#include <iostream>

#include "commands/command_table.hpp"
#include "config/config.hpp"
#include "core/clock.hpp"
#include "net/server.hpp"
#include "protocol/cli_formatter.hpp"
#include "util/logger.hpp"
#include "util/string_utils.hpp"
namespace {
void signal_handler(int) { mr::interrupted = 1; }
}  // namespace
int main(int argc, char** argv) {
  try {
    auto config = mr::parse_config(argc, argv);
    if (config.help) {
      std::cout << mr::config_help();
      return 0;
    }
    struct sigaction action{};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    mr::SystemClock clock;
    mr::CommandTable commands(config, clock);
    if (config.cli) {
      const bool interactive = ::isatty(STDIN_FILENO);
      std::string line;
      while (!mr::interrupted && !commands.shutdown_requested()) {
        if (interactive) std::cout << "mini-redis> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        std::vector<std::string> args;
        std::string error;
        if (!mr::split_inline(line, args, error)) {
          std::cout << "(error) " << error << '\n';
          continue;
        }
        if (args.empty()) continue;
        const auto name = mr::upper(args[0]);
        if (name == "EXIT" || name == "QUIT") break;
        std::cout << mr::format_cli(commands.execute(args)) << '\n';
        commands.tick();
      }
      commands.flush();
    } else {
      mr::Server server(config, clock, commands);
      server.run();
    }
    return 0;
  } catch (const std::exception& e) {
    mr::log("error", e.what());
    return 1;
  }
}
