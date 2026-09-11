#include "parser/cli_parser.h"
#include "server/cli_server.h"
#include "store/kv_store.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

using kv::parser::CliParser;
using kv::parser::CommandType;
using kv::server::CliServer;
using kv::store::KVStore;

TEST(CliParserTest, ParsesInfoVersionAndStatusAliases) {
  const CliParser parser;

  EXPECT_EQ(CommandType::kInfo, parser.Parse("INFO").type);
  EXPECT_EQ(CommandType::kInfo, parser.Parse("version").type);
  EXPECT_EQ(CommandType::kInfo, parser.Parse("Status").type);
}

TEST(CliParserTest, RejectsInfoWithArguments) {
  const CliParser parser;
  const auto command = parser.Parse("INFO extra");

  EXPECT_FALSE(command.IsValid());
  EXPECT_EQ("usage: INFO", command.error_message);
}

TEST(CliServerTest, InfoPrintsVersionEntriesConcurrencyAndDurability) {
  CliParser parser;
  KVStore store;
  store.Set("alpha", "1");
  store.Set("beta", "2");
  CliServer server(parser, store);

  std::istringstream input("INFO\nEXIT\n");
  std::ostringstream output;
  server.Run(input, output);

  const std::string text = output.str();
  EXPECT_NE(std::string::npos, text.find("Concurrent KV Store v0.6.0"));
  EXPECT_NE(std::string::npos, text.find("entries: 2"));
  EXPECT_NE(std::string::npos, text.find("concurrency: 64 shards"));
  EXPECT_NE(std::string::npos, text.find("durability: default WAL is buffered"));
}

TEST(CliServerTest, HelpIncludesCurrentConcurrencyCommands) {
  CliParser parser;
  KVStore store;
  CliServer server(parser, store);

  std::istringstream input("HELP\nEXIT\n");
  std::ostringstream output;
  server.Run(input, output);

  const std::string text = output.str();
  EXPECT_NE(std::string::npos, text.find("Concurrent KV Store v0.6.0"));
  EXPECT_NE(std::string::npos, text.find("INFO|VERSION|STATUS"));
  EXPECT_NE(std::string::npos, text.find("Concurrency: 64 shards"));
  EXPECT_NE(std::string::npos, text.find("COMPACT|SNAPSHOT"));
  EXPECT_NE(std::string::npos, text.find("CLEAR PERSISTENCE"));
}

}  // namespace
