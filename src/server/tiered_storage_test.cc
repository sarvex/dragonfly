// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include <absl/strings/str_cat.h>
#include <gmock/gmock.h>

#include "base/flags.h"
#include "base/logging.h"
#include "server/test_utils.h"

using namespace std;
using namespace testing;
using absl::SetFlag;
using absl::StrCat;

ABSL_DECLARE_FLAG(string, spill_file_prefix);

namespace dfly {

class TieredStorageTest : public BaseFamilyTest {
 protected:
  TieredStorageTest() : BaseFamilyTest() {
    num_threads_ = 1;
  }

  void FillExternalKeys(unsigned count, int val_size = 256);

  static void SetUpTestSuite();
};

void TieredStorageTest::SetUpTestSuite() {
  BaseFamilyTest::SetUpTestSuite();
  SetFlag(&FLAGS_spill_file_prefix, "/tmp/spill");

  auto* force_epoll = absl::FindCommandLineFlag("force_epoll");
  if (force_epoll->CurrentValue() == "true") {
    LOG(WARNING) << "Tiered storage only works with io uring, skipping tests.";
    // Exiting directly, as otherwise EngineShardSet will exit with error status.
    exit(0);
  }
}

void TieredStorageTest::FillExternalKeys(unsigned count, int val_size) {
  string val(val_size, 'a');

  unsigned batch_cnt = count / 50;
  for (unsigned i = 0; i < batch_cnt; ++i) {
    vector<string> cmd;
    cmd.push_back("mset");

    for (unsigned j = 0; j < 50; ++j) {
      string key = StrCat("k", i * 50 + j);
      cmd.push_back(key);
      cmd.push_back(val);
    }
    Run(absl::Span<string>{cmd});
  }

  for (unsigned i = batch_cnt * 50; i < count; ++i) {
    Run({"set", StrCat("k", i), val});
  }
}

TEST_F(TieredStorageTest, Basic) {
  FillExternalKeys(5000);

  EXPECT_EQ(5000, CheckedInt({"dbsize"}));
  Metrics m = GetMetrics();
  EXPECT_GT(m.db_stats[0].tiered_entries, 0u);

  FillExternalKeys(5000);
  usleep(20000);  // 0.02 milliseconds

  m = GetMetrics();
  DbStats stats = m.db_stats[0];

  LOG(INFO) << stats;
  unsigned tiered_entries = m.db_stats[0].tiered_entries;
  EXPECT_GT(tiered_entries, 0u);
  string resp = CheckedString({"debug", "object", "k1"});
  EXPECT_THAT(resp, HasSubstr("spill_len"));
  m = GetMetrics();
  LOG(INFO) << m.db_stats[0];
  ASSERT_EQ(tiered_entries, m.db_stats[0].tiered_entries);

  Run({"del", "k1"});
  m = GetMetrics();
  EXPECT_EQ(m.db_stats[0].tiered_entries, tiered_entries - 1);
}

TEST_F(TieredStorageTest, DelBeforeOffload) {
  FillExternalKeys(100);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));

  usleep(20000);  // 0.02 milliseconds
  Metrics m = GetMetrics();
  EXPECT_GT(m.db_stats[0].tiered_entries, 0u);
  EXPECT_LT(m.db_stats[0].tiered_entries, 100);

  for (unsigned i = 0; i < 100; ++i) {
    Run({"del", StrCat("k", i)});
  }
  m = GetMetrics();
  EXPECT_EQ(m.db_stats[0].tiered_entries, 0u);

  FillExternalKeys(100);
  usleep(20000);  // 0.02 milliseconds
  m = GetMetrics();
  EXPECT_GT(m.db_stats[0].tiered_entries, 0u);
  EXPECT_LT(m.db_stats[0].tiered_entries, 100);
}

TEST_F(TieredStorageTest, AddMultiDb) {
  Run({"select", "1"});
  FillExternalKeys(100);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));
  Run({"select", "5"});
  FillExternalKeys(100);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));

  usleep(20000);  // 0.02 milliseconds

  Metrics m = GetMetrics();
  EXPECT_GT(m.db_stats[1].tiered_entries, 0u);
  EXPECT_LT(m.db_stats[1].tiered_entries, 100);
  EXPECT_GT(m.db_stats[5].tiered_entries, 0u);
  EXPECT_LT(m.db_stats[4].tiered_entries, 100);
}

TEST_F(TieredStorageTest, FlushDBAfterSet) {
  Run({"select", "5"});
  FillExternalKeys(100);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));

  Run({"flushdb"});
  Metrics m = GetMetrics();
  EXPECT_EQ(m.db_stats[5].tiered_entries, 0u);

  FillExternalKeys(100);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));

  usleep(20000);  // 0.02 milliseconds
  m = GetMetrics();
  EXPECT_GT(m.db_stats[5].tiered_entries, 0u);
  EXPECT_LT(m.db_stats[5].tiered_entries, 100);
}

TEST_F(TieredStorageTest, FlushAllAfterSet) {
  Run({"select", "5"});
  FillExternalKeys(100);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));

  Run({"flushall"});
  Metrics m = GetMetrics();
  EXPECT_EQ(m.db_stats[5].tiered_entries, 0u);

  FillExternalKeys(100);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));

  usleep(20000);  // 0.02 milliseconds
  m = GetMetrics();
  EXPECT_GT(m.db_stats[5].tiered_entries, 0u);
  EXPECT_LT(m.db_stats[5].tiered_entries, 100);
}

TEST_F(TieredStorageTest, AddBigValues) {
  FillExternalKeys(100, 5000);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));

  Run({"flushall"});
  Metrics m = GetMetrics();
  EXPECT_EQ(m.db_stats[0].tiered_entries, 0u);

  FillExternalKeys(100, 5000);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));

  usleep(20000);  // 0.02 milliseconds
  m = GetMetrics();
  EXPECT_GT(m.db_stats[0].tiered_entries, 0u);
}

TEST_F(TieredStorageTest, DelBigValues) {
  FillExternalKeys(100, 5000);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));

  for (unsigned i = 0; i < 100; ++i) {
    Run({"del", StrCat("k", i)});
  }
  Metrics m = GetMetrics();
  EXPECT_EQ(m.db_stats[0].tiered_entries, 0u);

  FillExternalKeys(100, 5000);
  EXPECT_EQ(100, CheckedInt({"dbsize"}));

  usleep(20000);  // 0.02 milliseconds
  m = GetMetrics();
  EXPECT_GT(m.db_stats[0].tiered_entries, 0u);
}

}  // namespace dfly
