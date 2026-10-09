# GET and SET benchmarks

Build with the same optimization level for every comparison:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

In one terminal, start a dedicated server with a fresh temporary log:

```sh
benchmark_dir=$(mktemp -d)
./build-release/kv_server "$benchmark_dir/store.log" 6381
```

In another terminal, run:

```sh
./build-release/kv_benchmark 6381
```

Stop the server with Ctrl-C, then remove its temporary directory in the first
terminal using `rm -r "$benchmark_dir"`. Use a fresh directory/server for each run.
The benchmark writes 100 preparation keys: run it only against this dedicated
server, never a personal database. The optional port defaults to 6380; the address
is always loopback.

## SET workload and restart verification

Start a fresh dedicated server/log using the commands above, then run:

```sh
./build-release/kv_benchmark 6381 SET
```

SET prepares the same 100 keys, warms up with 100 SETs, and measures 100,000
SETs. Each request overwrites a key with `benchmark-write-0` through
`benchmark-write-99`, the same byte lengths as the GET workload's values.
These values differ from preparation values so final checks can detect a server
that acknowledged writes without updating the store. The live key count stays
at 100 on a fresh database. Each SET must return `OK`; all final values are checked
with GET outside timing, before printing results.

After the run, stop the server with Ctrl-C. Keep the log and restart the server
in the same terminal:

```sh
./build-release/kv_server "$benchmark_dir/store.log" 6381
```

Then, in the other terminal:

```sh
./build-release/kv_benchmark 6381 VERIFY
```

VERIFY only reads and checks all 100 expected final SET values. It does not
prepare keys, warm up, write, or report timings. Run it after restart before
deleting the temporary log. Stop the server and remove its temporary directory
when verification succeeds. Repeat the entire procedure three times with fresh
logs to collect SET results. GET remains the default workload; an explicit
`GET` argument also works. GET and SET measurements are reported separately.

The benchmark prepares 100 keys, warms up with 100 GETs, then measures 100,000 GETs
on one persistent connection. Requests and expected responses are prepared before
timing. Keys are `benchmark-key-0` through `benchmark-key-99` (15–16 bytes), and
values are `benchmark-value-0` through `benchmark-value-99` (17–18 bytes).

`steady_clock` measures elapsed time without calendar-clock adjustments. Each
latency includes sending, receiving the complete framed response, and checking
its value. Total duration runs from releasing the client threads until the last
completed response check; throughput is completed requests divided by that
duration. Setup, connection,
warm-up, sorting, and printing are excluded. Median is the average of the two
middle samples; p95 uses nearest rank, the 95,000th sorted sample here.

Connection setup has a five-second timeout. Blocking sends and receives have
five-second per-call timeouts; these are inactivity limits, not absolute request
deadlines. Failures exit with a nonzero status and do not print a successful
measurement. `main` owns the plain socket descriptors and explicitly closes them
on success and in its exception handler, after threads are joined. The warm-up and measurement use direct
loops, like the existing client. Nonblocking connection setup is kept only to
enforce the connection timeout; normal request handling uses blocking I/O.

## One client versus two

The optional last argument selects one or two clients (default one):

```sh
./build-release/kv_benchmark 6381 GET 1
./build-release/kv_benchmark 6381 GET 2
./build-release/kv_benchmark 6381 SET 1
./build-release/kv_benchmark 6381 SET 2
```

Run each case on a fresh dedicated server/log, at least three times. Predict the
effect on throughput and latency before running. Use the same restart/VERIFY
procedure for each SET run; VERIFY checks all 100 keys using one connection.

The total remains 100,000 measured requests, 100 keys, and 100 warm-up requests.
With two clients, each handles 50,000 requests, 50 keys, and 50 warm-up requests.
Client 0 owns keys 0–49 and client 1 owns keys 50–99. Separate key ranges avoid
interfering preparation or writes and preserve key/value sizes and total dataset.

Both connections, preparation, warm-up, and sample allocation finish before
timing. Requests are prepared once and shared read-only. Each client uses one
thread with its own socket and local protocol buffer. The sample vector is sized
before starting: each thread writes to a separate half (or the entire vector
with one client), so no resizing or sample-combining step is needed. Finish times
and errors use two-element arrays; no client wrapper struct is needed.
A ready latch tells main the threads have reached the start gate; a
second latch holds them until main records the start time and releases them.
The scheduler determines when each thread actually runs after release.

Main joins all threads, then checks their results. Combined throughput is
100,000 divided by the duration from release to the final completed request;
client durations are not added. Verification, joins, sorting,
and printing are outside timing. Errors in a client are saved and reported after
joining, with no successful result printed. Already-created threads are also
released and joined if creating another thread fails.

One-client runs now use the same thread/start mechanism as two-client runs.
Rerun one-client baselines with this version for the comparison; earlier results
below used the original single-thread measurement path. The comparison results
later in this document use the version supporting one or two clients.

## Findings so far (2026-10-08)

An initial 1,000-request run reported approximately 43,500 requests/s, a median
of 16.375 microseconds, and p95 of 35.708 microseconds. That run lasted only about
23 milliseconds. We increased the measured count to 100,000 to observe a longer
workload and compare variation across runs; the implementation was not optimized.

The three user-reported runs with 100,000 measured GETs were:

| Run | Duration (s) | Requests/s | Median (microseconds) | p95 (microseconds) |
| --- | ---: | ---: | ---: | ---: |
| 1 | 1.507 | 66,338.547 | 14.834 | 17.209 |
| 2 | 1.511 | 66,200.030 | 14.834 | 17.625 |
| 3 | 1.516 | 65,958.248 | 14.916 | 17.500 |

Throughput differed by about 0.6% between the lowest and highest runs. These
measurements give us a consistent initial baseline for this workload: roughly
66,000 GETs/s, median latency around 15 microseconds, and p95 around 17–18
microseconds. Microseconds are millionths of a second. p95 is a cutoff at which
at least 95% of measured requests finished, not an average of those requests.

The longer runs had higher throughput and lower p95 than the short run. This
does not demonstrate a code improvement: the measurement length changed. The
cause of the difference has not been established.

This is end-to-end latency, including the C++ client, loopback networking,
server scheduling, parsing, store access, response formatting, and validation.
It does not isolate hash-map lookup time, identify a bottleneck, or establish
maximum server capacity. Repeatedly accessing 100 keys after warm-up describes
a small, frequently reused dataset.

The server has two workers, but this workload uses one client connection and
therefore one server worker. The reproduction procedure above uses a Release
build and a fresh server/log for each run; those conditions were recommended,
but were not independently verified for the reported runs. Exact machine details
and the code revision used for these measurements have not yet been recorded.

## Initial SET results and learnings

The three user-reported runs with 100,000 measured SETs were:

| Run | Duration (s) | Requests/s | Median (microseconds) | p95 (microseconds) |
| --- | ---: | ---: | ---: | ---: |
| 1 | 3.270 | 30,577.022 | 31.584 | 37.750 |
| 2 | 3.373 | 29,650.913 | 31.875 | 39.458 |
| 3 | 3.539 | 28,257.310 | 32.375 | 43.042 |

These SET runs achieved less than half the GET throughput, with roughly twice
the median latency. This is consistent with SET doing additional persistence
work, but does not isolate its cost: SET also parses a larger request and formats
a different response. Opening, appending to, and flushing the log are candidate
costs to investigate, not proven bottlenecks.

The highest and lowest SET throughput differed by about 8% relative to the
lowest, compared with about 0.6% for GET. Each successive SET run was slower,
but three runs do not establish the cause. Fresh servers/logs between these runs
and successful restart verification have not been confirmed. If the same log
was reused, repeat with a fresh server/log per run before treating this as a
controlled comparison. Restart with each run's log and use VERIFY before cleanup.

What this experiment helped clarify:

- **Both GET and SET use the store mutex.** GET holds it while finding and copying
  a value. SET holds it while opening, appending to, and flushing the log and
  updating the map. With one client, another client's requests are not competing
  for that mutex; two-client tests will help explore contention.
- **Receiving a response is not a database GET.** The benchmark sends SET and
  receives `OK`; GET receives `VALUE`. Both are timed from sending until the
  complete response is received and checked. Final GET validation after SET
  is outside timing.
- **Operation count is different from live key count.** The measured workload
  performs 100,000 SETs across 100 keys, overwriting each key 1,000 times with
  the same updated value. On a fresh log there are 100 live keys but 100,200
  records: 100 preparation SETs, 100 warm-up SETs, and 100,000 measured SETs.
- **Correctness is part of a useful performance result.** Every SET must return
  `OK`, and final values must match. VERIFY reads those values after a restart
  to check recovery without writing or hiding missing data through preparation.
  It tests normal restart recovery, not power-loss durability.

## One-client versus two-client results

All runs below completed 100,000 measured requests with 100 total keys and
100 total warm-up requests. Two clients split these totals evenly. GET and SET
ran separately. These are user-reported results with the concurrent benchmark;
the earlier one-client measurements above are retained as historical baselines.

| Workload | Clients | Run | Duration (s) | Requests/s | Median (microseconds) | p95 (microseconds) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| GET | 1 | 1 | 1.610 | 62,115.809 | 15.250 | 20.000 |
| GET | 1 | 2 | 1.504 | 66,482.799 | 14.667 | 16.958 |
| GET | 1 | 3 | 1.507 | 66,354.572 | 14.833 | 17.000 |
| GET | 2 | 1 | 0.914 | 109,424.589 | 17.500 | 24.417 |
| GET | 2 | 2 | 0.873 | 114,606.580 | 17.042 | 22.917 |
| GET | 2 | 3 | 0.887 | 112,721.621 | 17.292 | 23.208 |
| SET | 1 | 1 | 3.145 | 31,800.846 | 30.958 | 36.792 |
| SET | 1 | 2 | 2.943 | 33,977.816 | 28.834 | 33.292 |
| SET | 1 | 3 | 2.992 | 33,426.678 | 29.125 | 34.167 |
| SET | 1 | 4 | 3.244 | 30,825.795 | 29.500 | 39.875 |
| SET | 2 | 1 | 2.109 | 47,414.792 | 41.333 | 48.292 |
| SET | 2 | 2 | 2.093 | 47,770.020 | 40.500 | 48.084 |
| SET | 2 | 3 | 2.054 | 48,688.256 | 40.709 | 45.542 |

All four reported SET/1 runs are included rather than selecting the best three.
No implementation optimization is being claimed between these cases; the
variable being compared is client count.

### Findings

- **Two clients increased combined throughput for both workloads.** The median
  of the three GET throughput results rose from 66,354.572 to 112,721.621
  requests/s, about a 70% increase. For SET, the median throughput across the
  four one-client runs was 32,613.762 requests/s (the average of the two middle
  results), versus 47,770.020 with two clients, about a 46% increase. These are
  medians across run-level throughput results, distinct from request latency.
- **More throughput came with longer individual request waits.** GET request
  medians increased from 14.667–15.250 to 17.042–17.500 microseconds, and p95
  from 16.958–20.000 to 22.917–24.417. SET medians increased from 28.834–30.958
  to 40.500–41.333 microseconds, and p95 from 33.292–39.875 to 45.542–48.292.
  Finishing the combined workload faster does not mean each request got faster.
- **Scaling was below doubling, and SET scaled less than GET.** Workers can
  overlap network waits, but both use the same store mutex. SET holds it through
  log opening, appending, flushing, and map mutation; GET holds it for lookup
  and value copying. This is a plausible explanation for the scaling difference,
  not a measurement of lock contention. Client scheduling and other shared
  resources may also contribute.
- **Repeated runs show variation.** Relative to each case's minimum throughput,
  the full range was about 7.0% for GET/1, 4.7% for GET/2, 10.2% for SET/1, and
  2.7% for SET/2. Preserve that variation when comparing an optimization.

### Conditions and correctness checks

These remain preliminary comparisons. A fresh server/log for every case and run
has not been confirmed; the supplied client-terminal output alone does not show
server lifecycle commands. Some cases were run consecutively, so equivalent
starting log sizes cannot be assumed. Exact machine specifications and code
revision for these runs also remain unrecorded. Use the same Release build and
fresh logs when collecting a controlled before/after comparison.

Every successful benchmark run checked its responses. SET additionally checked
all 100 final values in memory before printing results. One restart VERIFY failed;
the shown server commands created a new temporary directory before restart, so
that check used a different log and does not demonstrate a persistence defect.
Restart verification with the original logs for this comparison remains pending.
VERIFY must run after restarting with the same path, before running another
benchmark that prepares and overwrites those keys.

### Next experiment

Investigate one specific cost before changing implementation. Opening and closing
the log for each SET is a concrete candidate for a small timing experiment or
profile. These results do not establish that it dominates performance. If evidence
supports keeping the log open as an optimization, preserve write ordering, flush
checks, and correct handling of compaction, then repeat the same four workloads
with equivalent starting conditions. Normal-restart checks remain required for
any write optimization we keep.
