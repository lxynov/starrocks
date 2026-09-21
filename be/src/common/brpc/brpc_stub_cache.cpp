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

#include "common/brpc/brpc_stub_cache.h"

#include <algorithm>

#include "base/failpoint/fail_point.h"
#include "base/metrics.h"
#include "base/time/time.h"
#include "common/config_network_fwd.h"
#include "gen_cpp/internal_service.pb.h"
#ifndef __APPLE__
#include "gen_cpp/lake_service.pb.h"
#endif

namespace starrocks {

namespace {

const char* const kBrpcEndpointStubCountMetric = "brpc_endpoint_stub_count";
const char* const kBrpcEndpointRetiredUnhealthyMetric = "brpc_endpoint_stub_retired_unhealthy";
const char* const kBrpcEndpointRetireDeferredMetric = "brpc_endpoint_stub_retire_deferred";

template <typename Cache>
Cache*& singleton_cache() {
    static Cache* cache = nullptr;
    return cache;
}

template <typename Cache>
std::mutex& singleton_cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline int64_t seconds_to_us(int64_t seconds) {
    return std::max<int64_t>(seconds, 1) * 1000 * 1000;
}
} // namespace

int64_t brpc_stub_idle_ttl_us() {
    return seconds_to_us(config::brpc_stub_expire_s);
}

int64_t brpc_stub_unhealthy_ttl_us() {
    return std::min(seconds_to_us(config::brpc_unhealthy_stub_expire_s), brpc_stub_idle_ttl_us());
}

template <typename CacheT, typename ExtractFn>
void wait_clean_tasks_terminate(CacheT* cache, ExtractFn extract) {
    std::vector<std::shared_ptr<EndpointCleanupTask<CacheT>>> tasks;
    // Entries are moved out rather than dropped under the lock so that ~brpc::Channel, which takes
    // brpc's global socket-map mutex and can close sockets, runs after the spinlock is released.
    std::vector<typename CacheT::DetachedEntry> detached;
    BthreadTimer* timer = nullptr;
    {
        std::lock_guard<SpinLock> l(cache->_lock);
        cache->_stopping = true;
        timer = cache->_timer;
        cache->_timer = nullptr;
        for (auto& stub : cache->_stub_map) {
            tasks.push_back(extract(stub.second));
            detached.push_back(std::move(stub.second));
        }
        cache->_stub_map.clear();
    }
    if (timer != nullptr) {
        for (auto& task : tasks) {
            task->unschedule_and_join(timer);
        }
    }
}

template <typename CacheT>
void reset_state_for_rebind(CacheT* cache, BthreadTimer* timer) {
    DCHECK(timer != nullptr);
    std::lock_guard<SpinLock> l(cache->_lock);
    DCHECK(cache->_stub_map.empty() || cache->_timer == timer);
    cache->_stopping = false;
    cache->_timer = timer;
}

struct BrpcStubCache::Metrics {
    Metrics(MetricRegistry* metric_registry, BrpcStubCache* cache) : registry(metric_registry), cache(cache) {
        DCHECK(registry != nullptr);
        registry->register_metric(kBrpcEndpointStubCountMetric, &brpc_endpoint_stub_count);
        registry->register_hook(kBrpcEndpointStubCountMetric, [this] {
            std::lock_guard<SpinLock> l(this->cache->_lock);
            brpc_endpoint_stub_count.set_value(this->cache->_stub_map.size());
        });
        registry->register_metric(kBrpcEndpointRetiredUnhealthyMetric, &brpc_endpoint_stub_retired_unhealthy);
        registry->register_metric(kBrpcEndpointRetireDeferredMetric, &brpc_endpoint_stub_retire_deferred);
    }

    ~Metrics() {
        registry->deregister_hook(kBrpcEndpointStubCountMetric);
        brpc_endpoint_stub_count.hide();
        brpc_endpoint_stub_retired_unhealthy.hide();
        brpc_endpoint_stub_retire_deferred.hide();
    }

    MetricRegistry* registry;
    BrpcStubCache* cache;
    UIntGauge brpc_endpoint_stub_count{MetricUnit::NOUNIT};
    // Endpoints retired before the ordinary idle TTL because their channels were failing.
    IntCounter brpc_endpoint_stub_retired_unhealthy{MetricUnit::NOUNIT};
    // Retirement attempts that could not proceed because a caller still held one of the stubs.
    IntCounter brpc_endpoint_stub_retire_deferred{MetricUnit::NOUNIT};
};

BrpcStubCache::BrpcStubCache(BthreadTimer* timer, MetricRegistry* metric_registry) : _timer(timer) {
    _stub_map.init(239);
    if (metric_registry != nullptr) {
        _metrics = std::make_unique<Metrics>(metric_registry, this);
    }
}

BrpcStubCache::~BrpcStubCache() {
    // Join the cleanup tasks first: they touch _metrics under _lock, and resetting it here is not
    // serialized against a task that is already past the _stopping check.
    wait_clean_tasks_terminate(this, [](const std::shared_ptr<StubPool>& pool) { return pool->_cleanup_task; });
    _metrics.reset();
}

bool BrpcStubCache::replace_cleanup_task_locked(const butil::EndPoint& endpoint,
                                                std::shared_ptr<EndpointCleanupTask<BrpcStubCache>> task) {
    auto pool = _stub_map.seek(endpoint);
    if (pool != nullptr) {
        (*pool)->_cleanup_task = std::move(task);
        return true;
    }
    return false;
}

BrpcStubCache::DetachedEntry BrpcStubCache::detach_locked(const butil::EndPoint& endpoint) {
    DetachedEntry detached;
    if (auto pool = _stub_map.seek(endpoint); pool != nullptr) {
        detached = std::move(*pool);
        _stub_map.erase(endpoint);
    }
    return detached;
}

bool BrpcStubCache::endpoint_failing_locked(const butil::EndPoint& endpoint) const {
    auto pool = _stub_map.seek(endpoint);
    DCHECK(pool != nullptr) << "caller must verify cleanup task ownership first";
    return (*pool)->any_channel_failed();
}

bool BrpcStubCache::endpoint_owned_locked(const butil::EndPoint& endpoint) const {
    auto pool = _stub_map.seek(endpoint);
    DCHECK(pool != nullptr) << "caller must verify cleanup task ownership first";
    return (*pool)->any_stub_externally_owned();
}

void BrpcStubCache::record_retirement_locked(bool unhealthy) {
    if (_metrics != nullptr && unhealthy) {
        _metrics->brpc_endpoint_stub_retired_unhealthy.increment(1);
    }
}

void BrpcStubCache::record_deferral_locked() {
    if (_metrics != nullptr) {
        _metrics->brpc_endpoint_stub_retire_deferred.increment(1);
    }
}

std::shared_ptr<PInternalService_RecoverableStub> BrpcStubCache::get_stub(const butil::EndPoint& endpoint) {
    std::lock_guard<SpinLock> l(_lock);

    const int64_t now_us = butil::gettimeofday_us();
    auto stub_pool = _stub_map.seek(endpoint);
    if (stub_pool == nullptr) {
        auto new_pool = std::make_shared<StubPool>();
        new_pool->_cleanup_task = std::make_shared<EndpointCleanupTask<BrpcStubCache>>(this, endpoint);
        new_pool->_cleanup_task->renew_last_lookup_locked(now_us);
        _stub_map.insert(endpoint, new_pool);
        stub_pool = _stub_map.seek(endpoint);

        timespec tm = butil::microseconds_to_timespec(first_cleanup_fire_us(now_us));
        auto status = _timer->schedule((*stub_pool)->_cleanup_task.get(), tm);
        if (!status.ok()) {
            LOG(WARNING) << "Failed to schedule brpc cleanup task: " << endpoint;
            _stub_map.erase(endpoint);
            return new_pool->get_or_create(endpoint);
        }
    } else {
        (*stub_pool)->_cleanup_task->renew_last_lookup_locked(now_us);
    }

    return (*stub_pool)->get_or_create(endpoint);
}

std::shared_ptr<PInternalService_RecoverableStub> BrpcStubCache::get_stub(const TNetworkAddress& taddr) {
    return get_stub(taddr.hostname, taddr.port);
}

std::shared_ptr<PInternalService_RecoverableStub> BrpcStubCache::get_stub(const std::string& host, int port) {
    butil::EndPoint endpoint;
    std::string realhost;
    std::string brpc_url;
    realhost = host;
    if (!is_valid_ip(host)) {
        Status status = hostname_to_ip(host, realhost);
        if (!status.ok()) {
            LOG(WARNING) << "failed to get ip from host:" << status.to_string();
            return nullptr;
        }
    }
    brpc_url = get_host_port(realhost, port);
    if (str2endpoint(brpc_url.c_str(), &endpoint)) {
        LOG(WARNING) << "unknown endpoint, host=" << host;
        return nullptr;
    }
    return get_stub(endpoint);
}

BrpcStubCache::StubPool::StubPool() {
    _stubs.reserve(config::brpc_max_connections_per_server);
}

BrpcStubCache::StubPool::~StubPool() {
    _stubs.clear();
    _cleanup_task.reset();
}

bool BrpcStubCache::StubPool::any_channel_failed() const {
    return std::any_of(_stubs.begin(), _stubs.end(), [](const auto& stub) { return stub->channel_failed(); });
}

bool BrpcStubCache::StubPool::any_stub_externally_owned() const {
    return std::any_of(_stubs.begin(), _stubs.end(), [](const auto& stub) { return stub.use_count() > 1; });
}

std::shared_ptr<PInternalService_RecoverableStub> BrpcStubCache::StubPool::get_or_create(
        const butil::EndPoint& endpoint) {
    if (UNLIKELY(_stubs.size() < config::brpc_max_connections_per_server)) {
        auto stub =
                std::make_shared<PInternalService_RecoverableStub>(endpoint, "", static_cast<int64_t>(_stubs.size()));
        if (!stub->reset_channel().ok()) {
            return nullptr;
        }
        _stubs.push_back(stub);
        return stub;
    }
    if (++_idx >= config::brpc_max_connections_per_server) {
        _idx = 0;
    }
    return _stubs[_idx];
}

void HttpBrpcStubCache::initialize(BthreadTimer* timer) {
    DCHECK(timer != nullptr);
    std::lock_guard<std::mutex> l(singleton_cache_mutex<HttpBrpcStubCache>());
    auto*& cache = singleton_cache<HttpBrpcStubCache>();
    if (cache == nullptr) {
        cache = new HttpBrpcStubCache(timer);
        return;
    }
    cache->bind_timer(timer);
}

HttpBrpcStubCache* HttpBrpcStubCache::getInstance() {
    return singleton_cache<HttpBrpcStubCache>();
}

HttpBrpcStubCache::HttpBrpcStubCache(BthreadTimer* timer) : _timer(timer) {
    _stub_map.init(500);
}

HttpBrpcStubCache::~HttpBrpcStubCache() {
    shutdown();
}

void HttpBrpcStubCache::bind_timer(BthreadTimer* timer) {
    reset_state_for_rebind(this, timer);
}

void HttpBrpcStubCache::shutdown() {
    wait_clean_tasks_terminate(this, [](const StubEntry& entry) { return entry.cleanup_task; });
}

bool HttpBrpcStubCache::replace_cleanup_task_locked(const butil::EndPoint& endpoint,
                                                    std::shared_ptr<EndpointCleanupTask<HttpBrpcStubCache>> task) {
    auto entry = _stub_map.seek(endpoint);
    if (entry != nullptr) {
        entry->cleanup_task = std::move(task);
        return true;
    }
    return false;
}

HttpBrpcStubCache::DetachedEntry HttpBrpcStubCache::detach_locked(const butil::EndPoint& endpoint) {
    DetachedEntry detached;
    if (auto entry = _stub_map.seek(endpoint); entry != nullptr) {
        detached = std::move(*entry);
        _stub_map.erase(endpoint);
    }
    return detached;
}

bool HttpBrpcStubCache::endpoint_failing_locked(const butil::EndPoint& endpoint) const {
    auto entry = _stub_map.seek(endpoint);
    DCHECK(entry != nullptr) << "caller must verify cleanup task ownership first";
    return entry->stub->channel_failed();
}

bool HttpBrpcStubCache::endpoint_owned_locked(const butil::EndPoint& endpoint) const {
    auto entry = _stub_map.seek(endpoint);
    DCHECK(entry != nullptr) << "caller must verify cleanup task ownership first";
    return entry->stub.use_count() > 1;
}

StatusOr<std::shared_ptr<PInternalService_RecoverableStub>> HttpBrpcStubCache::get_http_stub(
        const TNetworkAddress& taddr) {
    butil::EndPoint endpoint;
    std::string realhost;
    std::string brpc_url;
    realhost = taddr.hostname;
    if (!is_valid_ip(taddr.hostname)) {
        Status status = hostname_to_ip(taddr.hostname, realhost);
        if (!status.ok()) {
            LOG(WARNING) << "failed to get ip from host:" << status.to_string();
            return nullptr;
        }
    }
    brpc_url = get_host_port(realhost, taddr.port);
    if (str2endpoint(brpc_url.c_str(), &endpoint)) {
        return Status::RuntimeError("unknown endpoint, host = " + taddr.hostname);
    }
    // get is exist
    std::lock_guard<SpinLock> l(_lock);
    if (_timer == nullptr) {
        return Status::ServiceUnavailable("HttpBrpcStubCache is not initialized");
    }

    const int64_t now_us = butil::gettimeofday_us();
    auto stub_pair_ptr = _stub_map.seek(endpoint);
    if (stub_pair_ptr == nullptr) {
        // create
        auto new_task = std::make_shared<EndpointCleanupTask<HttpBrpcStubCache>>(this, endpoint);
        auto stub = std::make_shared<PInternalService_RecoverableStub>(endpoint, "http");
        if (!stub->reset_channel().ok()) {
            return Status::RuntimeError("init http brpc channel error on " + taddr.hostname + ":" +
                                        std::to_string(taddr.port));
        }
        new_task->renew_last_lookup_locked(now_us);
        _stub_map.insert(endpoint, StubEntry{stub, new_task});
        stub_pair_ptr = _stub_map.seek(endpoint);

        timespec tm = butil::microseconds_to_timespec(first_cleanup_fire_us(now_us));
        auto status = _timer->schedule(stub_pair_ptr->cleanup_task.get(), tm);
        if (!status.ok()) {
            LOG(WARNING) << "Failed to schedule brpc cleanup task: " << endpoint;
            _stub_map.erase(endpoint);
            return stub;
        }
    } else {
        stub_pair_ptr->cleanup_task->renew_last_lookup_locked(now_us);
    }

    return stub_pair_ptr->stub;
}

#ifndef __APPLE__

void LakeServiceBrpcStubCache::initialize(BthreadTimer* timer) {
    DCHECK(timer != nullptr);
    std::lock_guard<std::mutex> l(singleton_cache_mutex<LakeServiceBrpcStubCache>());
    auto*& cache = singleton_cache<LakeServiceBrpcStubCache>();
    if (cache == nullptr) {
        cache = new LakeServiceBrpcStubCache(timer);
        return;
    }
    cache->bind_timer(timer);
}

LakeServiceBrpcStubCache* LakeServiceBrpcStubCache::getInstance() {
    return singleton_cache<LakeServiceBrpcStubCache>();
}

LakeServiceBrpcStubCache::LakeServiceBrpcStubCache(BthreadTimer* timer) : _timer(timer) {
    _stub_map.init(500);
}

LakeServiceBrpcStubCache::~LakeServiceBrpcStubCache() {
    shutdown();
}

void LakeServiceBrpcStubCache::bind_timer(BthreadTimer* timer) {
    reset_state_for_rebind(this, timer);
}

void LakeServiceBrpcStubCache::shutdown() {
    wait_clean_tasks_terminate(this, [](const StubEntry& entry) { return entry.cleanup_task; });
}

bool LakeServiceBrpcStubCache::replace_cleanup_task_locked(
        const butil::EndPoint& endpoint, std::shared_ptr<EndpointCleanupTask<LakeServiceBrpcStubCache>> task) {
    auto entry = _stub_map.seek(endpoint);
    if (entry != nullptr) {
        entry->cleanup_task = std::move(task);
        return true;
    }
    return false;
}

LakeServiceBrpcStubCache::DetachedEntry LakeServiceBrpcStubCache::detach_locked(const butil::EndPoint& endpoint) {
    DetachedEntry detached;
    if (auto entry = _stub_map.seek(endpoint); entry != nullptr) {
        detached = std::move(*entry);
        _stub_map.erase(endpoint);
    }
    return detached;
}

bool LakeServiceBrpcStubCache::endpoint_failing_locked(const butil::EndPoint& endpoint) const {
    auto entry = _stub_map.seek(endpoint);
    DCHECK(entry != nullptr) << "caller must verify cleanup task ownership first";
    return entry->stub->channel_failed();
}

bool LakeServiceBrpcStubCache::endpoint_owned_locked(const butil::EndPoint& endpoint) const {
    auto entry = _stub_map.seek(endpoint);
    DCHECK(entry != nullptr) << "caller must verify cleanup task ownership first";
    return entry->stub.use_count() > 1;
}

DEFINE_FAIL_POINT(get_stub_return_nullptr);
StatusOr<std::shared_ptr<starrocks::LakeService_RecoverableStub>> LakeServiceBrpcStubCache::get_stub(
        const std::string& host, int port) {
    butil::EndPoint endpoint;
    std::string realhost;
    std::string brpc_url;
    realhost = host;
    if (!is_valid_ip(host)) {
        RETURN_IF_ERROR(hostname_to_ip(host, realhost));
    }
    brpc_url = get_host_port(realhost, port);
    if (str2endpoint(brpc_url.c_str(), &endpoint)) {
        return Status::RuntimeError("unknown endpoint, host = " + host);
    }
    // get if exist
    std::lock_guard<SpinLock> l(_lock);
    if (_timer == nullptr) {
        return Status::ServiceUnavailable("LakeServiceBrpcStubCache is not initialized");
    }

    const int64_t now_us = butil::gettimeofday_us();
    auto stub_pair_ptr = _stub_map.seek(endpoint);
    FAIL_POINT_TRIGGER_EXECUTE(get_stub_return_nullptr, { stub_pair_ptr = nullptr; });
    if (stub_pair_ptr == nullptr) {
        // create
        auto stub = std::make_shared<starrocks::LakeService_RecoverableStub>(endpoint, "");
        auto new_task = std::make_shared<EndpointCleanupTask<LakeServiceBrpcStubCache>>(this, endpoint);
        if (!stub->reset_channel().ok()) {
            return Status::RuntimeError("init lakeService brpc channel error on " + host + ":" + std::to_string(port));
        }
        new_task->renew_last_lookup_locked(now_us);
        _stub_map.insert(endpoint, StubEntry{stub, new_task});
        stub_pair_ptr = _stub_map.seek(endpoint);

        timespec tm = butil::microseconds_to_timespec(first_cleanup_fire_us(now_us));
        auto status = _timer->schedule(stub_pair_ptr->cleanup_task.get(), tm);
        if (!status.ok()) {
            LOG(WARNING) << "Failed to schedule brpc cleanup task: " << endpoint;
            _stub_map.erase(endpoint);
            return stub;
        }
    } else {
        stub_pair_ptr->cleanup_task->renew_last_lookup_locked(now_us);
    }

    return stub_pair_ptr->stub;
}

#endif

} // namespace starrocks
