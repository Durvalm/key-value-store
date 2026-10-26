#include "partition.h"

std::uint64_t stable_key_hash(const std::string &key)
{
    constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset_basis;

    for (unsigned char byte : key)
    {
        hash ^= byte;
        hash *= prime; // Unsigned arithmetic wraps modulo 2^64.
    }
    return hash;
}

std::size_t partition_for_key(const std::string &key)
{
    return static_cast<std::size_t>(stable_key_hash(key) % 2);
}
