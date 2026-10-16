#pragma once

#include <cstdint>
#include <string>

// Connects and establishes a fresh replica session. Caller owns the returned socket.
int connect_to_replica(std::uint16_t port);

// Uses an existing connection to send a mutation and check its acknowledgment.
void send_to_replica(int replica_socket, const std::string &line,
                     const std::string &expected_response);
