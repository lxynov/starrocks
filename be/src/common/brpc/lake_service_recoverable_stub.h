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

#pragma once

#include "base/brpc/recoverable_closure.h"
#include "base/status.h"
#include "gen_cpp/lake_service.pb.h"

namespace starrocks {

class LakeService_RecoverableStub : public LakeService,
                                    public std::enable_shared_from_this<LakeService_RecoverableStub> {
public:
    LakeService_RecoverableStub(const butil::EndPoint& endpoint, std::string protocol = "",
                                int64_t connection_group_seed = 0);
    ~LakeService_RecoverableStub() override;

    Status reset_channel(int64_t next_connection_group = 0);

    std::shared_ptr<starrocks::LakeService_Stub> stub() const {
        std::shared_lock l(_mutex);
        return _stub;
    }

    int64_t connection_group() const { return _connection_group.load(); }

    // Wall-clock time (butil::gettimeofday_us() units) at which the last RPC was issued through this
    // stub, or at which the stub was created if it has never been used.
    int64_t last_use_us() const { return _last_use_us.load(std::memory_order_relaxed); }
    void mark_used();

    // True when brpc reports the current channel's socket as unavailable, which for a socket-map
    // socket means background health checking is running against it. A socket that has never
    // connected reports healthy, so a false result is not evidence of reachability.
    bool channel_failed() const;

    // implements LakeService ------------------------------------------

    void publish_version(::google::protobuf::RpcController* controller,
                         const ::starrocks::PublishVersionRequest* request,
                         ::starrocks::PublishVersionResponse* response, ::google::protobuf::Closure* done) override;

    void compact(::google::protobuf::RpcController* controller, const ::starrocks::CompactRequest* request,
                 ::starrocks::CompactResponse* response, ::google::protobuf::Closure* done) override;

private:
    std::shared_ptr<starrocks::LakeService_Stub> _stub;
    // Owned by _stub, which was constructed with STUB_OWNS_CHANNEL. Read and written under _mutex
    // together with _stub.
    brpc::Channel* _channel = nullptr;
    const butil::EndPoint _endpoint;
    std::atomic<int64_t> _connection_group = 0;
    // Distinguishes stubs that share the same endpoint.
    const int64_t _connection_group_seed = 0;
    std::atomic<int64_t> _last_use_us;
    mutable std::shared_mutex _mutex;
    std::string _protocol;
};

} // namespace starrocks
