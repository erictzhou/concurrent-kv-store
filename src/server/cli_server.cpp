#include "server/cli_server.h"

#include "common/version.h"

#include <exception>
#include <iostream>
#include <string>

namespace kv {
namespace server {

CliServer::CliServer(parser::CliParser& parser, store::KVStore& store)
  : parser_(parser), store_(store) {}

void CliServer::Run(std::istream& input, std::ostream& output) {
  bool running = true;
  std::string line;

  while (running) {
    // Flush the prompt immediately so interactive users see it before input is
    // read.
    output << "kv-store> ";
    output.flush();

    if (!std::getline(input, line)) {
      // Treat EOF as a clean shutdown path for piped input and terminal Ctrl-D.
      output << '\n';
      break;
    }

    const parser::Command command = parser_.Parse(line);
    Execute(command, output, running);
  }
}

void CliServer::Execute(const parser::Command& command, std::ostream& output, bool& running) {
  if (!command.IsValid()) {
    // Parser errors are reported as normal command output instead of
    // exceptions, keeping the loop alive for the next command.
    output << "ERR " << command.error_message << '\n';
    return;
  }

  try {
    // Keep command dispatch thin: parsing already validated arity, and storage
    // owns persistence side effects.
    switch (command.type) {
      case parser::CommandType::kSet:
        store_.Set(command.key, command.value);
        output << "OK\n";
        break;
      case parser::CommandType::kGet: {
        const auto value = store_.Get(command.key);
        output << (value.has_value() ? *value : "(nil)") << '\n';
        break;
      }
      case parser::CommandType::kDel:
        output << (store_.Delete(command.key) ? "1" : "0") << '\n';
        break;
      case parser::CommandType::kClearPersistence:
        store_.ClearPersistence();
        output << "OK persistence cleared\n";
        break;
      case parser::CommandType::kCompactPersistence:
        if (store_.CompactPersistence()) {
          output << "OK snapshot compacted\n";
        } else {
          output << "ERR snapshot persistence is not configured\n";
        }
        break;
      case parser::CommandType::kInfo:
        PrintInfo(output);
        break;
      case parser::CommandType::kHelp:
        PrintHelp(output);
        break;
      case parser::CommandType::kExit:
        output << "Bye\n";
        running = false;
        break;
      case parser::CommandType::kInvalid:
        // Defensive fallback in case an invalid command bypasses the guard
        // above.
        output << "ERR invalid command\n";
        break;
    }
  } catch (const std::exception& error) {
    // Surface storage or WAL failures to the CLI without terminating the
    // process.
    output << "ERR " << error.what() << '\n';
  }
}

void CliServer::PrintHelp(std::ostream& output) const {
  output << "Concurrent KV Store v" << common::kProjectVersion << '\n';
  output << "Commands:\n";
  output << "  SET <key> <value>\n";
  output << "  GET <key>\n";
  output << "  DEL|DELETE <key>\n";
  output << "  COMPACT|SNAPSHOT\n";
  output << "  CLEAR PERSISTENCE\n";
  output << "  INFO|VERSION|STATUS\n";
  output << "  HELP\n";
  output << "  EXIT\n";
  output << "Concurrency: " << common::kConcurrencyModel << '\n';
}

void CliServer::PrintInfo(std::ostream& output) const {
  output << "Concurrent KV Store v" << common::kProjectVersion << '\n';
  output << "entries: " << store_.Size() << '\n';
  output << "concurrency: " << common::kConcurrencyModel << '\n';
  output << "durability: default WAL is buffered (not crash-durable); "
            "Sync and GroupCommit require fdatasync before acknowledgement\n";
}

}  // namespace server
}  // namespace kv
