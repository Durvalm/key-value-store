#pragma once

#include <cstdint>
#include <string>

// A connected loopback socket with finite connect and send/receive waits.
// The caller owns the returned descriptor and must close it.
int connect_loopback(std::uint16_t port);

// One outstanding request, one short newline-terminated control/ACK response.
// Caller serializes complete exchanges. Request includes its framing newline.
std::string exchange_message(int socket, const std::string &request);
