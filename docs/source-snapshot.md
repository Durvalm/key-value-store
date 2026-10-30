# Offline source snapshot

This is the first data step for migration: recover the current logical data from
all old groups, without modifying their logs or transferring anything yet.

## Run it

Build the current binaries:

```sh
cmake --build build-release
```

Prepare an accurate source layout: its paths, group IDs, expected history IDs and
routing must describe the existing deployment, not the fresh destination. The
example old-layout file contains placeholders; replace those with your real values.

Pause client traffic. Start/recover each primary/replica pair with the same logs
if needed so startup catch-up finishes. Wait for in-flight writes to finish, then
stop every primary and replica. Do not generate fresh log paths for this step.

```sh
snapshot_dir=$(mktemp -d)
./build-release/kv_snapshot /path/to/source.layout "$snapshot_dir/source.snapshot"
cat "$snapshot_dir/source.snapshot"
```

Use a new output filename: the command refuses to overwrite any existing file,
including a previous snapshot, layout, source log, symlink or hard link.

## What it does

1. Require every configured primary and replica log to exist as a regular file.
   Detect repeated physical files, including hard-linked aliases.
2. Acquire exclusive, nonblocking log locks and hold them together throughout
   collection. A running node makes collection fail rather than read changing data.
3. Replay each log read-only, validating ownership, expected history ID, mutation
   syntax and consecutive sequence numbers. Existing two-group PARTITION logs
   work with the equivalent modulo layout, as they do in normal layout startup.
4. Compare each pair's complete canonical mutation history. Matching counters
   alone are insufficient; differing records or a behind/ahead replica reject
   collection. The command does not catch replicas up or repair source histories.
5. Copy the primary's current map once per group and combine the live pairs.
   Overwrites keep the newest value; deletes stay absent; recreated keys keep
   their latest value. Replica maps are not counted again.
6. Sort entries by key, reject duplicate ownership, and export the verified copy.
   Report source history/progress/counts for inspection and later validation.

This version loads the histories and combined dataset into memory, matching the
learning store's existing history-cache approach; it is not a streaming export.

A synchronized empty group is valid, including one whose keys have all been
deleted. A missing or zero-byte log is not treated as a fresh empty database.

The C++ API is `collect_source_snapshot(layout)`, returning a detached
`SourceSnapshot` with source progress and sorted entries. It reuses
`KeyValueStore::snapshot()`, which copies the current map under its mutex.
The read-only constructor reuses the existing replay logic but never initializes
new headers, creates missing logs, or opens an append stream. This read-only store
mode is for offline inspection; running replicas still need writable stores for
peer records while ordinary replica clients remain read-only.

## Example export

Illustrative, not a captured session:

```text
SNAPSHOT 1 fnv1a64-mod-v1 1 "group-0" "group-1"
SOURCE "group-0" "history-0" 12 1
SOURCE "group-1" "history-1" 8 1
DATA
SET "color" "blue"
SET "full name" "Durval"
END 2
```

- SNAPSHOT records the format version and source ownership definition.
- SOURCE records the group ID, history ID, last sequence, and live-key count.
- DATA starts sorted, quoted SET records describing only current values.
- END records the total live-key count.

The SET lines are logical data, not a numbered log. Old sequence numbers are
provenance metadata and must not be spliced into fresh destination histories.
A parser/importer for this export is not part of this step; the next migration
implementation can reuse the in-memory collection API directly.

Collection finishes and validates all groups before output creation. The writer
checks serialization, partial writes and close errors. It removes a partial
output on failure. Treat the export as complete only after the command succeeds;
it is not an atomic publication of a new cluster layout or an fsync guarantee.

## Why there are .lock files now

Every file-backed store now holds a process-level `flock` through a small
`LogLock` lifetime guard. Closing or killing the owning process releases the
kernel lock, even though the empty sidecar file remains. This is separate from
our in-process `std::mutex` protecting worker threads.

The sidecar is named `<canonical-log-path>.lock`. Locking the log itself would
lose protection when compaction replaces that file's inode, so the stable sidecar
is necessary. Stores close their append stream before releasing the lock.
Snapshot inspection creates/reuses these sidecars but preserves source-log bytes
and modification times. Lock files are ignored by Git for `.log` paths.

Do not delete/replace lock files while a server or snapshot is running. Their
presence is not evidence that a process is still alive. Locks are advisory:
updated binaries cooperate, but older already-running binaries or manual file
edits do not. Stop all source processes before this command and avoid modifying
source files externally. The source directory must allow creating/reusing locks.

The standalone snapshot command holds locks during collection, then releases them
when the detached copy is returned. The new [transfer command](destination-transfer.md)
uses an `OfflineSources` lifetime guard to retain the same locks through copying.
Neither command imposes an ongoing maintenance window after it exits.
If source traffic resumes, the export can become stale. The later migration must
keep traffic paused until transfer, verification and activation finish; a saved
snapshot must not be assumed to represent subsequently changed sources.

## Failure and recovery

If histories differ, restart the existing pair for normal startup verification
and catch-up, then stop both nodes and retry with the original logs. Conflicting,
malformed or incomplete histories still require investigation; this tool does not
merge or repair them. A failed collection creates no output file.

If output creation/writing fails, choose a fresh writable destination and retry.
The original logs are retained. This snapshot command does not start servers, send data to new groups, or activate
a topology. The separate [transfer step](destination-transfer.md) now sends the
fresh captured values through destination primaries; verification/activation follow.

## Verification

```sh
ctest --test-dir build-release --output-on-failure
```

Tests cover local snapshot copies, overwrite/delete/recreation, empty logical
state, metadata/progress, matching full histories, read-only rejection, lock
release after errors, compaction retaining its lock, missing/empty/aliased sources,
existing-output refusal and source preservation. Process tests exercise real
modulo and ring deployments, live primary/replica refusal, a competing server,
read-only export, byte/mtime preservation, unsynchronized groups, and a forced
output-write failure with partial-file cleanup.
