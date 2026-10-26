#pragma once

#include <cstddef>
#include <string>

inline constexpr std::size_t max_request_size = 4096;

struct ParsedCommand
{
    std::string name;
    std::string key;   // Empty for valid commands without a key.
    std::string value; // Used by SET.
    std::string error_response; // Empty on success; otherwise a framed protocol error.
};

// Validate an entire command without executing it or opening any connections.
ParsedCommand parse_command(const std::string &line);
