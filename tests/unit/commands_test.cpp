#include <gtest/gtest.h>

#include <limits>

#include "commands/command_table.hpp"
#include "protocol/resp_writer.hpp"
#include "test_util.hpp"
class CommandsTest : public testing::Test {
 protected:
  TempDir dir;
  mr::FakeClock clock;
  mr::Config config() {
    mr::Config c;
    c.dir = dir.path;
    c.appendonly = false;
    return c;
  }
  std::unique_ptr<mr::CommandTable> commands = std::make_unique<mr::CommandTable>(config(), clock);
  std::string run(std::vector<std::string> a) { return mr::resp_encode(commands->execute(a)); }
};
TEST_F(CommandsTest, CaseInsensitiveAndArityErrors) {
  EXPECT_EQ(run({"ping"}), "+PONG\r\n");
  EXPECT_EQ(run({"SET", "k"}), "-ERR wrong number of arguments for 'set' command\r\n");
  EXPECT_EQ(run({"fake"}), "-ERR unknown command 'fake'\r\n");
}
TEST_F(CommandsTest, SetOptionsAndTtlRounding) {
  EXPECT_EQ(run({"SET", "k", "v", "PX", "1500", "NX"}), "+OK\r\n");
  EXPECT_EQ(run({"TTL", "k"}), ":2\r\n");
  EXPECT_EQ(run({"SET", "k", "x", "NX"}), "$-1\r\n");
  clock.advance(501);
  EXPECT_EQ(run({"TTL", "k"}), ":1\r\n");
  EXPECT_EQ(run({"PTTL", "k"}), ":999\r\n");
  EXPECT_EQ(run({"SET", "k", "x", "NX", "XX"}), "-ERR syntax error\r\n");
  EXPECT_TRUE(run({"SET", "k", "x", "EX", "0"}).find("-ERR") == 0);
}
TEST_F(CommandsTest, TtlCodesAndPersist) {
  EXPECT_EQ(run({"TTL", "missing"}), ":-2\r\n");
  run({"SET", "k", "v"});
  EXPECT_EQ(run({"TTL", "k"}), ":-1\r\n");
  EXPECT_EQ(run({"EXPIRE", "k", "1"}), ":1\r\n");
  EXPECT_EQ(run({"PERSIST", "k"}), ":1\r\n");
}
TEST_F(CommandsTest, CountersOverflowAndPreserveExpiry) {
  run({"SET", "k", "9223372036854775807"});
  EXPECT_TRUE(run({"INCR", "k"}).find("overflow") != std::string::npos);
  EXPECT_EQ(run({"GET", "k"}), "$19\r\n9223372036854775807\r\n");
  run({"SET", "k", "-9223372036854775808"});
  EXPECT_TRUE(run({"DECR", "k"}).find("overflow") != std::string::npos);
  run({"SET", "k", "0", "PX", "100"});
  EXPECT_EQ(run({"DECRBY", "k", "-9223372036854775808"}).substr(0, 4), "-ERR");
  EXPECT_EQ(run({"INCRBY", "k", "2"}), ":2\r\n");
  EXPECT_EQ(run({"PTTL", "k"}), ":100\r\n");
}
TEST_F(CommandsTest, RejectsNonIntegerAndOverflowingExpiry) {
  run({"SET", "k", "x"});
  EXPECT_EQ(run({"INCR", "k"}), "-ERR value is not an integer or out of range\r\n");
  EXPECT_TRUE(run({"SET", "a", "b", "PX", "9223372036854775807"}).find("-ERR") == 0);
  EXPECT_EQ(run({"GET", "a"}), "$-1\r\n");
}
TEST_F(CommandsTest, MultiKeySemantics) {
  EXPECT_EQ(run({"MSET", "a", "1", "b", "2"}), "+OK\r\n");
  EXPECT_EQ(run({"EXISTS", "a", "a", "missing"}), ":2\r\n");
  EXPECT_EQ(run({"DEL", "a", "a", "b"}), ":2\r\n");
  EXPECT_EQ(run({"MGET", "a", "b"}), "*2\r\n$-1\r\n$-1\r\n");
}
TEST_F(CommandsTest, AppendPreservesTtlGetsetClearsIt) {
  run({"SET", "a", "x", "PX", "100"});
  EXPECT_EQ(run({"APPEND", "a", "y"}), ":2\r\n");
  EXPECT_EQ(run({"PTTL", "a"}), ":100\r\n");
  EXPECT_EQ(run({"GETSET", "a", "z"}), "$2\r\nxy\r\n");
  EXPECT_EQ(run({"PTTL", "a"}), ":-1\r\n");
}
TEST_F(CommandsTest, ConfigAndOom) {
  EXPECT_EQ(run({"CONFIG", "SET", "maxmemory", "162"}), "+OK\r\n");
  run({"SET", "a", "1"});
  EXPECT_TRUE(run({"SET", "b", "2"}).find("-OOM") == 0);
  EXPECT_TRUE(run({"CONFIG", "GET", "maxmemory"}).find("162") != std::string::npos);
  EXPECT_TRUE(run({"CONFIG", "SET", "port", "4"}).find("immutable") != std::string::npos);
}
TEST_F(CommandsTest, DirectoryLockPreventsConcurrentWriters) {
  EXPECT_THROW(mr::CommandTable other(config(), clock), std::runtime_error);
}
TEST(CommandPersistence, RestartAbsoluteExpiryAndAofPrecedence) {
  TempDir dir;
  mr::FakeClock clock;
  mr::Config c;
  c.dir = dir.path;
  c.appendfsync = mr::FsyncPolicy::Always;
  {
    mr::CommandTable first(c, clock);
    first.execute({"SET", "k", "old", "PX", "1000"});
    first.execute({"SAVE"});
    first.execute({"SET", "k", "new", "PX", "1000"});
  }
  clock.advance(400);
  {
    mr::CommandTable second(c, clock);
    EXPECT_EQ(mr::resp_encode(second.execute({"GET", "k"})), "$3\r\nnew\r\n");
    EXPECT_EQ(mr::resp_encode(second.execute({"PTTL", "k"})), ":600\r\n");
  }
  clock.advance(601);
  {
    mr::CommandTable third(c, clock);
    EXPECT_EQ(mr::resp_encode(third.execute({"GET", "k"})), "$-1\r\n");
  }
}
TEST(CommandPersistence, EvictionAndRewriteSurviveRestart) {
  TempDir dir;
  mr::FakeClock clock;
  mr::Config c;
  c.dir = dir.path;
  c.maxmemory = 324;
  c.memory_policy = mr::EvictionPolicy::AllKeysLru;
  {
    mr::CommandTable first(c, clock);
    first.execute({"SET", "a", "1"});
    first.execute({"SET", "b", "2"});
    first.execute({"GET", "a"});
    first.execute({"SET", "c", "3"});
    first.execute({"BGREWRITEAOF"});
    first.execute({"SET", "d", "4"});
  }
  {
    mr::CommandTable next(c, clock);
    EXPECT_EQ(mr::resp_encode(next.execute({"GET", "b"})), "$-1\r\n");
    EXPECT_EQ(mr::resp_encode(next.execute({"GET", "d"})), "$1\r\n4\r\n");
    EXPECT_EQ(mr::resp_encode(next.execute({"DBSIZE"})), ":2\r\n");
  }
}
TEST(CommandPersistence, RewriteFailureDisablesSubsequentWrites) {
  TempDir dir;
  mr::FakeClock clock;
  mr::Config config;
  config.dir = dir.path;
  mr::CommandTable commands(config, clock);
  EXPECT_FALSE(commands.execute({"SET", "k", "before"}).is_error());
  std::filesystem::create_directory(dir.file("appendonly.aof.tmp"));
  EXPECT_TRUE(commands.execute({"BGREWRITEAOF"}).is_error());
  const auto reply = commands.execute({"SET", "k", "after"});
  ASSERT_TRUE(reply.is_error());
  EXPECT_NE(std::get<mr::Error>(reply.value).value.find("MISCONF"), std::string::npos);
  EXPECT_EQ(mr::resp_encode(commands.execute({"GET", "k"})), "$6\r\nbefore\r\n");
}
