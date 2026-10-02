# Learning Log

## Database Server: Protocol V1

### Purpose

Protocol V1 defines how a client represents database commands and how the server represents results. It is independent of terminal input and TCP socket handling: `process_command()` accepts one complete request without its terminating newline and returns one complete newline-terminated response.

### Framing

- Protocol V1 is a UTF-8-compatible text protocol.
- Every request ends with one line-feed byte (`\n`).
- Every response ends with one line-feed byte (`\n`).
- The networking layer removes the terminating newline before calling `process_command()`.
- A request may not contain an embedded carriage return or newline.
- A request may contain at most 4096 bytes, excluding its terminating newline.
- Command names are uppercase and case-sensitive.
- One connection may carry multiple requests and responses in order.

TCP is a byte stream, so one socket read is not assumed to equal one request. The future networking layer must buffer bytes until it finds a newline, retain incomplete data for the next read, and process multiple complete lines when one read contains multiple requests.

### String Encoding

Keys and values use quoted C++ string syntax as implemented by `std::quoted`:

```text
"ordinary key"
"a value with spaces"
"Durval \"D\" Almeida"
"a backslash: \\"
```

Keys and values must be non-empty. Literal carriage returns and newlines are not supported.

### Requests

```text
SET "<key>" "<value>"
GET "<key>"
DELETE "<key>"
EXISTS "<key>"
SIZE
COMPACT
HELP
EXIT
```

Commands reject missing arguments, malformed quoted strings, and extra arguments. `COMPACT` is available in Protocol V1 for local experimentation; the server will initially bind only to the loopback interface and will not claim production security.

### Responses

```text
OK
VALUE "<value>"
NOT_FOUND
INTEGER <number>
COMMANDS "<space-separated command names>"
ERROR <code> "<message>"
BYE
```

Response meanings:

- `OK`: a mutation or administrative command completed successfully.
- `VALUE`: a `GET` found the key; the value uses quoted string encoding.
- `NOT_FOUND`: a `GET` did not find the key.
- `INTEGER`: the result of `DELETE`, `EXISTS`, or `SIZE`.
- `COMMANDS`: the response to `HELP`.
- `ERROR`: the request failed. The code is stable for programs; the quoted message is explanatory text.
- `BYE`: `EXIT` succeeded and the caller should close this client session.

Protocol V1 error codes are:

```text
EMPTY_REQUEST
INVALID_REQUEST
INVALID_ARGUMENT
UNKNOWN_COMMAND
REQUEST_TOO_LARGE
INTERNAL
```

### Example Session

The labels below are explanatory and are not transmitted:

```text
Client: SET "full name" "Durval Almeida"
Server: OK

Client: GET "full name"
Server: VALUE "Durval Almeida"

Client: EXISTS "full name"
Server: INTEGER 1

Client: GET "missing"
Server: NOT_FOUND

Client: GET "missing" extra
Server: ERROR INVALID_ARGUMENT "Usage: GET \"<key>\""

Client: EXIT
Server: BYE
```

### Current Guarantee

`process_command()` performs protocol parsing independently of input/output transport. The CLI prints its returned response; the future server will send the same response bytes to a connected client. `close_requested` is internal control information and is never transmitted.
