# Shared old/new layouts: first migration preparation step

A layout describes which groups exist, which ownership rule applies, and how to
find each group's primary, replica, expected history ID, and local log files.
It is data for the application and migration tool to share, not the migration
itself. This milestone adds that definition and a read-only ownership preview.

## Try it

From the project directory:

```sh
cmake --build build-release
./build-release/kv_layout examples/layouts/old-modulo.layout examples/layouts/new-ring.layout color experiment-key-0 experiment-key-1
```

The example old layout describes two modulo groups. The new one describes three
ring groups with 64 positions each, new history IDs, new ports, and separate log
paths. The preview selects owners using the existing hashing/ring code:

```text
color: group-0 (:6380) -> group-0 (:6480)
experiment-key-0: group-1 (:6382) -> group-0 (:6480)
experiment-key-1: group-0 (:6380) -> group-0 (:6480)
```

These are calculations, not evidence that any values have moved. The command
opens only the two configuration files. It does not open log files or sockets,
create log directories, start servers, or modify the application's active routing.

## File format

```text
LAYOUT 1 fnv1a64-fmix64-ring-v1 64
GROUP "group-0" "new-group-0" 6480 6481 "data/new/primary-0.log" "data/new/replica-0.log"
GROUP "group-1" "new-group-1" 6482 6483 "data/new/primary-1.log" "data/new/replica-1.log"
```

The first line contains the file-format version, routing rule, and positions per
group. Supported routing rules are:

- `fnv1a64-mod-v1`: original FNV hash modulo the number of groups. Positions must
  be 1. Group line order defines the modulo index; the two-group example agrees
  with the current application's partition 0/1 routing.
- `fnv1a64-fmix64-ring-v1`: existing mixed hash and virtual-node naming rule.
  Group IDs determine positions; reordering GROUP lines does not change ownership.
  The positions count is part of the ownership definition, not a tuning knob to
  change on populated data without migration.

Each GROUP line contains, in order:

```text
GROUP "group ID" "history ID" primary-port replica-port "primary-log" "replica-log"
```

Group IDs identify owners. History IDs identify their replication histories; one
pair shares its ID, and separate pairs must have distinct IDs. The example source
IDs and paths are placeholders: they must be changed to describe the actual old
deployment before a future migration uses them. Merely loading a layout does not
check existing HISTORY/PARTITION headers, data, or server availability.

Paths may be absolute. Relative paths resolve against the layout file's directory,
not the shell's working directory. IDs and paths must be quoted, using the same
standard C++ quoted-string escaping convention as the store. Blank GROUP lines
are allowed; comments and additional fields are not supported.

This local milestone assumes loopback IPv4 endpoints. Each node needs a unique
port and log path. The parser validates IDs, distinct history IDs/ports/normalized
paths, port ranges, routing versions, and nonempty membership. Limits are 1–64
groups and 1–1024 ring positions per group, enough for the agreed local experiment
and bounded configuration sizes. Existing symlinks and lexical path aliases are
normalized, but this is not a complete migration path-safety check (for example,
future source/destination validation must also consider hard links).

## Code flow

1. `read_layout()` parses the file and resolves relative paths.
2. The `ClusterLayout` constructor validates the complete definition and builds
   the ring once when ring routing is selected. Group definitions are exposed
   through const references, avoiding accidental mutation of the cached mapping.
3. `owner(key)` applies modulo or ring lookup, returning the actual group
   definition including its endpoint and paths.
4. `kv_layout` loads old and new definitions and prints both owners for each key.

The ring implementation is reused; there is no second hashing algorithm hidden
inside the parser. `group(id)` provides an explicit lookup for node startup later.

## What is still next

The running client/server do not consume these layout files yet. The preview
accepts files for comparison; it is not a server configuration switch or a
migration/activation command.

The next implementation must integrate this definition with ownership enforcement
and persisted identity, read one recovered primary snapshot per old group,
transfer live values into fresh replicated destination groups, verify exact data,
and publish the new deployment only after success. Traffic must stay paused and
original logs intact throughout that offline operation. Source/destination
cross-checks, interruption handling and activation are not implemented here.

## Verification

```sh
ctest --test-dir build-release --output-on-failure
```

Layout tests cover agreement with original modulo routing and existing ring
lookup, ring membership ordering, endpoint selection, relative paths with spaces,
unknown groups/routing versions, malformed files, numeric limits, duplicate
identities/endpoints/paths, and missing files. Loading a valid configuration must
not create the referenced log directories.
