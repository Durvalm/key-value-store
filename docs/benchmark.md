# First GET benchmark

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

This first milestone measures GET only. Next, compare a SET workload with
similarly sized keys and values, then compare one client with two. Investigating
a bottleneck and trying one measured optimization remain later steps.
