#pragma once

#include "key_value_store.h"

#include <cstddef>
#include <string>

inline constexpr std::size_t max_request_size = 4096;

struct CommandResult
{
    std::string response;
    bool close_requested;
};

CommandResult process_command(
    const std::string &line,
    KeyValueStore &store);

CommandResult request_too_large_result();
