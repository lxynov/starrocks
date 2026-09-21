// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/util/brpc_stub_cache.h

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

#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "base/brpc/brpc.h"
#include "base/concurrency/spinlock.h"
#include "base/network/network_util.h"
#include "base/time/time.h"
#include "common/brpc/internal_service_recoverable_stub.h"
#include "common/bthread_timer.h"
#include "common/logging.h"
#include "common/statusor.h"
#include "gen_cpp/Types_types.h" // TNetworkAddress

#ifndef __APPLE__
#include "common/brpc/lake_service_recoverable_stub.h"
#endif

namespace starrocks {

class MetricRegistry;

constexpr int TIMER_TASK_RUNNING = 1;

// Outcome of evaluating one cached endpoint for retirement.
struct EndpointCleanupDecision {
    enum class Action { kRetire, kReschedule };

    Action action{Action::kReschedule};
    // Absolute time (butil::gettimeofday_us() units) of the next evaluation. Only meaningful
    // for kReschedule.
    int64_t next_fire_us{0};
    // Latest activity observed for the endpoint, used for logging.
    int64_t last_use_us{0};
    // The endpoint was past a retirement deadline but a caller still owned one of its stubs.
    bool deferred_by_owner{false};
    // The endpoint was retired before the ordinary idle TTL because its channels were failing.
    bool retired_unhealthy{false};
};

// Retirement policy, kept free of cache internals so it can be reasoned about and tested on its own.
//
// An endpoint is retired once it has been idle for config::brpc_stub_expire_s, or once it has been
// idle for config::brpc_unhealthy_stub_expire_s while its channels are in brpc's failed state. The
// second rule is what releases the channels of a peer that changed IP: nothing looks the old address
// up again, so it goes idle, and its channels never recover.
//
// `failing` is only ever a reason to retire early. It is never used to keep an endpoint alive, because
// a socket that has never connected also reports healthy.
EndpointCleanupDecision decide_endpoint_cleanup(int64_t last_use_us, int64_t now_us, bool failing,
                                                bool externally_owned);

// When a newly cached endpoint should first be evaluated.
int64_t first_cleanup_fire_us(int64_t last_use_us);

template <typename StubCacheT>
class EndpointCleanupTask : public BthreadTimerTask {
public:
    EndpointCleanupTask(StubCacheT* cache, const butil::EndPoint& endpoint) : _cache(cache), _endpoint(endpoint) {}

    // The retirement decision must run while the cache's _lock is held so that _stopping, the entry's
    // activity and the ownership of its stubs are all observed atomically with the cache state.
    void Run() override {
        // Declared before the lock guard, and therefore destroyed after it, so that ~brpc::Channel
        // runs outside the spinlock: it takes brpc's global socket-map mutex and can close sockets.
        typename StubCacheT::DetachedEntry detached;
        std::lock_guard<SpinLock> l(_cache->_lock);
        if (_cache->_stopping) {
            return;
        }
        // Make sure this task is still the authoritative cleanup task for the
        // endpoint before rescheduling. If shutdown() cleared the cache or a new
        // entry was created for the same endpoint, this task is stale and must
        // not schedule anything.
        if (!_cache->is_cleanup_task_owner_locked(_endpoint, this)) {
            return;
        }
        int64_t now_us = butil::gettimeofday_us();
        auto decision = _cache->evaluate_cleanup_locked(_endpoint, _last_use_us, now_us);
        _cache->record_cleanup_outcome_locked(decision);
        if (decision.action == EndpointCleanupDecision::Action::kRetire) {
            LOG(INFO) << "cleanup brpc stub, endpoint:" << _endpoint << ", idle for "
                      << (now_us - decision.last_use_us) / 1000 << "ms"
                      << (decision.retired_unhealthy ? ", channels failing" : "");
            detached = _cache->detach_locked(_endpoint);
            return;
        }
        auto new_task = std::make_shared<EndpointCleanupTask<StubCacheT>>(_cache, _endpoint);
        new_task->_last_use_us = _last_use_us;
        if (!_cache->replace_cleanup_task_locked(_endpoint, new_task)) {
            return;
        }
        timespec tm = butil::microseconds_to_timespec(decision.next_fire_us);
        auto status = _cache->_timer->schedule(new_task.get(), tm);
        if (!status.ok()) {
            LOG(WARNING) << "Failed to reschedule brpc cleanup task: " << _endpoint;
            // Drop the entry; the next get_*_stub() will recreate it with a fresh task.
            detached = _cache->detach_locked(_endpoint);
        }
    }

    // Record that the endpoint was looked up at `now_us`. Lookups are only one source of activity;
    // the stubs themselves also timestamp every RPC, which covers callers that fetch a stub once and
    // then reuse it. Caller must hold the cache lock.
    void renew_last_use_locked(int64_t now_us) { _last_use_us = now_us; }

private:
    StubCacheT* _cache;
    butil::EndPoint _endpoint;
    // Time of the last lookup, in butil::gettimeofday_us() units. Read and written only under the
    // cache's _lock, so it does not need to be atomic.
    int64_t _last_use_us{0};
};

class BrpcStubCache {
public:
    explicit BrpcStubCache(BthreadTimer* timer, MetricRegistry* metrics = nullptr);
    ~BrpcStubCache();

    std::shared_ptr<PInternalService_RecoverableStub> get_stub(const butil::EndPoint& endpoint);
    std::shared_ptr<PInternalService_RecoverableStub> get_stub(const TNetworkAddress& taddr);
    std::shared_ptr<PInternalService_RecoverableStub> get_stub(const std::string& host, int port);

private:
    friend class EndpointCleanupTask<BrpcStubCache>;

    template <typename CacheT, typename ExtractFn>
    friend void wait_clean_tasks_terminate(CacheT* cache, ExtractFn extract);

    template <typename CacheT>
    friend void reset_state_for_rebind(CacheT* cache, BthreadTimer* timer);

    bool is_cleanup_task_owner_locked(const butil::EndPoint& endpoint,
                                      const EndpointCleanupTask<BrpcStubCache>* task) const {
        auto pool = _stub_map.seek(endpoint);
        return pool != nullptr && (*pool)->_cleanup_task.get() == task;
    }

    bool replace_cleanup_task_locked(const butil::EndPoint& endpoint,
                                     std::shared_ptr<EndpointCleanupTask<BrpcStubCache>> task);

    struct Metrics;
    struct StubPool {
        StubPool();
        ~StubPool();
        std::shared_ptr<PInternalService_RecoverableStub> get_or_create(const butil::EndPoint& endpoint);

        // The three predicates below are called by the cleanup task under BrpcStubCache::_lock.

        // Latest RPC timestamp across the pool's stubs.
        int64_t last_use_us() const;
        // Whether any channel in the pool is in brpc's failed state.
        bool any_channel_failed() const;
        // Whether any stub is referenced by something other than this pool. `_stubs` holds exactly one
        // reference per stub, and get_or_create() is the only way to obtain another, so while the cache
        // lock is held a count of one proves that no caller can be using the stub and that none can
        // start before the pool is detached. An in-flight RPC also counts here, because
        // RecoverableClosure holds the stub through shared_from_this().
        bool any_stub_externally_owned() const;

        std::vector<std::shared_ptr<PInternalService_RecoverableStub>> _stubs;
        int64_t _idx{-1};
        std::shared_ptr<EndpointCleanupTask<BrpcStubCache>> _cleanup_task;
    };

    // Entry ownership handed back to the cleanup task so that it can destroy the channels after
    // releasing the cache lock.
    using DetachedEntry = std::shared_ptr<StubPool>;
    DetachedEntry detach_locked(const butil::EndPoint& endpoint);
    EndpointCleanupDecision evaluate_cleanup_locked(const butil::EndPoint& endpoint, int64_t renewed_last_use_us,
                                                    int64_t now_us) const;
    void record_cleanup_outcome_locked(const EndpointCleanupDecision& decision);

    SpinLock _lock;
    butil::FlatMap<butil::EndPoint, std::shared_ptr<StubPool>> _stub_map;
    BthreadTimer* _timer;
    std::unique_ptr<Metrics> _metrics;
    bool _stopping{false};
};

class HttpBrpcStubCache {
public:
    HttpBrpcStubCache(const HttpBrpcStubCache&) = delete;
    HttpBrpcStubCache& operator=(const HttpBrpcStubCache&) = delete;

    static void initialize(BthreadTimer* timer);
    static HttpBrpcStubCache* getInstance();
    StatusOr<std::shared_ptr<PInternalService_RecoverableStub>> get_http_stub(const TNetworkAddress& taddr);
    void shutdown();

private:
    explicit HttpBrpcStubCache(BthreadTimer* timer);
    ~HttpBrpcStubCache();
    void bind_timer(BthreadTimer* timer);
    friend class EndpointCleanupTask<HttpBrpcStubCache>;

    template <typename CacheT, typename ExtractFn>
    friend void wait_clean_tasks_terminate(CacheT* cache, ExtractFn extract);

    template <typename CacheT>
    friend void reset_state_for_rebind(CacheT* cache, BthreadTimer* timer);

    bool is_cleanup_task_owner_locked(const butil::EndPoint& endpoint,
                                      const EndpointCleanupTask<HttpBrpcStubCache>* task) const {
        auto entry = _stub_map.seek(endpoint);
        return entry != nullptr && entry->cleanup_task.get() == task;
    }

    bool replace_cleanup_task_locked(const butil::EndPoint& endpoint,
                                     std::shared_ptr<EndpointCleanupTask<HttpBrpcStubCache>> task);

    struct StubEntry {
        std::shared_ptr<PInternalService_RecoverableStub> stub;
        std::shared_ptr<EndpointCleanupTask<HttpBrpcStubCache>> cleanup_task;
    };

    using DetachedEntry = StubEntry;
    DetachedEntry detach_locked(const butil::EndPoint& endpoint);
    EndpointCleanupDecision evaluate_cleanup_locked(const butil::EndPoint& endpoint, int64_t renewed_last_use_us,
                                                    int64_t now_us) const;
    void record_cleanup_outcome_locked(const EndpointCleanupDecision&) {}

    SpinLock _lock;
    butil::FlatMap<butil::EndPoint, StubEntry> _stub_map;
    BthreadTimer* _timer;
    bool _stopping{false};
};

#ifndef __APPLE__
class LakeServiceBrpcStubCache {
public:
    LakeServiceBrpcStubCache(const LakeServiceBrpcStubCache&) = delete;
    LakeServiceBrpcStubCache& operator=(const LakeServiceBrpcStubCache&) = delete;

    static void initialize(BthreadTimer* timer);
    static LakeServiceBrpcStubCache* getInstance();
    StatusOr<std::shared_ptr<starrocks::LakeService_RecoverableStub>> get_stub(const std::string& host, int port);
    void shutdown();

private:
    explicit LakeServiceBrpcStubCache(BthreadTimer* timer);
    ~LakeServiceBrpcStubCache();
    void bind_timer(BthreadTimer* timer);
    friend class EndpointCleanupTask<LakeServiceBrpcStubCache>;

    template <typename CacheT, typename ExtractFn>
    friend void wait_clean_tasks_terminate(CacheT* cache, ExtractFn extract);

    template <typename CacheT>
    friend void reset_state_for_rebind(CacheT* cache, BthreadTimer* timer);

    bool is_cleanup_task_owner_locked(const butil::EndPoint& endpoint,
                                      const EndpointCleanupTask<LakeServiceBrpcStubCache>* task) const {
        auto entry = _stub_map.seek(endpoint);
        return entry != nullptr && entry->cleanup_task.get() == task;
    }

    bool replace_cleanup_task_locked(const butil::EndPoint& endpoint,
                                     std::shared_ptr<EndpointCleanupTask<LakeServiceBrpcStubCache>> task);

    struct StubEntry {
        std::shared_ptr<LakeService_RecoverableStub> stub;
        std::shared_ptr<EndpointCleanupTask<LakeServiceBrpcStubCache>> cleanup_task;
    };

    using DetachedEntry = StubEntry;
    DetachedEntry detach_locked(const butil::EndPoint& endpoint);
    EndpointCleanupDecision evaluate_cleanup_locked(const butil::EndPoint& endpoint, int64_t renewed_last_use_us,
                                                    int64_t now_us) const;
    void record_cleanup_outcome_locked(const EndpointCleanupDecision&) {}

    SpinLock _lock;
    butil::FlatMap<butil::EndPoint, StubEntry> _stub_map;
    BthreadTimer* _timer;
    bool _stopping{false};
};
#endif

} // namespace starrocks
