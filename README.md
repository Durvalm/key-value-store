# Key-Value-Store

Like Redis and AWS DynamoDB, I'm building a Key-Value-Store Database.
Commonly used for caching and session management (browsers and stuff), it's an efficient way for high-speed, low-latency lookups.
I intent to learn more about distributed systems, TCP networking, threads, concurrency, etc.
I will be coding this in C++ as I also intend to learn it more and go deeper into real thread-level concurrency.
The project may eventually serve as the storage backend behind my "personal OS", which is an LLM-based project I actively use to keep context and help me organize my life and take actions.

## Planned Progression

### 1. In-Memory Store

Implement a thread-unsafe local store with:

- `SET key value`
- `GET key`
- `DELETE key`
- A command-line interface
- Clear operation semantics
- Unit tests

### 2. Persistence

Preserve data across process restarts using:

- An append-only log
- Record serialization
- Log replay
- Detection of incomplete or corrupted records
- Explicit durability guarantees
- Log compaction

### 3. Database Server

Expose the store over a network through:

- A TCP server and client
- A documented request/response protocol
- Connection management
- Request parsing
- Timeouts and graceful shutdown

### 4. Concurrency

Support multiple clients while exploring:

- Threads and thread pools
- Locks and synchronization
- Race conditions
- Contention
- Thread safety
- Backpressure

### 5. Performance

Measure and improve the system using:

- Repeatable benchmarks
- Throughput measurements
- Latency distributions
- CPU and memory profiling
- Disk and network profiling
- Caching and batching experiments

### 6. Replication

Introduce multiple database nodes with:

- Primary-replica replication
- An ordered replication log
- Replica recovery
- Replication lag
- Configurable acknowledgment behavior
- Manual failover

### 7. Distribution

Explore selected distributed-system mechanisms such as:

- Partitioning and consistent hashing
- Node membership and rebalancing
- Leader election
- Quorum reads and writes
- Consistency models
- Network partitions
- Idempotent retries

These features will be introduced incrementally rather than combined into one large implementation.
