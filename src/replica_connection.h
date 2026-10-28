#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Verify retained records and catch up before returning. Caller owns the socket.
int connect_to_replica(std::uint16_t port, const std::string &history_id,
                       const std::vector<std::string> &history,
                       const std::string &ownership = "");

// Uses an existing connection to send a mutation and check its acknowledgment.
void send_to_replica(int replica_socket, const std::string &line,
                     const std::string &expected_response);
