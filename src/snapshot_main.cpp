#include "source_snapshot.h"

#include <exception>
#include <iostream>

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: kv_snapshot <source-layout> <new-output-file>\n"
                     "Stop all source nodes after their primary/replica histories are synchronized.\n";
        return 1;
    }
    try
    {
        const auto layout = read_layout(argv[1]);
        const auto snapshot = collect_source_snapshot(layout);
        write_source_snapshot(snapshot, argv[2]);
        std::cout << "Captured " << snapshot.entries.size() << " live keys from "
                  << snapshot.sources.size() << " primary groups.\n";
        for (const auto &source : snapshot.sources)
            std::cout << source.group_id << ": sequence " << source.last_sequence
                      << ", " << source.live_keys << " live keys\n";
        std::cout << "Snapshot written to " << argv[2] << "; source logs unchanged.\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << "Snapshot failed: " << error.what() << '\n';
        return 1;
    }
}
