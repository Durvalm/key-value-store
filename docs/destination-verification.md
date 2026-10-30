# Exact destination verification

After copying, check that the new deployment contains exactly the source's live
key/value pairs, on both primaries and replicas. A successful transfer proves that
writes received acknowledgments; this independent check verifies the resulting data.

## Run it

Keep normal client traffic paused. All source nodes must be stopped, and all
candidate destination replicas and primaries must be running with their new layout.

```sh
cmake --build build-release
./build-release/kv_transfer /path/to/source.layout /path/to/destination.layout
./build-release/kv_verify /path/to/source.layout /path/to/destination.layout
```

Verification can also run again after destination restart. It does not require
fresh destination logs or an active IMPORT connection.

Example output:

```text
Verified 100 live keys on all destination primaries and replicas.
group-0: 32 keys verified on both copies
group-1: 35 keys verified on both copies
group-2: 33 keys verified on both copies
Verification is read-only; no layout was activated.
```

A failure exits nonzero without printing a successful verification report.

## Flow and reasoning

1. Validate source/destination history and filesystem separation, using the same
   checks as transfer. Lock and replay all stopped source logs, compare each
   primary/replica history, then collect the sorted unique live dataset.
2. Ask each destination node to confirm its expected history, ownership layout,
   role, configured primary/replica ports and serving log path. Record its current
   sequence. Each primary and replica must initially report equal progress.
3. Use the destination layout to choose each key's new owner. GET that key from
   both copies and compare the exact value with the source.
4. Check SIZE on every node, including groups expected to be empty.
5. Recheck all node identities and sequence numbers after all reads. Reject any
   sequence change during the check.

For example, if a group should contain `color=blue` and `size=large`, reading those
exact values proves both expected keys exist. SIZE must also equal 2. A leftover
third key would fail the count check. Applying this to every group detects missing,
wrong, extra and misplaced data; checking only the total key count would not.

The verifier compares logical state, not the source's journal bytes. Destination
histories have new IDs and sequences; old overwrites and deletions are absent from
the imported live dataset. Matching contents do not require matching source counters.

## Protocol and code

`verify_main.cpp` keeps an `OfflineSources` guard alive throughout the operation.
`destination_verification.cpp` groups expected reads by owner, checks every endpoint,
and closes all sockets on success or failure. Network operations retain the existing
finite connection and I/O timeouts.

A read-only `VERIFY` control message carries the expected identity. The server
returns `VERIFY_READY <sequence>` only when it matches its own configuration. This
works on either role without reserving an import or replication session. Ordinary
GET and SIZE commands perform the data checks. The shared exchange helper accepts
a larger bounded response for GET values; short control replies keep their old limit.

Tests cover modulo-to-ring and ring-to-ring transfer followed by verification,
verification after restart, empty datasets, large quoted/UTF-8 values, incorrect
values, missing/extra keys, identity mistakes and unchanged log bytes/timestamps.
Deterministic protocol peers additionally test replica-only discrepancies, unequal
progress, malformed counters and a sequence change during verification.

## Limits and next step

This is an offline operator check. Source locks cover the command's lifetime;
sequence checks detect accepted destination mutations during the checked interval.
They do not prevent writes after the command finishes. Keep traffic paused through
activation, and do not restart or edit sources between transfer and verification.

Verification does not change layout files, release import reservations, save an
activation marker or publish a topology. The remaining step is controlled activation:
record the verified destination layout, move all clients to it together while traffic
is still paused, and only then resume traffic. Preserve the old logs until acceptance.

The dataset remains in memory and requests run sequentially. Authentication, online
migration and resumable verification are deferred. Normal log flushing still does
not promise survival of power loss.
