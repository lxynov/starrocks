# FE Error-Reporting Race Condition: `MEM_LIMIT_EXCEEDED` vs `CANCELLED`

## TL;DR

This is a **real issue**. When a query exceeds a memory limit on one BE, the FE
coordinator does **not** guarantee that the memory-limit error reaches the user.
Because of a first-writer-wins status merge with no priority for
`MEM_LIMIT_EXCEEDED`, a sibling BE's `CANCELLED` status (produced by the OOM
teardown) can pre-empt the real error, and the user/gateway sees a bare
`CANCELLED` instead of the descriptive resource-group message.

All file/line references below are for `fe/fe-core/src/main/java/com/starrocks/qe/DefaultCoordinator.java`
unless otherwise noted.

## Background

On the BE side, memory-limit detection is reliable. BE keeps a tree of memory
trackers and checks limits bottom-up:

```text
PROCESS -> QUERY_POOL -> RESOURCE_GROUP -> QUERY
```

The tracker that is found over its limit produces a specific message (see
`be/src/runtime/mem_tracker.cpp:246-296`), e.g.:

- `Mem usage has exceed the limit of the resource group [rgSmallP0]. You can change the limit by modifying [mem_limit] of this group`
- `Mem usage has exceed the big query limit of the resource group [...]. You can change the limit by modifying [big_query_mem_limit] of this group`

The intent is that a gateway can read this message and decide whether to retry in
the same resource group (rg-limit hit) versus upgrade to a bigger one
(per-query limit hit).

The reliability problem is **not** on the BE. It is in how the FE aggregates
statuses reported by multiple BEs.

## Root cause: first-writer-wins status merge

The client-facing error is `queryStatus`, exposed via `getExecStatus()` and thrown
out of `getNext()` through `dealStatusToTryRetry(...)`.

`queryStatus` is set by `updateStatus(...)`, which drops any status once a non-OK
status is already present, with no consideration of status type:

```java
// DefaultCoordinator.updateStatus, ~line 894
// don't override an error status; also, cancellation has already started
if (!queryStatus.ok()) {
    return;
}
queryStatus.setStatus(status);
```

There is no rule that `MEM_LIMIT_EXCEEDED` should override `CANCELLED`. Whichever
non-OK status is processed **first** wins. The same first-wins semantics apply to
the client-facing error code via `setErrorCodeOnce(...)` (lines ~963 and ~1230)
and to the external `cancel()` path (line ~1012).

## Why ordering is not guaranteed

Multiple independent, asynchronous producers feed `updateStatus(...)`, serialized
only by a lock in **arrival order**:

- The report-RPC handler for each fragment instance (`updateFragmentExecStatus`, ~line 1236). The OOMing BE reports `MEM_LIMIT_EXCEEDED` here, but every cancelled sibling BE reports `CANCELLED` through the same handler.
- The result receiver in `getNext()` (~lines 959-967), which can surface a teardown/stream-broken status.
- External `cancel()` (~lines 1004-1018) from timeout watchdog, `KILL`, or client disconnect.

None of these are ordered relative to each other.

## The failing interleaving

1. BE-A exceeds its resource-group memory limit; its fragment dies.
2. The teardown breaks data channels feeding the result sink / other fragments, so a sibling BE (or the coordinator's `receiver.getNext`) produces a `CANCELLED`/stream-broken status.
3. That `CANCELLED` reaches `updateStatus` **before** BE-A's authoritative `reportExecStatus(MEM_LIMIT_EXCEEDED)` RPC. `queryStatus` was OK, so `CANCELLED` is stored.
4. BE-A's real `MEM_LIMIT_EXCEEDED` report arrives, hits the `!queryStatus.ok()` guard, and is discarded.
5. The user/gateway sees `CANCELLED`, not the resource-group message.

### Why the happy path usually works

When BE-A's report is processed first, `updateStatus` records
`MEM_LIMIT_EXCEEDED` and only then calls `cancelInternal(...)` to cancel siblings.
The siblings' later `CANCELLED` reports hit the guard and are discarded. So the
message is correct whenever the OOM report happens to arrive first — which is why
this fails only a fraction of the time, not always.

## Why existing cancel filtering does not save us

The only cancel-suppression logic covers internal teardown reasons, and only in
the deploy path (`handleErrorExecution`), not the report-RPC path:

```java
// DefaultCoordinator.isInternalCancelError, ~line 734
private boolean isInternalCancelError(String errMsg) {
    return errMsg.startsWith(FeConstants.LIMIT_REACH_ERROR)
        || errMsg.startsWith(FeConstants.QUERY_FINISHED_ERROR);
}
```

A generic OOM-induced `CANCELLED` from a sibling BE matches none of these, so
nothing prevents it from winning the race in `updateStatus`.

## Impact

- A fraction of memory-limit failures surface to the client as a bare `CANCELLED` (error code `CANCELLED`, message like `[reason=INTERNAL_ERROR] [msg=...]` or internal cancel text) instead of the descriptive resource-group message.
- Any consumer that keys decisions on the memory-limit message/code (e.g., a gateway deciding whether to retry in the same resource group vs upgrade) will intermittently mis-route.

## Suggested mitigations

- **Gateway side:** treat an ambiguous `CANCELLED` (no clear cause) as inconclusive and fall back to a conservative retry policy instead of assuming per-query exhaustion.
- **SR side (preferred fix):** make the status merge prefer `MEM_LIMIT_EXCEEDED` over `CANCELLED` rather than relying on arrival order — e.g., allow a `MEM_LIMIT_EXCEEDED` report to override a previously stored `CANCELLED` in `updateStatus`, or capture the first "real" (non-cancel) error separately from the cancellation status.

## Key references

- `be/src/runtime/mem_tracker.cpp:246-296` — BE message construction (`MemTracker::err_msg`).
- `DefaultCoordinator.java` `updateStatus` (~880-907) — first-writer-wins merge.
- `DefaultCoordinator.java` `getNext` (~949-997) — receiver status path + throw to user.
- `DefaultCoordinator.java` `updateFragmentExecStatus` (~1226-1236) — per-instance report path.
- `DefaultCoordinator.java` `cancel` (~1004-1018) — external cancel path.
- `DefaultCoordinator.java` `isInternalCancelError` (~734-736) — narrow cancel suppression that does not cover this case.
