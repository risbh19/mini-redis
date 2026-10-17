#include <gtest/gtest.h>

#include "persistence/aof.hpp"
#include "persistence/crc32.hpp"
#include "persistence/snapshot.hpp"
#include "protocol/resp_writer.hpp"
#include "test_util.hpp"
TEST(Crc32, StandardVector) { EXPECT_EQ(mr::crc32("123456789"), 0xCBF43926U); }
TEST(Snapshot, BinaryRoundtripAndExpiry) {
  TempDir dir;
  mr::FakeClock clock;
  mr::Store store(clock), restored(clock);
  store.set("k", std::string("a\0b", 3), 100);
  store.set("plain", "v");
  mr::save_snapshot(dir.file("dump"), store);
  mr::load_snapshot(dir.file("dump"), restored);
  EXPECT_EQ(restored.get("k"), std::string("a\0b", 3));
  EXPECT_EQ(restored.pttl("k"), 100);
  EXPECT_EQ(restored.size(), 2U);
}
TEST(Snapshot, RejectsCorruptionAndEveryTruncation) {
  auto bytes = mr::encode_snapshot({{"k", "v", {}}});
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    EXPECT_THROW(mr::decode_snapshot(bytes.substr(0, i)), std::runtime_error);
    auto corrupt = bytes;
    corrupt[i] ^= 1;
    EXPECT_THROW(mr::decode_snapshot(corrupt), std::runtime_error);
  }
}
TEST(Snapshot, InvalidLoadLeavesOriginalStoreIntact) {
  TempDir dir;
  mr::FakeClock clock;
  mr::Store store(clock);
  store.set("safe", "yes");
  mr::atomic_file(dir.file("dump"), "broken");
  EXPECT_THROW(mr::load_snapshot(dir.file("dump"), store), std::runtime_error);
  EXPECT_EQ(store.get("safe"), "yes");
}
TEST(Aof, RoundtripAndRewriteCompact) {
  TempDir dir;
  mr::FakeClock clock;
  mr::Store store(clock);
  store.set("k", "v", 100);
  mr::Aof aof(dir.file("log"), mr::FsyncPolicy::Always, clock);
  aof.append(mr::encode_command({"SET", "old", "v"}));
  aof.rewrite(store.records());
  std::vector<std::vector<std::string>> commands;
  mr::replay_aof(dir.file("log"), [&](const auto& a) { commands.push_back(a); });
  ASSERT_EQ(commands.size(), 2U);
  EXPECT_EQ(commands[0], (std::vector<std::string>{"SET", "k", "v"}));
  EXPECT_EQ(commands[1][0], "PEXPIREAT");
}
TEST(Aof, EverySecondSyncScheduleWithInjectedSyncer) {
  TempDir dir;
  mr::FakeClock clock;
  int calls = 0;
  mr::Aof aof(dir.file("log"), mr::FsyncPolicy::EverySecond, clock, [&](int) { ++calls; });
  aof.append("x");
  clock.advance(999);
  aof.tick();
  EXPECT_EQ(calls, 0);
  clock.advance(1);
  aof.tick();
  EXPECT_EQ(calls, 1);
  aof.tick();
  EXPECT_EQ(calls, 1);
  aof.append("y");
  clock.advance(1000);
  aof.tick();
  EXPECT_EQ(calls, 2);
}
TEST(Aof, AlwaysAndNoSyncPolicies) {
  TempDir dir;
  mr::FakeClock clock;
  int calls = 0;
  {
    mr::Aof aof(dir.file("always"), mr::FsyncPolicy::Always, clock, [&](int) { ++calls; });
    aof.append("x");
    aof.append("y");
    EXPECT_EQ(calls, 2);
  }
  {
    mr::Aof aof(dir.file("no"), mr::FsyncPolicy::No, clock, [&](int) { ++calls; });
    aof.append("x");
    clock.advance(10000);
    aof.tick();
    EXPECT_EQ(calls, 2);
    aof.flush();
    EXPECT_EQ(calls, 3);
  }
}
TEST(Aof, TruncatedTailRecoversOnlyCommittedTransaction) {
  TempDir dir;
  const auto first = mr::encode_command({"SET", "old", "v"});
  const auto transaction = mr::encode_command({"MULTI"}) + mr::encode_command({"SET", "a", "1"}) +
                           mr::encode_command({"SET", "b", "2"}) + mr::encode_command({"EXEC"});
  for (std::size_t split = 1; split < transaction.size(); ++split) {
    mr::atomic_file(dir.file("log"), first + transaction.substr(0, split));
    std::size_t seen = 0;
    const auto result = mr::replay_aof(dir.file("log"), [&](const auto&) { ++seen; });
    EXPECT_EQ(seen, 1U) << split;
    EXPECT_EQ(result.recovered_bytes, split) << split;
    EXPECT_EQ(mr::read_file(dir.file("log")), first);
  }
}
TEST(Aof, CompleteTransactionExecutesAndCorruptionRejects) {
  TempDir dir;
  mr::atomic_file(dir.file("log"), mr::encode_command({"MULTI"}) +
                                       mr::encode_command({"SET", "a", "1"}) +
                                       mr::encode_command({"EXEC"}));
  std::size_t seen = 0;
  mr::replay_aof(dir.file("log"), [&](const auto&) { ++seen; });
  EXPECT_EQ(seen, 1U);
  mr::atomic_file(dir.file("log"), "*wat\r\n");
  EXPECT_THROW(mr::replay_aof(dir.file("log"), [](const auto&) {}), std::runtime_error);
}
