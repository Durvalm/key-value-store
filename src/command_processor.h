#pragma once

#include "key_value_store.h"

#include <cstddef>
#include <string>

inline constexpr std::size_t max_request_size = 4096;

struct CommandResult
{
    std::string response;
    bool close_requested;
    bool mutation_applied = false;
};

CommandResult process_command(
    const std::string &line,
    KeyValueStore &store,
    bool read_only = false,
    bool allow_compaction = true);

CommandResult request_too_large_result();
