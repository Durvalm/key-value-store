#pragma once

#include <cstdint>
#include <string>

// Validates the peer's history ID and progress. Caller owns the returned socket.
int connect_to_replica(std::uint16_t port, const std::string &history_id, std::uint64_t last_sequence);

// Uses an existing connection to send a mutation and check its acknowledgment.
void send_to_replica(int replica_socket, const std::string &line,
                     const std::string &expected_response);
