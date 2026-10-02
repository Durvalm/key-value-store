#pragma once

#include "key_value_store.h"

#include <string>

struct CommandResult
{
    std::string response;
    bool close_requested;
};

CommandResult process_command(
    const std::string &line,
    KeyValueStore &store);