# Part 5: Performance

## What We Are Trying To Do

Find out how fast our current server is, understand where it spends time, and try
one small improvement based on evidence.

We are not trying to beat Redis or reach a particular requests-per-second target.
A useful result explains what was measured and why the numbers changed.

## What You Will Learn And Why It Matters

### Throughput: How Much Work Gets Done

Throughput is the number of completed requests per second.

If 1,000 requests finish in 2 seconds, throughput is 500 requests per second.
This tells us how much work the system handles under a particular workload.

### Latency: How Long One Request Takes

Latency is the time from sending a request until receiving its complete response.
For a user, it answers: "How long did I wait?"

Measure it with a monotonic clock: a clock intended for elapsed-time measurement
that does not jump when the computer's calendar time changes.

Our measurement includes the client, loopback networking, command parsing, store
access, and response delivery. It is not just the time spent inside the hash map.

### Median And p95: Typical Waits And Slower Waits

Sort the measured request times from fastest to slowest.

- The median is the middle value: a useful picture of a typical request.
- p95 is the 95th percentile: about 95% of requests finish within this time.

An average alone can hide slower requests. Record how the benchmark calculates
p95 so the same method is used before and after a change.

### Bottlenecks: What Limits Progress

A bottleneck is the part of the system that limits how much work it can finish.
Possible causes in this project include disk writes, lock contention, parsing,
network overhead, or the benchmark client itself.

Lock contention means threads have to wait for a lock another thread holds.
Adding threads does not guarantee more throughput if they spend their time
waiting for the same database lock.

### Experiments: How To Know A Change Helped

First measure the current version. That result is the baseline.
Then change one thing and repeat the same measurement.

This makes optimization a question you can test instead of a guess. A change
that does not help is still useful evidence; you do not need to keep it.

## Assumptions For This Phase

- Run the client and server on this computer using loopback TCP.
- Keep the existing two workers and sixteen waiting slots.
- Use small, fixed keys and values within the existing protocol limit.
- Use one or two active clients; queue-overload testing belongs to concurrency.
- Keep each connection open for many requests.
- Each client sends one request, waits for its response, then sends the next.
- Use a temporary log and a dedicated test server, not your personal log or server.
- Keep the machine reasonably quiet. Local results are approximate and workload-specific.

## Step 1: Measure GET With One Client

Write a small benchmark client. Python and its standard library are sufficient;
you can focus on measurement without learning more C++ socket code here.

Suggested starting workload: populate 100 keys and measure 1,000 GET requests,
cycling through those keys. These numbers are starting points, not mandatory targets.

The benchmark must:

- Set up the keys before measurement starts.
- Perform a short, fixed warm-up before measurement, such as 100 requests.
- Exclude server startup, connection setup, key preparation, and warm-up from timing.
- Time each request until its complete newline-terminated response arrives.
- Check that the returned value is correct.
- Report completed request count, total duration, requests per second, median, and p95.
- Have finite connection and response timeouts so a failure does not hang the experiment.

Use the existing protocol. A TCP receive call is not guaranteed to return a whole
response, so the benchmark must read until its framing newline arrives.

This first step is enough to start implementing. Do not build the whole phase at once.

## Step 2: Compare Reads And Writes

Run a separate SET workload with the same number of requests and similarly sized
keys and values. Overwrite a fixed set of keys so the number of live keys stays stable.
Verify successful responses and check the expected final values outside the timed run.

GET reads memory. SET also appends to and flushes the persistence log. Comparing
them helps you investigate the cost of different operations, but a slower SET
alone does not prove which individual step caused the difference.

Keep GET and SET results separate so the workload is easy to interpret.

## Step 3: Compare One Client With Two

Repeat both workloads with two clients running concurrently. Prepare both
connections before timing and coordinate their starts with a barrier or event.

For the first comparison, keep the total measured request count the same and split
it evenly between clients. Give each writer its own keys to keep final checks simple.

Calculate combined throughput using all completed requests divided by the elapsed
time from releasing the clients to the final completed request. Do not add the
clients' separate durations together. Combine their request samples for latency.

Predict the result first: will throughput improve, stay similar, or get worse?
After measuring, explain how shared locks and overlapping network waits might
contribute. Present explanations as hypotheses unless you have measured the cause.

## Step 4: Make Results Repeatable

Run each of the four cases at least three times with equivalent starting data.
Use a fresh temporary log for each run so earlier writes do not change later setup.

Save a short report in `docs/performance-results.md` containing:

- The code revision, machine, build type, worker count, and request/key/value sizes.
- The exact commands used to reproduce the measurements.
- The results for each run and any noticeable variation.
- What you think limits performance and what evidence supports that idea.

Use the same build type for comparisons. A Release build is a sensible choice
for timing, but do not compare an unoptimized baseline with an optimized build
and attribute the difference to your code change.

If results vary a lot, increase the request count or reduce background activity
before drawing conclusions. Record variation rather than selecting the best run.

## Step 5: Investigate And Try One Improvement

Choose one hypothesis from the results. Read the relevant code, add a small timing
experiment, or use one profiler if needed to check it. A profiler helps show where
execution time goes; a full monitoring system is unnecessary for this phase.

Try one small change that addresses the observed cost. Explain what work it saves,
rerun the same workloads, and record before/after results. Run the existing storage
and network tests after changing implementation code.

Keep existing correctness guarantees:

- Keep the required store and connection-state synchronization.
- Keep memory changes and persistence records in a consistent order.
- Keep successful mutations recoverable after a normal restart.
- Keep protocol behavior, request limits, timeouts, and socket ownership intact.

Removing locks or delaying the required flush can make numbers look better by
changing the guarantees. Such tradeoffs may be discussed, but are outside this phase.
After a write experiment, restart from the benchmark log and check final values
before deleting it.

## What We Are Leaving Out

No distributed load generator, dashboard, automatic scaling, new caching layer,
lock-free structure, or full CPU/memory/disk/network profiling suite is required.
The store already holds its live data in memory; adding another cache needs a
specific reason. Batching is also optional because it can change response timing
and persistence behavior.

Benchmark failures must be visible. Fail the run on errors, rejected connections,
wrong responses, or timeouts instead of treating those requests as successful work.

## When This Phase Is Done

You have a reproducible report for GET and SET with one and two clients, can explain
throughput and latency, and have investigated one likely bottleneck. You have tried
one focused improvement and recorded its result, even if it did not help and was
reverted. Existing tests pass for any implementation changes you keep.

The next planned phase is replication: sharing state between separate database nodes.
