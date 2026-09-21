# BE-local retirement of obsolete brpc endpoints

Status: implemented. See "Implementation status" for what landed and what did not.

This document replaces an earlier proposal that distributed an authoritative endpoint snapshot from the leader FE to every BE over the heartbeat protocol. That approach is retained below as an alternative, not as the recommendation. The reasons for demoting it are in "Why the FE-snapshot design was set aside".

## Implementation status

All four work items landed, together with the complementary gflag lever, across these files:

- `be/src/common/brpc/brpc_stub_cache.h` and `.cpp`: the `decide_endpoint_cleanup()` policy, the rewritten `EndpointCleanupTask::Run()`, the per-cache `detach_locked()` / `evaluate_cleanup_locked()` helpers, and the two new counters.
- `be/src/common/brpc/internal_service_recoverable_stub.h` / `.cpp` and `lake_service_recoverable_stub.h` / `.cpp`: `channel_failed()`, `last_use_us()` and `mark_used()`.
- `be/src/common/config.h`: `brpc_unhealthy_stub_expire_s` (default 300) and `brpc_health_check_interval_s` (default 3, matching brpc's own default).
- `be/src/service/service_be/starrocks_be.cpp`: assigns `brpc::FLAGS_health_check_interval`, clamped to >= 1.
- `be/test/common/brpc/brpc_stub_cache_test.cpp`: policy table test plus lifetime tests for all three caches.
- `docs/{en,zh,ja}/administration/configuration/BE_parameters/log_server_meta.md`.

Deviations from the design as written:

- The recheck cadence is derived as `clamp(unhealthy_window / 10, 1s, 30s)` rather than being a third config. It governs both health re-polling between the two deadlines and the retry after an ownership deferral, and it keeps short windows usable in tests.
- The "endpoints currently observed failing" gauge was dropped. Maintaining it would either mean probing every channel on each metric scrape or publishing a value stale by up to one unhealthy window. The two counters (`brpc_endpoint_stub_retired_unhealthy`, `brpc_endpoint_stub_retire_deferred`) carry the same signal without either drawback.
- The unlock-before-destroy fix was also applied to `wait_clean_tasks_terminate()`, which had the same defect on the shutdown path.
- No live-cluster reproduction was run; see "Validation requirements" for what remains.

## Provenance and verification basis

The StarRocks investigation was prepared against commit `99486d2b96daca350f1c8e3c0645f086956071a0` and re-checked against the current checkout.

The brpc mechanism was originally read from a local `apache/brpc` checkout at **1.17.0**, while StarRocks pins **1.9.0** in `thirdparty/vars.sh`. That gap has since been closed: every item in the re-verification checklist at the end of this document was confirmed against the `1.9.0` tag, and the results are recorded there.

Statements below are marked as verified from source, or as estimate/model. Nothing here has been measured on a live cluster.

Statements below are marked as verified from source, or as estimate/model. Nothing here has been measured on a live cluster.

## Incident and motivation

FE and BE run in Kubernetes pods in the same cluster. After a BE pod was replaced, peers emitted enough warnings to overload the Datadog log service:

```text
Fail to wait EPOLLOUT of fd=6262: Connection timed out [110]
```

The incident configuration supplied by the operator was:

```yaml
brpc_port: 8060
brpc_socket_keepalive: true
brpc_socket_max_unwritten_bytes: "10737418240"
brpc_max_connections_per_server: "64"
brpc_connection_type: "single"
```

Note that `brpc_max_connections_per_server` defaults to `1` in the current checkout; the incident cluster set it to `64`. Use the incident values when reproducing.

The operator creates StatefulSets and headless services and supplies `HOST_TYPE=FQDN`, so a BE registers under a stable pod FQDN such as `celostar-be-2.celostar-be-search.<namespace>.svc.cluster.local`. Replacing the pod at the same StatefulSet ordinal normally preserves its FQDN and FE backend ID while DNS can point to a new pod IP. An ordinary container restart within the same pod generally retains that pod's IP; pod replacement can assign a new IP. Do not equate all "restarts" with IP changes.

FE query scheduling places resolved IP addresses into execution destinations through methods such as `ComputeNode.getBrpcIpAddress()`. The BE brpc caches are keyed by the resolved `IP:port`, not by the stable backend ID or FQDN, so a DNS change influences new destinations without invalidating channels already cached for the old IP.

Temporary unreachability and an obsolete address are different situations. Probing a registered endpoint that may recover is useful. Continuing to probe the former IP of a replaced pod for an hour is not.

## Current failure mechanism

Verified from source:

1. BE B has cached stubs, and therefore brpc channels, to BE A's old IP.
2. A is replaced, retains its registered hostname, and obtains a different IP.
3. New work uses the new IP after FE/DNS convergence. Existing channels remain bound to the old numeric endpoint, and nothing renews or invalidates them.
4. A failed brpc socket enters background health checking. `Socket::OnFailed()` calls `StartHealthCheck()` whenever `HCEnabled()`, which is true for every socket created through the client-side socket map, because `GlobalSocketCreator::CreateSocket()` sets `health_check_interval_s = FLAGS_health_check_interval`.
5. `HealthCheckTask::OnTriggeringTask()` calls `Socket::CheckHealth()`, which performs a fresh `Connect()` bounded by `-health_check_timeout_ms`. When the old IP is silently unreachable, that connect times out and emits the warning above, then reschedules at `-health_check_interval`.
6. The StarRocks cache entry expires only after `brpc_stub_expire_s`, which defaults to 3600 and is renewed by every lookup.

Relevant brpc defaults: `health_check_interval` is 3 seconds, `health_check_timeout_ms` is 500, `defer_close_second` is 0.

With 64 populated connection groups, a rough model is `64 / (3 + 0.5) ≈ 18` timeout warnings per second per peer per obsolete endpoint. This is illustrative, not a measured incident rate.

Each StarRocks stub is placed on its own brpc connection group. `PInternalService_RecoverableStub::reset_channel()` sets `options.connection_group = "<seed>_<epoch>"`, and `ComputeChannelSignature()` folds `connection_group` into the `ChannelSignature` that forms part of the `SocketMapKey`. Consequently `connection_type=single` does **not** mean one TCP connection across all cache slots: 64 sibling stubs mean up to 64 distinct sockets, each independently health-checked. The `RecoverableClosure` `EHOSTDOWN` path bumps the epoch, which releases the old socket and creates a new one, so the count stays bounded but the sockets churn.

## The retirement rule

Retire a cached endpoint when both conditions hold:

- no RPC has been issued to it for `brpc_unhealthy_stub_expire_s` (proposed default 300 seconds), and
- its channels are currently in brpc's failed/health-checking state.

Healthy endpoints keep the existing unconditional idle TTL (`brpc_stub_expire_s`, 3600), so a warm connection to a quiet-but-reachable peer is not torn down and re-established.

The rule is deliberately local to the BE. It needs no membership knowledge, no protocol change, and no coordination with the FE. It relies on a single observation about the incident: once FE and DNS converge, nothing on the BE ever looks up the obsolete IP again, so the entry goes idle on its own. The health condition is what makes early retirement safe — the only entries eligible for it are ones that could not serve an RPC anyway.

## Verified release chain: dropping the stub really does stop the probing

This is the load-bearing claim, and it is the reason no brpc modification is required. Verified in the 1.17.0 source; re-verify against 1.9.0.

1. `BrpcStubCache::StubPool` holds `shared_ptr<PInternalService_RecoverableStub>`. Destroying the last reference destroys the inner `PInternalService_Stub`, which was constructed with `google::protobuf::Service::STUB_OWNS_CHANNEL` in `reset_channel()`, so it destroys the `brpc::Channel`.
2. `Channel::~Channel()` recomputes the channel signature and calls `SocketMapRemove(SocketMapKey(_server_address, sig))`.
3. `SocketMap::RemoveInternal()` decrements the entry's reference count. At zero, with `defer_close_second <= 0`, it erases the map entry, calls `s->ReleaseAdditionalReference()`, then `ReleaseReference(s)`.
4. `SocketMap::ReleaseReference()` sees `HCEnabled()` and calls `s->ReleaseHCRelatedReference()`, which clears `_is_hc_related_ref_held` and drops the health-check reference.
5. The socket is recycled. At the next firing, `HealthCheckTask::OnTriggeringTask()` gets `rc < 0` from `Socket::AddressFailedAsWell()` and returns `false`, ending the periodic task.

Probing therefore stops within at most one `health_check_interval` (about 3 seconds by default) after the final channel reference is released. A connect attempt already in progress is allowed to finish; there is no promise of instantaneous cancellation.

The success condition for this work is step 5, not the size of `_stub_map`. A test that asserts only on map size proves nothing.

## What already exists in StarRocks

Verified from source:

- The cache is keyed by `butil::EndPoint`, that is, resolved `IP:port`. Per-IP tracking is already the natural granularity.
- `BrpcStubCache::get_stub()` renews an absolute deadline on the endpoint's `EndpointCleanupTask` on every lookup, so an idle clock per endpoint already exists.
- `EndpointCleanupTask::Run()` already runs under the cache lock, already re-checks `_stopping`, already verifies through `is_cleanup_task_owner_locked()` that it is still the authoritative task for the endpoint, and either erases the entry or reschedules itself at the renewed deadline.
- `wait_clean_tasks_terminate()` and `reset_state_for_rebind()` already handle shutdown and timer rebinding for all three caches.
- Only two places in `be/src` construct a `brpc::Channel`: `internal_service_recoverable_stub.cpp` and `lake_service_recoverable_stub.cpp`. Every brpc client channel in the BE therefore flows through `BrpcStubCache`, `HttpBrpcStubCache`, or `LakeServiceBrpcStubCache`. There is no fourth owner to audit.

In other words the skeleton of "idle for N seconds, then drop" is already implemented. What is missing is the health condition, a shorter second-tier deadline, an accurate last-use signal, and — the real work — a guarantee that erasing the entry actually destroys the channels.

## Work item 1: expose a health signal on the recoverable stubs

`brpc::Channel::CheckHealth()` is public and needs no brpc change:

```cpp
int Channel::CheckHealth() {
    if (_lb == NULL) {
        SocketUniquePtr ptr;
        if (Socket::Address(_server_id, &ptr) == 0 && ptr->IsAvailable()) {
            return 0;
        }
        return -1;
    }
    // ... load-balanced case, not used by StarRocks ...
}
```

StarRocks constructs the channel in `reset_channel()`, so retain the raw `brpc::Channel*` next to `_stub`, written under the same `_mutex` that already guards the stub swap, and expose an accessor such as `bool channel_failed() const`. Reading the pointer under the shared lock is safe because the inner stub owns the channel and both are replaced atomically.

Do not reach the channel by casting `PInternalService_Stub::channel()`; that works but couples the cache to generated protobuf internals and to the `RecoverableChannel` indirection.

Semantics the implementation must encode honestly:

- `CheckHealth() != 0` means the socket is currently in the failed state, which for socket-map sockets means health checking is active. That is the signal we want.
- `CheckHealth() == 0` does **not** mean a connection exists. A socket that has never connected is not failed and reports healthy. The rule must therefore never treat "healthy" as evidence of reachability, only as a veto on early retirement.
- The state can flap, including when an unrelated pod reuses the old IP and starts accepting connections. That is correct behavior: a reachable endpoint should not be retired early.

Consider requiring the failing state to be observed on two consecutive evaluations before retiring, to avoid acting on a single blip. Retiring on a blip is not harmful — it costs one reconnect — so this is a tuning choice, not a correctness requirement.

## Work item 2: two-tier expiry

Add one mutable config next to the existing one in `be/src/common/config.h`:

```cpp
// Idle window before an endpoint whose channels are in brpc's failed/health-checking
// state is retired. Shorter than brpc_stub_expire_s so that obsolete addresses stop
// being probed promptly after a peer changes IP.
CONF_mInt32(brpc_unhealthy_stub_expire_s, "300");
```

`EndpointCleanupTask` then evaluates the applicable deadline at firing time from the pool's current health rather than from a value fixed at construction. Because the task already reschedules itself, the tier can change between firings without any extra machinery.

Setting `brpc_unhealthy_stub_expire_s >= brpc_stub_expire_s` must degrade cleanly to current behavior, and is the natural off switch for the feature. Both configs are BE configs, so `docs/en/administration/management/BE_configuration.md` and its `docs/zh/` peer need updating; read `docs/CLAUDE.md` first.

## Work item 3: a true last-use timestamp

`get_stub()` renewal is a proxy for usage, not a measurement of it. Some callers fetch a stub once and hold it for the lifetime of a fragment or load rather than re-looking it up per RPC; `exec/pipeline/exchange/sink_buffer.cpp` and `data_workflows/load/tablet_writer/local_tablets_channel.cpp` are examples. At a 3600-second window this is invisible. At 300 seconds an actively used endpoint could look idle.

`RecoverableChannel::CallMethod()` intercepts every RPC issued through a recoverable stub, so a relaxed atomic store of the current time there yields an exact per-stub last-use clock at negligible cost. The cleanup task then uses the maximum last-use time across the pool instead of, or in addition to, the lookup-renewed deadline.

This work item is strictly an accuracy improvement. The ownership gate in work item 4 already prevents a held stub from being destroyed, so omitting it cannot cause incorrect teardown — it can only cause a pool to be erased from the map and immediately recreated on the next lookup.

## Work item 4: ownership-gated eviction (the hard part)

Erasing the map entry does not by itself destroy the channels. Callers hold `shared_ptr<PInternalService_RecoverableStub>`, and every in-flight RPC holds one through `RecoverableClosure`, which captures `_stub->shared_from_this()`. Today, an entry that expires while a caller holds a stub is removed from the map while its channel — and its health checking — survives.

The eviction step must therefore be:

1. Under the cache's `_lock`, confirm the task is still the entry's owner and that the policy still applies.
2. Check `use_count() == 1` on **every** stub in the pool. One reference means only the pool's `_stubs` vector holds it.
3. If any stub is externally owned, defer the whole pool and reschedule the task at `now + retry_interval` rather than at the idle deadline. The current code only re-fires at the deadline, so without this change a deferred pool would never be reconsidered until it was looked up again.
4. If no stub is externally owned, move the `shared_ptr<StubPool>` out of the map into a local variable, erase the map entry, and release the lock.
5. Let the local go out of scope **after** unlocking, so channel destruction and `SocketMapRemove` run outside the spinlock.

Why the reference-count check is sound here, which is not true in general: `_lock` serializes the only issuance path, `StubPool::get_or_create()`. If all counts are 1 while the lock is held, no external reference exists and none can be created before the erase completes. `RecoverableChannel` holds a raw `_owner` pointer rather than a `shared_ptr`, so there is no self-reference inflating the count, and no ownership cycle. Document this invariant next to the check, because it breaks the moment someone adds a second issuance path that does not take `_lock`.

Step 5 fixes a pre-existing defect. `EndpointCleanupTask::Run()` currently calls `_cache->_stub_map.erase(_endpoint)` with the `SpinLock` held, so `~brpc::Channel` and `SocketMap::RemoveInternal()` — which takes brpc's global socket-map mutex and can destroy sockets — already run under a spinlock today. Fix it while touching this code.

Keep the existing `EndpointCleanupTask` owner check intact: a stale timer for endpoint A must never erase a newly created entry for the same A. Keep `_stopping`, the timer rebind path, and TTL renewal semantics unchanged.

## Scope: all three caches

`HttpBrpcStubCache` and `LakeServiceBrpcStubCache` share `EndpointCleanupTask` and the same lifetime helpers, so the change is uniform. They must be included, not deferred, because each stub gets its own connection group and therefore its own socket and its own health check. Leaving either cache untreated leaves a population of probing sockets to the obsolete endpoint.

Two per-cache details:

- The HTTP stubs use `protocol = "http"` and skip `connection_group` entirely, so their channel signature is zero and they share one socket per endpoint. The ownership check is the same; the population is smaller.
- `LakeServiceBrpcStubCache` and `HttpBrpcStubCache` store a single stub per endpoint rather than a pool, so their check is a single `use_count()`.

## Complementary lever: brpc health-check cadence

Retirement shortens how long probing lasts. The health-check cadence controls how loud it is while it lasts. These are independent and stack.

`starrocks_be.cpp` already sets brpc gflags directly at startup:

```cpp
brpc::FLAGS_max_body_size = config::brpc_max_body_size;
brpc::FLAGS_socket_keepalive = config::brpc_socket_keepalive;
brpc::FLAGS_socket_max_unwritten_bytes = config::brpc_socket_max_unwritten_bytes;
brpc::FLAGS_max_connection_pool_size = config::brpc_max_connection_pool_size;
```

`health_check_interval` is defined in `socket_map.cpp` and is not declared in a public brpc header, so it needs either a local `namespace brpc { DECLARE_int32(health_check_interval); }` or `GFLAGS_NS::SetCommandLineOption("health_check_interval", ...)`. Either way this is StarRocks-side configuration, not a brpc modification.

It is read in `GlobalSocketCreator::CreateSocket()`, so a change applies to sockets created afterwards, not to existing ones. Set it at startup.

Combined effect, as a model rather than a measurement: cutting the retirement window from 3600 to 300 seconds is a 12x reduction in warning duration, and raising the interval from 3 to 10 seconds is roughly a further 3x reduction in rate.

## Limitations

State these plainly in the PR; do not claim more than the design delivers.

- Retirement is not immediate. Five minutes of probing at the modeled rate is still on the order of thousands of log lines per peer. If that is still too many, tune the two levers above; the design does not eliminate the window.
- An endpoint that keeps being looked up is never retired, no matter how unhealthy. If any BE code path keeps resolving to the obsolete IP, the idle clock keeps being renewed. This design assumes FE/DNS convergence stops new lookups, which matches the incident but is an assumption, not an enforced invariant.
- A late caller can create a new entry for a retired address. The periodic re-evaluation retires it again once it is idle and failing.
- The `RecoverableClosure` `EHOSTDOWN` path still resets a held stub's channel, creating a fresh socket that will also fail and be health-checked. Cleanup only converges once the holder releases the stub. This behavior is left intact in this release.
- Nothing here forcibly cancels in-flight RPCs, rejects future calls to an obsolete address, or establishes process identity after IP reuse. An `IP:port` cannot distinguish two incarnations at the same address.

## Why the FE-snapshot design was set aside

The earlier proposal had the leader FE publish a versioned, complete set of registered BE endpoints, distribute it over the existing heartbeat with per-recipient acknowledgement and leader-epoch fencing, and have each BE reconcile its caches against it.

It solves a strictly larger problem: because FE membership is authoritative, it can retire an endpoint that the BE is still actively looking up, and it converges in seconds rather than minutes.

It was set aside because the cost is concentrated almost entirely outside the part that actually stops the probing:

- Two new optional `TMasterInfo` fields and one new `THeartbeatResult` field, with generated-code regeneration and mixed-version compatibility handling in both directions.
- A versioned snapshot publisher with DNS resolution outside the heartbeat critical path, membership revalidation, and explicit rules for incomplete resolution, because `DnsCache.tryLookup()` returns the hostname on failure and must never be allowed to masquerade as a resolved endpoint.
- Per-recipient acknowledgement state, leader-epoch reset, version-reuse prohibition, out-of-order and restart handling, and a BE-side reconciliation eligibility lease.
- Careful integration with `compare_master_info()`, `print_master_info()` and `update_master_info()` so that a large endpoint list does not end up in equality comparisons, master-info logs, or doubly buffered retained state, and so that `MasterInfoPtr` lifetimes are not extended across asynchronous work.
- O(N²) distribution bursts on membership change, modeled at roughly 30 MB to push 1,000 IPv4 endpoints to 1,000 BEs once.

Crucially, work item 4 above — proving no external owner holds a stub, detaching safely, and releasing the channel outside the cache lock — is required by **both** designs. The FE snapshot does not avoid it; it adds a distributed control plane on top of it. The BE-local rule keeps that one necessary piece and drops the rest.

If a future requirement demands retiring endpoints that are still being looked up, the FE-snapshot design is the right escalation, and it can be layered on top of this work without redoing it.

## Alternatives

| Alternative | Benefit | Limitation / decision |
|---|---|---|
| Idle plus unhealthy retirement (this document) | Small, BE-local, no protocol change; reuses the existing per-endpoint timer | Minutes rather than seconds; cannot retire an endpoint that is still being looked up |
| Lower `brpc_stub_expire_s` alone | One-line operational change | Tears down healthy idle endpoints too; still does not release channels held by callers, so probing can continue past expiry |
| Increase `health_check_interval` alone | Fewer warnings immediately | Slows recovery of valid endpoints and still probes obsolete addresses for the full TTL; good as a complement, not a fix |
| Reduce `brpc_max_connections_per_server` | Fewer probing sockets per endpoint | Changes concurrency and performance characteristics; does not address endpoint lifecycle |
| Adjust socket keepalive or unwritten-byte limit | May address other transport issues | Does not touch the background connect-check path that produces this warning |
| Filter or rate-limit the warning in Datadog | Protects ingestion quickly | Leaves the producer work and the stale channel ownership in place |
| FE-informed endpoint snapshots | Authoritative, converges in seconds, can retire actively-looked-up endpoints | Large protocol, state-machine and master-info surface for an incremental gain over the idle rule; see the section above |
| Strict generation-aware stub retirement | Can reject stale callers and stop channel resets | Broad behavioral change across callers and recovery paths; defer |

## Observability

Existing signals reviewed:

- `brpc_endpoint_stub_count`, registered by `BrpcStubCache::Metrics`, which counts cached endpoint entries and not sockets.
- The existing `LOG(INFO) << "cleanup brpc stub, endpoint:"` line in `EndpointCleanupTask::Run()`.
- brpc's `rpc_health_check_count` bvar and its socket/channel bvars and connection inspection pages.
- brpc's per-socket `LOG(INFO) << "Checking " << *this` emitted by `Socket::CheckHealth()` on the first check of each episode.

Preserve all of these. `brpc_endpoint_stub_count` in particular keeps its current meaning and must not be repurposed as a proxy for socket count.

Add a small, bounded-cardinality set following local metric conventions: endpoints retired early under the unhealthy rule, endpoints deferred because a stub was externally owned, and current count of endpoints observed in the failing state. Do not use endpoint IP as a metric label. Keep the existing per-eviction log line, and do not add a per-endpoint log on every deferred sweep; use a rate-limited summary instead.

The distinction that matters in the logs is "entry erased" versus "final channel reference released". Only the second stops probing. If the implementation adds a log line, make it say which one happened.

## Validation requirements

Deterministic unit tests, plus one transport-level test that proves the actual claim.

BE cache behavior:

1. An idle, failing endpoint is retired after `brpc_unhealthy_stub_expire_s` while an idle, healthy endpoint survives until `brpc_stub_expire_s`.
2. A held stub defers retirement; a later firing retires the pool once the holder releases it, without requiring a new lookup.
3. Setting `brpc_unhealthy_stub_expire_s >= brpc_stub_expire_s` restores current behavior exactly.
4. Lookup and insertion concurrent with a firing cleanup task are safe, and a recreated entry survives a callback owned by an older entry.
5. Timer cancellation, cache shutdown and timer rebind cannot deadlock or touch freed state; channel destruction does not occur under the cache spinlock.
6. All three caches are covered.

Transport reproduction:

Create an endpoint that becomes unreachable, populate the intended number of connection groups, observe health-check attempts, let the entry go idle, and verify that the final channel reference is released and that recurring connect attempts stop after any already-running attempt finishes. Verify separately that an endpoint which becomes temporarily unreachable but is still being used recovers normally and is not retired.

Where a real TCP blackhole is hard to arrange in a unit test, use a deterministic connect hook for the lifetime assertions and a separate controlled environment for the actual timeout warning. Do not alter the user's live cluster to obtain this measurement.

## Implementation map

| Area | Source entrypoints |
|---|---|
| Cache, timers, eviction policy | `be/src/common/brpc/brpc_stub_cache.cpp` and `.h` |
| Channel ownership and health accessor | `be/src/common/brpc/internal_service_recoverable_stub.cpp` and `.h`; `lake_service_recoverable_stub.cpp` and `.h` |
| Last-use timestamp interception | `RecoverableChannel::CallMethod()` in `internal_service_recoverable_stub.cpp` |
| Reset-on-`EHOSTDOWN` behavior | `be/src/base/brpc/recoverable_closure.h` |
| Config | `be/src/common/config.h`, plus `docs/en/` and `docs/zh/` BE configuration pages |
| brpc gflag startup block | `be/src/service/service_be/starrocks_be.cpp` |
| Timer facility | `be/src/common/bthread_timer.cpp` and `.h` |
| Tests | `be/test/common/brpc/brpc_stub_cache_test.cpp`; `internal_service_recoverable_stub_test.cpp` and `internal_service_recoverable_stub_parallel_test.cpp`; `be/test/base/brpc/recoverable_closure_test.cpp` |

Sequence followed, all steps complete except the last:

1. Re-verify the brpc release chain against the pinned 1.9.0 source (checklist below).
2. Add the health accessor to both recoverable stubs, with unit tests that do not require a live peer.
3. Add the config and the two-tier deadline evaluation in `EndpointCleanupTask::Run()`.
4. Add the ownership gate, the deferred-retry rescheduling, and the unlock-before-destroy fix; extend to all three caches.
5. Add the last-use timestamp.
6. Add metrics and documentation; run the transport reproduction. Metrics and documentation are done; the transport reproduction is not.

Run `python3 build-support/check_be_module_boundaries.py --mode full` before handoff. The change stays within `Common`, which is where these files already live, so no manifest change is expected.

## Re-verification checklist against brpc 1.9.0

All items were confirmed against the `1.9.0` tag of the local `apache/brpc` checkout.

- `Channel::~Channel()` calls `SocketMapRemove(SocketMapKey(_server_address, ComputeChannelSignature(_options)))`. Confirmed, `src/brpc/channel.cpp:142`.
- `ComputeChannelSignature()` folds `connection_group` into the signature, so the per-stub connection groups give each stub its own socket-map entry and its own socket. Confirmed, `src/brpc/channel.cpp:67`.
- `GlobalSocketCreator::CreateSocket()` sets `sock_opt.health_check_interval_s = FLAGS_health_check_interval`, so every client-side socket-map socket is health-check enabled. Confirmed, `src/brpc/socket_map.cpp:63`.
- `HealthCheckTask::OnTriggeringTask()` returns `false` when `Socket::AddressFailedAsWell()` returns a negative value, which is what ends the probe loop once the socket has been recycled. Confirmed, `src/brpc/details/health_check.cpp:161`.
- `Channel::CheckHealth()` exists with the single-server semantics used here: with no load balancer it reports healthy only when `Socket::Address(_server_id)` succeeds and the socket `IsAvailable()`. Confirmed, `src/brpc/channel.cpp:605`, identical to 1.17.0. The function has been present since brpc was open-sourced in 2017.
- `defer_close_second` still defaults to 0, so there is no deferred-close delay in the release path. Confirmed, `src/brpc/socket_map.cpp:43`.

One item differs from 1.17.0 without changing the conclusion. The zero-reference branch of `SocketMap::RemoveInternal()` in 1.9.0 reads:

```cpp
Socket* const s = sc->socket;
_map.erase(key);
mu.unlock();
s->ReleaseAdditionalReference(); // release extra ref
s->SetHCRelatedRefReleased();    // set released status to cancel health checking
SocketUniquePtr ptr(s);          // Dereference
```

1.17.0 folds the last two steps into `ReleaseReference()` / `ReleaseHCRelatedReference()`. 1.9.0 splits them: `SetHCRelatedRefReleased()` clears the flag that `HCEnabled()` reads, and the `SocketUniquePtr` drops the map's reference. The observable result is the same — the socket is recycled, the next `AddressFailedAsWell()` fails, and the health-check task ends. Note also that brpc unlocks its own map mutex before these releases, so the cost the StarRocks cache must keep off its spinlock is `~Channel` itself, not brpc's internal locking.
