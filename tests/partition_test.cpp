#include "partition.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main()
{
    struct TestCase
    {
        std::string key;
        std::uint64_t hash;
        std::size_t partition;
    };

    // Fixed outputs from a separate Python integer implementation of FNV-1a.
    // Full hashes catch errors that checking only hash % 2 would conceal.
    const TestCase cases[] = {
        {"", 0xcbf29ce484222325ULL, 1},
        {"a", 0xaf63dc4c8601ec8cULL, 0},
        {"b", 0xaf63df4c8601f1a5ULL, 1},
        {"foobar", 0x85944171f73967e8ULL, 0},
        {"hello", 0xa430d84680aabd0bULL, 1},
        {"color", 0x77f5c18e246c6638ULL, 0},
        {"full name", 0x6ea0936e2e80fe0fULL, 1},
        {"caf\xc3\xa9", 0x48e8823acfa40d89ULL, 1}, // UTF-8: cafe with acute e.
        {"caf\xc3\xa8", 0x48e8813acfa40bd6ULL, 0}, // UTF-8: cafe with grave e.
        {"quote\"and\\slash", 0x1afdc13397c10295ULL, 1},
        {std::string(4096, 'a'), 0x15a9fd7b219d7325ULL, 1},
        {std::string("prefix\0suffix", 13), 0x2fdda63535a6943eULL, 0},
    };

    int failures = 0;
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        const auto &test = cases[i];
        if (stable_key_hash(test.key) != test.hash)
        {
            std::cerr << "FAIL: full hash for test case " << i << '\n';
            ++failures;
        }
        if (partition_for_key(test.key) != test.partition)
        {
            std::cerr << "FAIL: partition for test case " << i << '\n';
            ++failures;
        }
    }

    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "All partition tests passed.\n";
    return EXIT_SUCCESS;
}
