# Distribution results: fixed partitions (7A)

Measured 2026-10-26 on the development Mac. Two replicated groups improved SET
throughput by **35.4%** for this workload. GET throughput was essentially unchanged.
This covers fixed partitions, not consistent hashing or migration (7B).

## Comparison and method

```text
One group:   client 0 ─┐
                      ├→ one primary → one replica
             client 1 ─┘

Two groups:  client 0 → primary 0 → replica 0
             client 1 → primary 1 → replica 1
```

Both used the same Release binaries, 100 keys, two closed-loop client threads,
and **100,000 total measured requests** per run. Each client waited for its reply
before its next request: at most two measured requests were outstanding. Each
client used the same 50 keys and request order in both configurations. One group
received all 100,000 requests; two groups received **50,000 each**. This was an
intentionally balanced workload, not a skew experiment.

Every successful SET waited for its own replica's numbered ACK. GETs read the
primary without contacting its replica. One-group nodes were unpartitioned;
two-group nodes additionally enforced ownership using FNV-1a 64-bit modulo two.
Each primary had two worker threads. One group used two server processes;
two groups used four. All processes shared the same laptop, OS, and storage.

There were three runs per configuration per workload: **12 runs total**. Within
each workload, configuration order was one/two groups, then two/one, then one/two.
All runs used fresh processes and logs. No user servers or existing logs were used.

Environment: Apple M3 Pro, 11 logical CPUs, 18 GiB RAM, macOS 14.1 arm64,
Apple Clang 15.0.0, CMake 4.1.1, Release build, loopback TCP. Background application
load, CPU frequency, and scheduling were not controlled.

## Timing boundaries

The existing benchmark gained a small two-partition mode. Both comparison sides
use two persistent client connections. One-server mode remains available.
For comparable workloads, both two-client modes now group keys by hash instead
of the old contiguous-index assignment. Historical measurements are therefore
not a controlled comparison with this run.

Before timing: connect, prepare 100 keys with SET, precompute requests and key
assignments, perform 100 total warm-up requests, allocate samples, start threads.
A latch releases both measured loops together. Latency covers sending, receiving
a complete reply, and validating it. Total duration ends at the last completed
request. Sorting samples and final SET verification happen afterward.

**Connection setup, startup replay/catch-up, preparation, and routing hash
calculation are excluded from timing.** Routing assignments are precomputed.
This measures service through persistent connections, not the interactive
`kv_client`, which currently opens one connection per routed request.
Neither configuration gets a connection-setup advantage over the other.

## Results

Arithmetic means of three run results:

| Workload | One group: requests/s | Two groups: requests/s | Change | Median latency: one → two (µs) | p95: one → two (µs) |
| --- | ---: | ---: | ---: | ---: | ---: |
| GET | 109,720 | 109,619 | −0.1% | 17.611 → 17.750 | 24.264 → 23.736 |
| SET | 30,751 | 41,648 | +35.4% | 63.916 → 46.917 | 73.403 → 55.459 |

Latency columns are means of run medians/p95s, **not pooled percentiles**.
Mean SET duration fell from 3.252 s to 2.401 s (26.2% less time).
Mean run median SET latency fell 26.6%; mean run p95 fell 24.4%.

### Captured individual runs

| Workload | Groups | Trial | Duration (s) | Requests/s | Median (µs) | p95 (µs) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| GET | 1 | 1 | 0.931 | 107,453.330 | 17.833 | 24.750 |
| GET | 1 | 2 | 0.892 | 112,161.933 | 17.375 | 23.833 |
| GET | 1 | 3 | 0.913 | 109,545.058 | 17.625 | 24.209 |
| GET | 2 | 1 | 0.933 | 107,148.650 | 18.000 | 24.375 |
| GET | 2 | 2 | 0.904 | 110,643.820 | 17.666 | 23.375 |
| GET | 2 | 3 | 0.900 | 111,063.338 | 17.584 | 23.458 |
| SET | 1 | 1 | 3.258 | 30,694.762 | 63.625 | 74.083 |
| SET | 1 | 2 | 3.261 | 30,661.355 | 64.208 | 73.375 |
| SET | 1 | 3 | 3.237 | 30,895.860 | 63.916 | 72.750 |
| SET | 2 | 1 | 2.408 | 41,525.880 | 47.125 | 55.709 |
| SET | 2 | 2 | 2.410 | 41,491.272 | 46.750 | 55.250 |
| SET | 2 | 3 | 2.385 | 41,927.982 | 46.875 | 55.417 |

[Raw evidence](distribution-benchmark-raw.json) includes stdout, actual commands
and ports, environment details, per-group counts, verification output, matching-log
checks and SHA-256 hashes. Temporary logs were deleted after validation. The JSON
records the base commit and working-tree changes because the benchmark extension
was uncommitted when measured.

## Interpretation

One primary serializes local mutation and the replica exchange under its
replication mutex. Two clients share that ordered path. Two groups have separate
primaries, locks, logs and replica connections, allowing independent progress.

The SET improvement is consistent with that design, but the experiment does not
isolate mutex, filesystem, network or scheduling costs. It changes the number of
processes as well as where contention occurs. This is not a bottleneck profile.

GET did not benefit measurably: its throughput difference was much smaller than
variation across runs. GET already bypasses the replica exchange. With only two
outstanding requests on one laptop, adding groups may offer little benefit;
finding the limiting resource requires a separate experiment.

Two groups gave **1.35× SET throughput, not 2×**. They share hardware resources,
and we used only two clients. This does not predict maximum capacity, behavior
on separate machines, more clients, large datasets, skew, or stronger durability.
Partitioning can still spread storage and isolate failures without speeding up
every workload. Three repeats are a small local experiment, not a confidence
interval or proof of a universal speedup.

## Correctness checks

Every measured reply was validated. After every SET run, all 100 final values
were also read directly from replicas using VERIFY. In every group, primary and
replica logs matched byte-for-byte, sequence numbers were consecutive, and the
number of distinct keys was correct.

| Workload | One-group log: mutation records | Two-group logs: records per group |
| --- | ---: | ---: |
| GET | 100 | 50 |
| SET | 100,200 | 50,100 |

GET logs contain only preparation SETs. SET logs include 100 preparation writes,
100 warm-up writes, and 100,000 measured writes in total. Headers and reads add
no mutations. Together with successful responses, these checks confirm that the
second configuration distributed actual work to both groups.

All six CTest suites passed after the benchmark changes. Integration tests cover
wrong-owner rejection, concurrent clients and matching replication order, both
groups restarting, and replica failure in one group while the other stays usable.
Those are separate correctness tests; performance trials did not inject faults
or measure recovery time.

These traces are **illustrative descriptions of tested behavior**, not captured
benchmark transcripts:

- Normal: SET for `color` → owner 0 → primary 0 applies/logs → replica 0 applies
  and ACKs → client receives OK. Group 1 receives no mutation.
- Restart: reuse original files, partition flags and expected history IDs.
  Replicas replay; primaries replay and verify/catch up their replicas before
  serving requests. Existing values survive and new mutations continue numbering.
- Failure: stop replica 0. The next group-0 SET can apply locally but reports
  REPLICATION_UNCERTAIN. Further writes are rejected, local reads remain possible,
  and group 1 continues writing. Restart group 0 with saved logs for catch-up.

Ordering and ownership are per group. There are no cross-partition transactions,
cluster snapshots, automatic retries, election, failover or automatic reconnection.
Replicas allow ordinary reads and reject ordinary writes. Expected history IDs
and saved partition/routing headers catch wrong log/configuration pairings.
Stream flushing is **not an fsync or power-loss guarantee**, and copies on one
laptop do not protect against losing that laptop.

## Reproduce

From the project directory:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
python3 scripts/compare_distribution.py --output /tmp/distribution-benchmark-new.json
```

The macOS runner reuses existing process-test helpers, selects local ports, starts
replicas before primaries, validates results, and cleans up its processes and
temporary files. It records each completed trial to the specified output. Run
without other benchmarks or heavy work for a cleaner comparison.

For an already running topology, equivalent benchmark commands are:

```sh
# One unpartitioned primary/replica group; two clients share this primary.
./build-release/kv_benchmark 6380 SET 2

# Two partitioned primary/replica groups; specify the two primary ports.
./build-release/kv_benchmark --cluster 6380 6382 SET

# Untimed verification: specify the two replica ports after a SET run.
./build-release/kv_benchmark --cluster 6381 6383 VERIFY
```

Replace SET with GET for read measurements. Controlled repeats use fresh logs;
recovery checks instead reuse the original logs. See the
[requirements](distribution-requirements.md) for the remaining 7B migration work.
