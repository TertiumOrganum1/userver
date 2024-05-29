#pragma once

#include <memory>
#include <string>

#include <userver/utest/utest.hpp>
#include <userver/utils/regex.hpp>

#include <userver/storages/redis/impl/thread_pools.hpp>

#include <storages/redis/client_impl.hpp>
#include <storages/redis/dynamic_config.hpp>
#include <storages/redis/impl/sentinel.hpp>
#include <storages/redis/impl/subscribe_sentinel.hpp>
#include <storages/redis/subscribe_client_impl.hpp>
#include <storages/redis/util_redistest.hpp>
#include <userver/dynamic_config/storage_mock.hpp>
#include <userver/dynamic_config/test_helpers.hpp>
#include "storages/redis/impl/keyshard_impl.hpp"

USERVER_NAMESPACE_BEGIN

// This fixture is used for both Client and SubscribeClient tests, because
// Publish() method is part of Client - not SubscribeClient. And without
// Publish() there is no way to write proper unit tests for subscribing
class RedisClusterClientTest : public ::testing::Test {
 public:
  struct Version {
    int major{};
    int minor{};
    int patch{};
  };

  static void SetUpTestSuite() {
    static auto config_mock = MakeDynamicConfigStorage();
    auto configs_source = config_mock.GetSource();
    thread_pools_ = std::make_shared<redis::ThreadPools>(
        redis::kDefaultSentinelThreadPoolSize,
        redis::kDefaultRedisThreadPoolSize);
    sentinel_ = redis::Sentinel::CreateSentinel(
        thread_pools_, GetTestsuiteRedisClusterSettings(), "none",
        configs_source, "pub", redis::KeyShardFactory{redis::kRedisCluster});
    sentinel_->WaitConnectedDebug();

    subscribe_sentinel_ = redis::SubscribeSentinel::Create(
        thread_pools_, GetTestsuiteRedisClusterSettings(), "none",
        configs_source, "pub", true, {}, {});
    subscribe_sentinel_->WaitConnectedDebug();

    auto info_reply =
        sentinel_->MakeRequest({"info", "server"}, "none", false).Get();
    ASSERT_TRUE(info_reply->IsOk());
    ASSERT_TRUE(info_reply->data.IsString());
    const auto info = info_reply->data.GetString();

    utils::regex redis_version_regex(R"(redis_version:(\d+.\d+.\d+))");
    utils::smatch redis_version_matches;
    ASSERT_TRUE(
        utils::regex_search(info, redis_version_matches, redis_version_regex));
    version_ = MakeVersion(redis_version_matches[1]);
  }

  static void TearDownTestSuite() {
    subscribe_sentinel_.reset();
    sentinel_.reset();
    thread_pools_.reset();
    version_ = {};
  }

  void SetUp() override {
    for (size_t shard = 0; shard < 3; ++shard) {
      sentinel_->MakeRequest({"flushdb"}, shard, true).Get();
    }

    client_ = std::make_shared<storages::redis::ClientImpl>(sentinel_);

    /* Subscribe client */
    subscribe_client_ = std::make_shared<storages::redis::SubscribeClientImpl>(
        subscribe_sentinel_);
  }

  static std::shared_ptr<redis::Sentinel> GetSentinel() { return sentinel_; }

  storages::redis::ClientPtr GetClient() { return client_; }
  std::shared_ptr<storages::redis::SubscribeClient> GetSubscribeClient() {
    return subscribe_client_;
  }

  static bool CheckVersion(Version since) {
    return std::tie(since.major, since.minor, since.patch) <=
           std::tie(version_.major, version_.minor, version_.patch);
  }

  static std::string SkipMsgByVersion(std::string_view command,
                                      Version version) {
    return fmt::format("{} command available since {}.{}.{}", command,
                       version.major, version.minor, version.patch);
  }

 private:
  static std::shared_ptr<redis::ThreadPools> thread_pools_;
  static std::shared_ptr<redis::Sentinel> sentinel_;
  static std::shared_ptr<redis::SubscribeSentinel> subscribe_sentinel_;
  static Version version_;
  storages::redis::ClientPtr client_{};
  std::shared_ptr<storages::redis::SubscribeClient> subscribe_client_{};

  static Version MakeVersion(std::string from) {
    utils::regex rgx(R"((\d+).(\d+).(\d+))");
    utils::smatch matches;
    const auto result = utils::regex_search(from, matches, rgx);
    EXPECT_TRUE(result);
    if (!result) return {};
    const std::string major{matches[1]};
    const std::string minor{matches[2]};
    const std::string patch{matches[3]};
    return {std::stoi(major), std::stoi(minor), std::stoi(patch)};
  }

  static dynamic_config::StorageMock MakeDynamicConfigStorage() {
    auto docs_map = dynamic_config::impl::GetDefaultDocsMap();
    docs_map.Set("REDIS_REPLICA_MONITORING_SETTINGS",
                 formats::json::FromString(R"(
      {
        "__default__": {
          "enable-monitoring": true,
          "forbid-requests-to-syncing-replicas": true
        }
      }
    )"));
    return dynamic_config::StorageMock(docs_map, {});
  }
};

inline std::shared_ptr<redis::ThreadPools>
    RedisClusterClientTest::thread_pools_;
inline std::shared_ptr<redis::Sentinel> RedisClusterClientTest::sentinel_;
inline std::shared_ptr<redis::SubscribeSentinel>
    RedisClusterClientTest::subscribe_sentinel_;
inline RedisClusterClientTest::Version RedisClusterClientTest::version_;

USERVER_NAMESPACE_END
