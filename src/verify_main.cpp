#include "destination_transfer.h"
#include "destination_verification.h"

#include <csignal>
#include <exception>
#include <iostream>
#include <stdexcept>

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: kv_verify <source-layout> <destination-layout>\n"
                     "Sources must be stopped; keep destination traffic paused.\n";
        return 1;
    }
    try
    {
        if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            throw std::runtime_error("Could not configure socket error handling");
        const auto source = read_layout(argv[1]);
        const auto destination = read_layout(argv[2]);
        validate_transfer_layouts(source, destination);
        OfflineSources sources(source);
        const auto snapshot = sources.snapshot();
        const auto counts = verify_destination(snapshot, destination);
        std::cout << "Verified " << snapshot.entries.size() << " live keys on all destination primaries and replicas.\n";
        for (std::size_t i = 0; i < counts.size(); ++i)
            std::cout << destination.groups()[i].id << ": " << counts[i] << " keys verified on both copies\n";
        std::cout << "Verification is read-only; no layout was activated.\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << "Verification failed: " << error.what() << '\n';
        return 1;
    }
}
