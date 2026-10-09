# One-client GET and SET benchmarks

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
its value. Total duration also includes the loop and recording samples;
throughput is completed requests divided by that duration. Setup, connection,
warm-up, sorting, and printing are excluded. Median is the average of the two
middle samples; p95 uses nearest rank, the 95,000th sorted sample here.

Connection setup has a five-second timeout. Blocking sends and receives have
five-second per-call timeouts; these are inactivity limits, not absolute request
deadlines. Failures exit with a nonzero status and do not print a successful
measurement. `main` owns a plain socket descriptor and explicitly closes it on
success and in its exception handler. The warm-up and measurement use direct
loops, like the existing client. Nonblocking connection setup is kept only to
enforce the connection timeout; normal request handling uses blocking I/O.

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

Next, confirm equivalent starting conditions and restart checks for SET, then
compare one client with two for GET and SET. Investigating a bottleneck and
trying one measured optimization remain later steps.
