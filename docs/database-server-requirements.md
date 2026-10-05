# Part 3: Database Server Requirements

## Purpose

Turn the key-value store into a long-running service that other processes can use over TCP.

This document defines externally observable behavior, failure policies, and acceptance tests. It intentionally does not prescribe classes, function signatures, socket wrappers, or the structure of the server loop. Designing those parts is part of the learning exercise.

## Learning Objectives

By completing this phase, be able to explain and demonstrate:

- Why a database server separates storage from the programs that use it.
- The roles of a server address, port, listening socket, and connected socket.
- The lifecycle from accepting a connection through closing it.
- Why TCP is a byte stream rather than a sequence of messages.
- How a protocol lets independent processes agree on request and response meanings.
- How message framing survives requests split across reads or combined into one read.
- Why reading and writing may transfer fewer bytes than requested.
- How normal disconnection differs from a network or protocol error.
- Where command parsing belongs relative to networking and storage.
- Which resources the process owns and when they must be released.

## Existing Behavior to Preserve

The existing `KeyValueStore` behavior and persistence guarantees must remain unchanged:

- `SET <key> <value>` inserts or overwrites a value.
- `GET <key>` returns the current value or a missing-key result.
- `DELETE <key>` reports whether a key was removed.
- `EXISTS <key>` reports whether a key exists.
- `SIZE` reports the number of live keys.
- Successful mutations remain recoverable after a normal restart.
- Manual compaction continues to preserve the current state.
- Existing unit and persistence tests continue to pass.

The interactive CLI may remain available. The server must use the same storage semantics rather than creating a separate implementation of the database.

## Phase 3A: Protocol Before Networking

### N1. Command Boundary

Separate command execution from terminal input and output.

Given one complete request, the command layer must:

- Parse the request.
- Validate its command name and arguments.
- Execute the corresponding store operation.
- Produce one complete response.
- Produce an error response for invalid input without terminating the process.

The same command behavior should be reusable by both the CLI and the future TCP server. The representation of the result and the exact code organization are design decisions.

### N2. Protocol Specification

Before opening a socket, document the application protocol in `docs/learning-log.md`.

The specification must define:

- How a receiver knows where one request ends.
- Whether command names are case-sensitive.
- How keys and values containing spaces are represented.
- Whether empty keys or values are valid.
- The response for every supported command.
- The response for missing keys.
- The response format for malformed requests and unknown commands.
- The maximum accepted request size.
- Whether a connection supports one request or many requests.
- Whether administrative commands such as `COMPACT` are remotely available.

A newline-terminated text protocol is recommended for the first version because it is human-readable and can be exercised with `nc`. The exact grammar is still a design decision.

Every request must receive exactly one response. Every response must use the protocol's framing rule so a client can distinguish adjacent responses.

## Phase 3B: Sequential TCP Server

### N3. Configurable Endpoint

The server must accept a configurable port and persistence-file path.

For local development, it should listen only on the loopback interface by default. Exposing the server to other machines must require an intentional configuration choice because this phase does not include authentication or encryption.

Startup failures, including an unavailable port or inaccessible persistence file, must be visible and must end the process with a failure status.

### N4. Basic Connection Lifecycle

The first server version must serve one connected client at a time. Concurrency belongs to Part 4.

The server must:

- Start and wait for a client connection.
- Receive bytes until one complete request is available.
- Execute that request and return its complete response.
- Support multiple requests on the same connection if the protocol promises this.
- Treat an orderly client disconnect as normal.
- Release the connected client's resources after disconnection.
- Continue accepting later clients without losing database state.

A client that disconnects must not stop the server.

### N5. TCP Stream Handling

Correctness must not depend on one read returning exactly one request or one write sending the entire response.

The server must correctly handle:

- A request arriving across multiple reads.
- Multiple complete requests arriving in one read.
- A final complete request followed immediately by disconnection.
- A connection closing with an incomplete request buffered.
- A write that transfers only part of a response.

Incomplete input must not execute a partial command. The policy for an incomplete request at disconnect must be documented.

### N6. Protocol Errors

Malformed input must not crash the server or corrupt the store.

The protocol must define behavior for:

- Empty requests.
- Unknown commands.
- Missing arguments.
- Extra arguments.
- Requests exceeding the documented size limit.
- Invalid text according to the chosen grammar.

For each case, decide whether the server sends an error and keeps the connection open or sends an error and closes it. Apply the policy consistently.

## Phase 3C: Client and Server Lifecycle

### N7. Minimal Client

Provide a client program that can:

- Connect to a configured server address and port.
- Send a complete request using the documented framing.
- Receive and print the complete response.
- Report connection and protocol failures clearly.
- Close its connection cleanly.

The client does not need to contain storage logic. A client-side library or advanced interactive interface is not required.

### N8. Idle Connections

An idle or stalled client must not hold the sequential server forever.

Define and implement a finite timeout policy for receiving a complete request. A timeout must close or otherwise release that connection without terminating the server or changing database state.

### N9. Graceful Shutdown

The server must have a documented way to stop intentionally.

On graceful shutdown, it must:

- Stop accepting new connections.
- Release active network resources.
- Leave the persistence log recoverable.
- Exit without reporting an in-progress partial request as successful.

The exact signal or command used to initiate shutdown is a design decision. Abrupt crash and power-loss guarantees remain outside this phase.

## Required Tests

Keep storage tests separate from protocol and network tests. Network tests must use an isolated persistence path and a port selected for the test rather than depending on a personal database file or a permanently fixed port.

### Command and Protocol

- Each supported command produces its documented response.
- A missing key produces the documented missing-key response.
- Invalid commands and arguments produce errors without terminating command handling.
- Values containing spaces survive `SET` followed by `GET`.
- One input containing multiple framed requests produces the same number of ordered responses.
- A request split at several different byte positions is not executed until complete.

### Connections

- A client can connect, issue a request, and receive a response.
- One client can issue multiple requests on one connection if promised by the protocol.
- A disconnected client does not stop the server from serving the next client.
- State written by one connection is visible to a later connection.
- An incomplete request followed by disconnect does not mutate the store.
- An oversized request follows the documented error policy.
- An idle client is eventually released according to the timeout policy.

### Persistence and Lifecycle

- Data written through TCP survives a normal server restart.
- A malformed request does not append a persistence record.
- Graceful shutdown leaves a log that can be replayed.
- Starting on an unavailable port fails visibly.

## Manual Acceptance Session

At minimum, demonstrate the server from a separate terminal with a general-purpose TCP client such as `nc`:

1. Connect to the configured loopback address and port.
2. Set a key and receive the documented success response.
3. Read the same key and receive its value.
4. Send an invalid request and receive an error without crashing the server.
5. Disconnect and reconnect.
6. Read the key again from the new connection.
7. Restart the server and confirm the key still exists.

Record the exact session in `docs/learning-log.md` once the protocol has been chosen.

## Non-Goals

Do not add these during the initial database-server phase:

- Concurrent client handling, worker threads, or a thread pool.
- Asynchronous event loops.
- Replication or communication between database nodes.
- Sharding, leader election, or consensus.
- TLS, authentication, or internet exposure.
- HTTP, REST, gRPC, or compatibility with the Redis protocol.
- Request pipelining beyond correctly handling multiple framed requests already received.
- Transactions across multiple commands or keys.
- Automatic retries by the server.
- Production deployment or daemon management.

## Implementation Checkpoints

Complete these checkpoints in order. Each checkpoint states the result to achieve, not the code structure to use.

### Checkpoint 1: Protocol Sketch

- Write example request and response transcripts.
- Define framing, grammar, limits, and error behavior.
- Explain how a receiver distinguishes two adjacent requests.
- List at least three ways network input can differ from terminal input.

### Checkpoint 2: Command Execution

- Pass complete request strings into command handling without using `std::cin`.
- Verify responses without opening a socket.
- Keep database behavior in `KeyValueStore` rather than duplicating it.

### Checkpoint 3: Connection Experiment

- Build the smallest server that accepts one connection.
- Prove a hard-coded request and response can cross the connection.
- Observe what happens when the client disconnects.

This experiment may be throwaway code. Its purpose is to understand the socket lifecycle before combining networking with the store.

### Checkpoint 4: Framed Sequential Server

- Connect the documented protocol to the store.
- Buffer bytes until complete requests are available.
- Handle fragmented and combined requests correctly.
- Continue serving later connections after a client disconnects.

### Checkpoint 5: Client and Lifecycle

- Add the minimal client.
- Add request-size and idle-time limits.
- Add graceful shutdown behavior.
- Prove persistence across a server restart.

## Learning Rules

Before each checkpoint:

1. Draw or write the data flow from client bytes to database response.
2. State which component owns each resource and when it is released.
3. Predict at least two failure cases.
4. Write or outline the acceptance tests.
5. Implement the smallest experiment that answers the current question.

When asking for help, bring the attempted design, observed socket behavior, compiler error, failed test, or protocol transcript. Reviews should identify incorrect assumptions and explain tradeoffs without replacing the complete implementation.

Useful questions to keep asking:

- What does this piece of code know, and what should it not know?
- Am I processing bytes, a complete request, or a database command at this point?
- What happens if the peer disconnects here?
- Could this operation complete only partially?
- Who owns this resource now?
- Can malformed external input reach the storage layer?

## Definition of Done

The database-server phase is complete when:

- The wire protocol and its limits are documented with examples.
- A separate process can use the store over TCP.
- TCP stream fragmentation and multiple requests per read are handled correctly.
- Malformed, incomplete, and oversized requests follow documented policies.
- Client disconnects and timeouts do not terminate the server.
- A minimal client can send requests and receive responses.
- The server can shut down gracefully and restart with its persisted state.
- Existing storage tests and all required protocol and network tests pass.
- The complete path from received bytes to persisted mutation and response can be explained.
