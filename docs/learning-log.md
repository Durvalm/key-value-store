# CS Learning Log

Things explored while building the key-value store. These are takeaways to revisit,
not claims that every topic is mastered.

## Storage and Recovery

- **Memory and disk serve different purposes.** The map supports fast lookups; the file lets the data survive a process restart.
- **An append-only log records changes.** SET and DELETE change state; GET does not need to be logged to reconstruct the database.
- **Replay rebuilds state from history.** Startup applies the recorded changes in order to reconstruct the in-memory map.
- **Compaction keeps the result instead of the whole history.** Writing the current live keys to a temporary file and replacing the old log reduces future replay work.
- **Persistence has levels of guarantees.** Flushing a language-level stream does not necessarily mean data has reached durable storage. Surviving a process restart is different from surviving power loss.

## Processes and the Operating System

- **A process is a running program with its own address space and resources.** The client and server are separate processes, even on the same computer.
- **The kernel manages shared machine resources.** Programs ask it to manage network communication, files, memory, and waiting through operating-system interfaces.
- **A socket is a communication endpoint managed by the kernel.** The program uses a file descriptor, a small integer handle, to refer to it.
- **Blocking does not mean repeatedly checking in a loop.** The kernel can put a thread to sleep until data, a connection, a timeout, or a signal wakes it.
- **Pointers connect APIs to application memory.** A function may read input from a supplied address or write a result there. The API contract tells us which; some APIs also require a byte count.

## Networking and Protocols

- **An IP address and port identify a network endpoint.** The server listens at a known endpoint; the client connects to it. Loopback allows the same arrangement entirely on one machine.
- **TCP makes packets look like an ordered byte stream.** Sequence numbers, acknowledgments, and retransmission handle loss and reordering; bytes missing from a failed connection cannot be recovered by a guarantee alone.
- **TCP does not know where commands end.** Sending PING in one write or PI followed by NG produces the same stream. Our protocol adds a newline to mark each complete request or response.
- **`pending_data` bridges bytes and messages.** It accumulates fragments, extracts complete lines, and preserves the unfinished remainder. One read can contain part of a message or several messages.
- **Sending is also incremental.** A send call can accept only part of the data. The sender must track progress and continue with the remaining bytes.
- **Successful sending is not proof of execution.** Bytes accepted by the local kernel may not yet have been read or processed by the other program. An application response communicates the result.
- **Binary formats need shared rules.** Multibyte numbers can have different byte orders in memory. Network address fields use an agreed ordering so machines interpret them consistently.
- **The protocol is the API between processes.** Our client sends requests and displays responses; the server executes commands and owns persistence. Higher-level frameworks usually hide much of this transport work.

## Timeouts, Signals, and Limits

- **Socket settings configure future behavior.** Installing a receive timeout tells the kernel how long a later receive operation may wait; installing the setting does not itself start a receive operation.
- **The kernel manages the wait.** The application supplies a duration rather than incrementing a seconds counter. Our current timeout limits inactivity between reads, not total time to complete a request.
- **Signals are notifications to a process.** Ctrl-C normally generates SIGINT for the foreground process group. Registering a handler changes the response to that signal; it does not enable Ctrl-C detection.
- **Graceful shutdown needs ordinary cleanup code.** A signal handler requests shutdown; the normal program flow releases resources. Signal interruption and network timeout are different reasons for a blocked call to return.
- **Limits are part of correctness.** Without a request-size cap, incomplete input can consume growing memory. Without time limits, a stalled client can occupy service capacity indefinitely.

## Concurrency and Shared State

- **Concurrency is about overlapping progress; parallelism is simultaneous execution.** A server benefits from concurrency even on one CPU because one worker can run while another waits for network data.
- **Threads share process memory but have separate execution state.** Every worker can access the same store and connection queue, while local variables such as `pending_data` belong to one client-handling call.
- **A mutex protects an invariant, not merely a variable.** The store lock keeps the map update and persistence-log append in one ordered operation so memory and restart recovery agree.
- **Lock scope determines both correctness and contention.** Queue operations happen under the queue lock, but receiving, sending, and handling a client happen after releasing it. One slow client therefore does not block connection scheduling.
- **A condition variable coordinates sleeping and waking.** Workers sleep while the queue is empty. The queue remembers pending work; a notification only prompts a worker to check the condition again.
- **A worker pool bounds concurrency and reuses threads.** A fixed number of workers repeatedly handle sockets from a bounded queue instead of creating an unlimited thread for every client.
- **Backpressure is a resource policy.** When active workers and the waiting queue are full, rejecting another connection protects memory and file-descriptor capacity.
- **Ownership makes cleanup understandable.** Main accepts and queues sockets; a worker owns a socket after removing it. During shutdown, main disables active communication, while the owning worker performs the final close.
- **Joining is a lifetime guarantee.** Main waits for workers before destroying the shared store, queue, mutex, or condition variable they reference.
- **Signals and worker coordination solve different problems.** The signal handler asks main to stop; the stop flag, socket shutdown, condition-variable notification, and joins coordinate the actual worker shutdown.
- **Thread safety does not create transactions.** Making each store operation atomic does not make a sequence such as GET followed by SET atomic.

## Still to Learn

Measuring contention and actual bottlenecks; asynchronous I/O; replication ordering; consistency across multiple processes and machines.
