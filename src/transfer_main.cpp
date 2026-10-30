#include "destination_transfer.h"

#include <csignal>
#include <exception>
#include <iostream>
#include <stdexcept>

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: kv_transfer <source-layout> <destination-layout>\n"
                     "Sources must be stopped; destinations must be fresh and running.\n";
        return 1;
    }
    try
    {
        if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            throw std::runtime_error("Could not configure socket error handling");
        const auto source = read_layout(argv[1]);
        const auto destination = read_layout(argv[2]);
        validate_transfer_layouts(source, destination);
        OfflineSources sources(source); // Hold source locks until transfer finishes/fails.
        const auto snapshot = sources.snapshot();
        const auto counts = transfer_snapshot(snapshot, destination);
        std::cout << "Transferred " << snapshot.entries.size() << " live keys with replica acknowledgments.\n";
        for (std::size_t i = 0; i < counts.size(); ++i)
            std::cout << destination.groups()[i].id << ": " << counts[i] << " acknowledged SETs\n";
        std::cout << "Source logs unchanged. Destination data still needs exact verification; no layout was activated.\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << "Transfer failed: " << error.what() << '\n';
        return 1;
    }
}
