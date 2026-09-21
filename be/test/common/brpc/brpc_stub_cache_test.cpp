// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "common/brpc/brpc_stub_cache.h"

#include <base/testutil/assert.h>
#include <gtest/gtest.h>

#include "base/failpoint/fail_point.h"
#include "common/config_network_fwd.h"

namespace starrocks {

class BrpcStubCacheTest : public testing::Test {
public:
    BrpcStubCacheTest() = default;
    ~BrpcStubCacheTest() override = default;
    void SetUp() override {
        _saved_brpc_max_connections_per_server = config::brpc_max_connections_per_server;
        _saved_brpc_stub_expire_s = config::brpc_stub_expire_s;
        _saved_brpc_unhealthy_stub_expire_s = config::brpc_unhealthy_stub_expire_s;
        config::brpc_max_connections_per_server = 1;
        config::brpc_stub_expire_s = 3600;
        config::brpc_unhealthy_stub_expire_s = 300;
        _timer = std::make_unique<BthreadTimer>();
        ASSERT_OK(_timer->start());
    }
    void TearDown() override {
        _timer.reset();
        config::brpc_max_connections_per_server = _saved_brpc_max_connections_per_server;
        config::brpc_stub_expire_s = _saved_brpc_stub_expire_s;
        config::brpc_unhealthy_stub_expire_s = _saved_brpc_unhealthy_stub_expire_s;
    }

private:
    std::unique_ptr<BthreadTimer> _timer;
    int32_t _saved_brpc_max_connections_per_server = 0;
    int32_t _saved_brpc_stub_expire_s = 0;
    int32_t _saved_brpc_unhealthy_stub_expire_s = 0;
};

TEST_F(BrpcStubCacheTest, retirement_windows) {
    constexpr int64_t kSecondUs = 1000 * 1000;

    config::brpc_stub_expire_s = 3600;
    config::brpc_unhealthy_stub_expire_s = 300;
    EXPECT_EQ(3600 * kSecondUs, brpc_stub_idle_ttl_us());
    EXPECT_EQ(300 * kSecondUs, brpc_stub_unhealthy_ttl_us());

    // An unhealthy window at or above the idle TTL turns early retirement off rather than making
    // retirement more aggressive.
    config::brpc_unhealthy_stub_expire_s = 7200;
    EXPECT_EQ(brpc_stub_idle_ttl_us(), brpc_stub_unhealthy_ttl_us());

    // A non-positive window would schedule the cleanup task in the past.
    config::brpc_stub_expire_s = 0;
    config::brpc_unhealthy_stub_expire_s = 0;
    EXPECT_EQ(1 * kSecondUs, brpc_stub_idle_ttl_us());
    EXPECT_EQ(1 * kSecondUs, brpc_stub_unhealthy_ttl_us());
}

TEST_F(BrpcStubCacheTest, normal) {
    BrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    auto stub1 = cache.get_stub(address);
    ASSERT_NE(nullptr, stub1);
    address.port = 124;
    auto stub2 = cache.get_stub(address);
    ASSERT_NE(nullptr, stub2);
    ASSERT_NE(stub1, stub2);
    address.port = 123;
    auto stub3 = cache.get_stub(address);
    ASSERT_EQ(stub1, stub3);
}

TEST_F(BrpcStubCacheTest, invalid) {
    BrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "invalid.cm.invalid";
    address.port = 123;
    auto stub1 = cache.get_stub(address);
    ASSERT_EQ(nullptr, stub1);
}

TEST_F(BrpcStubCacheTest, reset) {
    BrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    auto stub1 = cache.get_stub(address);
    ASSERT_NE(nullptr, stub1);
    auto istub1 = stub1->stub();

    stub1->reset_channel();
    auto istub2 = stub1->stub();

    ASSERT_NE(istub1, istub2);
}

#ifndef __APPLE__
TEST_F(BrpcStubCacheTest, lake_service_stub_normal) {
    LakeServiceBrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    std::string hostname = "127.0.0.1";
    int32_t port1 = 123;
    auto stub1 = cache.get_stub(hostname, port1);
    ASSERT_TRUE(stub1.ok());
    int32_t port2 = 124;
    auto stub2 = cache.get_stub(hostname, port2);
    ASSERT_TRUE(stub2.ok());
    ASSERT_NE(*stub1, *stub2);
    auto stub3 = cache.get_stub(hostname, port1);
    ASSERT_TRUE(stub3.ok());
    ASSERT_EQ(*stub1, *stub3);
    auto stub4 = cache.get_stub("invalid.cm.invalid", 123);
    ASSERT_FALSE(stub4.ok());
}
#endif

TEST_F(BrpcStubCacheTest, test_http_stub) {
    HttpBrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    auto stub1 = cache.get_http_stub(address);
    ASSERT_NE(nullptr, *stub1);
    address.port = 124;
    auto stub2 = cache.get_http_stub(address);
    ASSERT_NE(nullptr, *stub2);
    ASSERT_NE(*stub1, *stub2);
    address.port = 123;
    auto stub3 = cache.get_http_stub(address);
    ASSERT_NE(nullptr, *stub3);
    ASSERT_EQ(*stub1, *stub3);

    address.hostname = "invalid.cm.invalid";
    auto stub4 = cache.get_http_stub(address);
    ASSERT_EQ(nullptr, *stub4);
}

// Retirement must release the stub, and with it the brpc channel, not merely drop the map entry:
// releasing the last channel reference is what makes brpc stop health-checking the endpoint.
TEST_F(BrpcStubCacheTest, test_cleanup) {
    config::brpc_stub_expire_s = 1;
    BrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    std::weak_ptr<PInternalService_RecoverableStub> retired;
    {
        auto stub1 = cache.get_stub(address);
        ASSERT_NE(nullptr, stub1);
        auto stub2 = cache.get_stub(address);
        ASSERT_EQ(stub2, stub1);
        retired = stub1;
    }

    sleep(2);
    ASSERT_TRUE(retired.expired()) << "idle endpoint must be retired and its channel released";
    auto stub3 = cache.get_stub(address);
    ASSERT_NE(nullptr, stub3);
}

#ifndef __APPLE__
TEST_F(BrpcStubCacheTest, test_lake_cleanup) {
    config::brpc_stub_expire_s = 1;
    LakeServiceBrpcStubCache cache(_timer.get());
    std::string hostname = "127.0.0.1";
    int32_t port = 123;
    std::weak_ptr<LakeService_RecoverableStub> retired;
    {
        auto stub1 = cache.get_stub(hostname, port);
        ASSERT_TRUE(stub1.ok());
        ASSERT_NE(nullptr, *stub1);
        auto stub2 = cache.get_stub(hostname, port);
        ASSERT_TRUE(stub2.ok());
        ASSERT_EQ(*stub2, *stub1);
        retired = *stub1;
    }

    sleep(2);
    ASSERT_TRUE(retired.expired()) << "idle lake endpoint must be retired and its channel released";
    auto stub3 = cache.get_stub(hostname, port);
    ASSERT_TRUE(stub3.ok());
    ASSERT_NE(nullptr, *stub3);
}
#endif

TEST_F(BrpcStubCacheTest, test_http_cleanup) {
    config::brpc_stub_expire_s = 1;
    HttpBrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    std::weak_ptr<PInternalService_RecoverableStub> retired;
    {
        auto stub1 = cache.get_http_stub(address);
        ASSERT_NE(nullptr, *stub1);
        auto stub2 = cache.get_http_stub(address);
        ASSERT_EQ(*stub2, *stub1);
        retired = *stub1;
    }

    sleep(2);
    ASSERT_TRUE(retired.expired()) << "idle http endpoint must be retired and its channel released";
    auto stub3 = cache.get_http_stub(address);
    ASSERT_NE(nullptr, *stub3);
}

// A caller holding a stub must defer retirement. Detaching the pool while its channel is still
// referenced would leave the channel probing and make the next lookup build a second pool for the
// same endpoint.
TEST_F(BrpcStubCacheTest, test_cleanup_deferred_while_stub_is_held) {
    config::brpc_stub_expire_s = 1;
    BrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    auto held = cache.get_stub(address);
    ASSERT_NE(nullptr, held);
    std::weak_ptr<PInternalService_RecoverableStub> observed = held;

    sleep(2);
    ASSERT_FALSE(observed.expired()) << "a held stub must not be destroyed";
    auto same = cache.get_stub(address);
    ASSERT_EQ(held, same) << "the deferred pool must stay in the map instead of being rebuilt";

    // Once the last caller releases it, a later firing retires the pool without a new lookup.
    held.reset();
    same.reset();
    sleep(3);
    ASSERT_TRUE(observed.expired()) << "retirement must resume after the caller releases the stub";
}

#ifndef __APPLE__
TEST_F(BrpcStubCacheTest, test_lake_cleanup_deferred_while_stub_is_held) {
    config::brpc_stub_expire_s = 1;
    LakeServiceBrpcStubCache cache(_timer.get());
    std::shared_ptr<LakeService_RecoverableStub> held;
    {
        auto acquired = cache.get_stub("127.0.0.1", 123);
        ASSERT_TRUE(acquired.ok());
        held = *acquired;
    }
    std::weak_ptr<LakeService_RecoverableStub> observed = held;

    sleep(2);
    ASSERT_FALSE(observed.expired());
    {
        auto same = cache.get_stub("127.0.0.1", 123);
        ASSERT_TRUE(same.ok());
        ASSERT_EQ(held, *same);
    }

    held.reset();
    sleep(3);
    ASSERT_TRUE(observed.expired());
}
#endif

TEST_F(BrpcStubCacheTest, test_http_cleanup_deferred_while_stub_is_held) {
    config::brpc_stub_expire_s = 1;
    HttpBrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    std::shared_ptr<PInternalService_RecoverableStub> held;
    {
        auto acquired = cache.get_http_stub(address);
        ASSERT_TRUE(acquired.ok());
        held = *acquired;
    }
    std::weak_ptr<PInternalService_RecoverableStub> observed = held;

    sleep(2);
    ASSERT_FALSE(observed.expired());
    {
        auto same = cache.get_http_stub(address);
        ASSERT_TRUE(same.ok());
        ASSERT_EQ(held, *same);
    }

    held.reset();
    sleep(3);
    ASSERT_TRUE(observed.expired());
}

// The short unhealthy window must only apply to endpoints brpc has marked failed. This endpoint has
// never connected, so brpc reports its socket healthy and only the ordinary idle TTL may retire it.
TEST_F(BrpcStubCacheTest, test_unhealthy_window_does_not_shorten_healthy_ttl) {
    config::brpc_stub_expire_s = 5;
    config::brpc_unhealthy_stub_expire_s = 1;
    BrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    std::weak_ptr<PInternalService_RecoverableStub> observed;
    {
        auto stub = cache.get_stub(address);
        ASSERT_NE(nullptr, stub);
        ASSERT_FALSE(stub->channel_failed()) << "a socket that never connected must not look failed";
        observed = stub;
    }

    sleep(3);
    ASSERT_FALSE(observed.expired()) << "a healthy endpoint must outlive the unhealthy window";

    sleep(4);
    ASSERT_TRUE(observed.expired()) << "the ordinary idle TTL must still retire it";
}

// Regression test: destroying BrpcStubCache while a cleanup task is scheduled
// (and possibly firing) must join the task before the cache state is torn down.
TEST_F(BrpcStubCacheTest, test_destructor_joins_inflight_cleanup_tasks) {
    config::brpc_stub_expire_s = 1;
    auto cache = std::make_unique<BrpcStubCache>(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    auto stub = cache->get_stub(address);
    ASSERT_NE(nullptr, stub);

    // Trigger ~BrpcStubCache() while the cleanup task is (or will be) in flight.
    // Drain the cache and assert the unique_ptr release returns cleanly.
    cache.reset();

    // Reacquire the endpoint through a fresh cache; the slot must have been
    // cleanly torn down without leaking the previous task.
    auto cache2 = std::make_unique<BrpcStubCache>(_timer.get());
    auto fresh_stub = cache2->get_stub(address);
    ASSERT_NE(nullptr, fresh_stub);
    cache2.reset();
}

// Regression test for the lazy-reschedule fix: while an endpoint is being
// accessed within the expire window, the timer fires, sees the stub is still
// active (idle < brpc_stub_expire_s), and reschedules instead of evicting, so the
// stub must survive across multiple timer periods.
TEST_F(BrpcStubCacheTest, test_active_access_keeps_stub_alive_across_expire_window) {
    config::brpc_stub_expire_s = 2;
    BrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    auto stub1 = cache.get_stub(address);
    ASSERT_NE(nullptr, stub1);

    // Repeatedly access within the window; each access refreshes the last-access
    // time, and the single scheduled timer reschedules instead of evicting.
    for (int i = 0; i < 3; ++i) {
        sleep(1);
        auto stub = cache.get_stub(address);
        ASSERT_EQ(stub1, stub) << "stub must not be evicted while being accessed";
    }
}

TEST_F(BrpcStubCacheTest, test_http_active_access_keeps_stub_alive_across_expire_window) {
    config::brpc_stub_expire_s = 2;
    HttpBrpcStubCache cache(_timer.get());
    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    auto stub1 = cache.get_http_stub(address);
    ASSERT_NE(nullptr, *stub1);

    for (int i = 0; i < 3; ++i) {
        sleep(1);
        auto stub = cache.get_http_stub(address);
        ASSERT_NE(nullptr, *stub);
        ASSERT_EQ(*stub1, *stub) << "http stub must not be evicted while being accessed";
    }
}

#ifndef __APPLE__
TEST_F(BrpcStubCacheTest, test_lake_active_access_keeps_stub_alive_across_expire_window) {
    config::brpc_stub_expire_s = 2;
    LakeServiceBrpcStubCache cache(_timer.get());
    std::string hostname = "127.0.0.1";
    int32_t port = 123;
    auto stub1 = cache.get_stub(hostname, port);
    ASSERT_TRUE(stub1.ok());
    ASSERT_NE(nullptr, *stub1);

    for (int i = 0; i < 3; ++i) {
        sleep(1);
        auto stub = cache.get_stub(hostname, port);
        ASSERT_TRUE(stub.ok());
        ASSERT_NE(nullptr, *stub);
        ASSERT_EQ(*stub1, *stub) << "lake stub must not be evicted while being accessed";
    }
}
#endif

TEST_F(BrpcStubCacheTest, http_singleton_reinitialize_rebinds_pipeline_timer) {
    auto timer2 = std::make_unique<BthreadTimer>();
    ASSERT_OK(timer2->start());

    HttpBrpcStubCache::initialize(_timer.get());
    auto* cache = HttpBrpcStubCache::getInstance();
    ASSERT_NE(nullptr, cache);

    TNetworkAddress address;
    address.hostname = "127.0.0.1";
    address.port = 123;
    auto stub = cache->get_http_stub(address);
    ASSERT_TRUE(stub.ok());
    ASSERT_NE(nullptr, *stub);

    cache->shutdown();
    ASSERT_FALSE(cache->get_http_stub(address).ok());

    HttpBrpcStubCache::initialize(timer2.get());

    auto rebound_stub = cache->get_http_stub(address);
    ASSERT_TRUE(rebound_stub.ok());
    ASSERT_NE(nullptr, *rebound_stub);

    cache->shutdown();
}

#ifndef __APPLE__
TEST_F(BrpcStubCacheTest, lake_singleton_reinitialize_rebinds_pipeline_timer) {
    auto timer2 = std::make_unique<BthreadTimer>();
    ASSERT_OK(timer2->start());

    LakeServiceBrpcStubCache::initialize(_timer.get());
    auto* cache = LakeServiceBrpcStubCache::getInstance();
    ASSERT_NE(nullptr, cache);

    auto stub = cache->get_stub("127.0.0.1", 123);
    ASSERT_TRUE(stub.ok());
    ASSERT_NE(nullptr, *stub);

    cache->shutdown();
    ASSERT_FALSE(cache->get_stub("127.0.0.1", 123).ok());

    LakeServiceBrpcStubCache::initialize(timer2.get());

    auto rebound_stub = cache->get_stub("127.0.0.1", 123);
    ASSERT_TRUE(rebound_stub.ok());
    ASSERT_NE(nullptr, *rebound_stub);

    cache->shutdown();
}
#endif

} // namespace starrocks
