# Offline destination transfer

This milestone copies the verified live source data into fresh replicated groups
under the destination layout. It supports modulo-to-ring and ring-to-ring changes.
It does not publish an active layout or declare the destination fully verified.

## Run it

```sh
cmake --build build-release
./build-release/kv_transfer /path/to/source.layout /path/to/destination.layout
./build-release/kv_verify /path/to/source.layout /path/to/destination.layout
```

Before running:

1. Pause normal client traffic. Recover/catch up each old pair if necessary, then
   stop every source primary and replica using the original files.
2. Prepare a destination layout with distinct new history IDs and separate log
   paths. Do not point a destination at an original log or lock file.
3. Start fresh destination replicas, then primaries, with that destination layout.
   Their logs must contain only their OWNERSHIP and HISTORY headers. Existing data,
   even data subsequently deleted, makes a history nonfresh.
4. Keep destination client traffic paused too. The command is the only writer.

The [layout guide](cluster-layout.md) shows how to start candidate groups. Its old
example has placeholder source paths/history IDs: use a layout describing your
actual source. The two layouts may use the same stable group IDs; destination
history IDs must be separate from every source history. Source endpoints can be
reused once those processes are stopped; log files must remain separate.

There is no need to export a snapshot file first. The transfer command collects a
fresh in-memory SourceSnapshot while retaining its source locks through the entire
operation. This avoids relying on an exported snapshot that might have become stale.
The standalone `kv_snapshot` command remains useful for inspecting source state.

## Flow

```text
Validate source/destination separation
    ↓
Lock stopped sources, replay and compare their paired histories
    ↓
Copy primary live values into a detached snapshot
    ↓
Check all SET sizes and destination header-only logs
    ↓
Connect to all destination primaries and reserve fresh import sessions
    ↓
For each key: new layout chooses owner → SET → replica ACK → OK
    ↓
Report acknowledged counts; release sockets and source locks
```

The tool rejects shared history IDs, overlapping log/lock paths, and physical-file
aliases such as hard links. Source validation still requires every source log to
exist, match the source layout/history, and have a synchronized counterpart.
Destinations must already have the expected fresh canonical headers, and every
serialized SET must fit the existing 4096-byte request protocol. These checks
happen before any destination data writes.

## Why the IMPORT handshake exists

Each primary receives an internal control message containing the expected history
ID and full ownership header. Under its existing replication mutex, it checks:

- It is a layout-managed primary with the expected identity.
- Its sequence and live-key count are zero, and replication has not failed.
- No other importer has already reserved this process.

It then returns READY_IMPORT and reserves that connection. **Every destination
must be ready before the first SET is sent.** A later group's refusal cannot leave
new data writes on earlier groups, although earlier empty reservations remain.

The importer sends normal SET commands through that reserved connection. The
existing local-persist/apply → replicate → numbered ACK flow remains unchanged.
A successful OK therefore has the same acknowledgment policy as an ordinary
replicated write. Destination sequence numbers start at 1 independently per group;
source sequence numbers and old deleted/overwritten values are not imported.

Other connections can still read, but their writes return IMPORT_IN_PROGRESS.
The reservation remains after the tool disconnects, until that primary process
is restarted. It prevents concurrent importers or accidental ordinary writers
from mixing data into the copy. It is not authentication or a persistent
activation marker; restarting a process clears the in-memory reservation.

## Completion and failure

Success reports the total number of acknowledged SETs and counts per group. It
means the copy operations were acknowledged, not that an independent exact
source/destination comparison or topology activation has occurred.

On any rejected response, disconnect, timeout or uncertain write, the command
stops, closes its connections, and releases the source locks. It does not retry:
a missing response may follow a write already applied locally or on both copies.
It does not roll back or delete destination data, modify source logs, or change
layout files. Some destinations may hold a partial copy after failure.

If a preflight fails before any writes, restart any empty reserved destination
pairs before retrying. If writes have begun, stop the candidate nodes and prepare
fresh destination histories/files for another attempt from the intact sources.
The tool refuses to append a repeated transfer to a nonfresh history.

Do not start normal clients on the candidate layout after copying. Keep the
maintenance window in place for exact destination verification and controlled
activation. Source locks protect cooperating binaries during the command, not
indefinitely after it returns. The [activation guide](layout-activation.md) covers
candidate restart, fresh verification and atomic active-layout publication.
Candidate process restart alone is not a verified
migration or a safe publication step.

## Important code

- `OfflineSources` owns read-only source stores so their locks survive snapshot
  creation and remain held while network writes run.
- `validate_transfer_layouts` checks lineage and filesystem separation.
- `transfer_snapshot` precomputes owner requests, checks freshness, reserves all
  targets, sends sequential SETs, validates replies and returns counts.
- `line_connection` contains the existing finite-timeout connection/exchange
  code factored out of replica communication so the importer can reuse it.
- `server_main` uses its replication mutex to make freshness checking and import
  reservation atomic with respect to other primary commands.

This is a local-filesystem, loopback, whole-dataset-in-memory learning milestone.
The importer is sequential, not a throughput benchmark. Actual snapshot-file
import, resumable copies and online migration are not added. The separate
`kv_activate` command provides explicit offline publication, not automatic cutover.

## Verification

```sh
ctest --test-dir build-release --output-on-failure
```

Tests transfer modulo 2 → ring 3 and ring 3 → ring 4, including quoted/UTF-8 keys,
overwrite/delete/recreation, per-group new sequences, matching logs, primary and
replica values/counts, and destination restart. They reject shared histories,
path/lock/hard-link overlap, malformed identity, nonfresh logs and oversized SETs.
A later-group preflight refusal is checked before any earlier-group mutations.
A timed proxy stops a real replica after the first acknowledged SET: the second
write reports uncertainty, no retry/later write occurs, sources stay unchanged,
and their locks release on failure. The proxy also confirms sources remain locked
after capture while copying is in progress. These are automated correctness checks;
the separate [verification command](destination-verification.md) now performs
independent exact checks for an operator's particular transfer.
