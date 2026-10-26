#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// FNV-1a 64-bit over the key's raw bytes; independent of process or locale.
std::uint64_t stable_key_hash(const std::string &key);

// Fixed routing rule v1: two partitions, numbered 0 and 1.
std::size_t partition_for_key(const std::string &key);
