#include "destination_verification.h"
#include "command_parser.h"
#include "line_connection.h"

#include <unistd.h>

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    struct VerificationNode
    {
        std::size_t group;
        bool replica;
        int socket = -1;
        std::uint64_t sequence = 0;
    };

    std::string label(const ClusterLayout &layout, const VerificationNode &node)
    {
        return layout.groups()[node.group].id + (node.replica ? "/replica" : "/primary");
    }

    std::uint64_t check_identity(const ClusterLayout &layout, const VerificationNode &node)
    {
        const auto &group = layout.groups()[node.group];
        std::ostringstream request;
        request << "VERIFY " << group.history_id << ' ' << std::quoted(layout.ownership_header(group.id))
                << ' ' << std::quoted(layout.node_description(group.id, node.replica ? "replica" : "primary")) << '\n';
        if (request.str().size() > 32768)
            throw std::runtime_error("Verification identity exceeds protocol limit");
        const auto response = exchange_message(node.socket, request.str());
        std::istringstream reply(response);
        std::string marker, text, extra;
        if (!(reply >> marker >> text) || (reply >> extra) || marker != "VERIFY_READY")
            throw std::runtime_error("Identity check failed for " + label(layout, node) + ": " + response);
        std::uint64_t sequence = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), sequence);
        if (error != std::errc{} || end != text.data() + text.size())
            throw std::runtime_error("Invalid verification progress from " + label(layout, node));
        return sequence;
    }
}

std::vector<std::size_t> verify_destination(const SourceSnapshot &snapshot, const ClusterLayout &destination)
{
    const auto &groups = destination.groups();
    // Each bucket contains GET request / expected VALUE response pairs.
    std::vector<std::vector<std::pair<std::string, std::string>>> requests(groups.size());
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i)
    {
        const auto &[key, value] = snapshot.entries[i];
        if (i > 0 && snapshot.entries[i-1].first >= key)
            throw std::runtime_error("Source snapshot keys must be unique and sorted");
        std::ostringstream get, expected;
        get << "GET " << std::quoted(key);
        expected << "VALUE " << std::quoted(value) << '\n';
        if (!get || !expected || !parse_command(get.str()).error_response.empty() ||
            expected.str().size() > max_request_size + 32)
            throw std::runtime_error("Source entry exceeds verification protocol limits");
        const auto &owner = destination.owner(key);
        std::size_t index = 0;
        while (groups[index].id != owner.id)
            ++index;
        requests[index].push_back({get.str() + '\n', expected.str()});
    }
    std::vector<std::size_t> counts;
    std::vector<VerificationNode> nodes;
    for (std::size_t group = 0; group < groups.size(); ++group)
    {
        counts.push_back(requests[group].size());
        nodes.push_back({group, false});
        nodes.push_back({group, true});
    }
    try
    {
        for (auto &node : nodes)
        {
            const auto &group = groups[node.group];
            node.socket = connect_loopback(node.replica ? group.replica_port : group.primary_port);
        }
        // Capture all starting counters before reading any data. Finish counters
        // are captured only after all data checks, so the checked windows overlap.
        for (auto &node : nodes)
            node.sequence = check_identity(destination, node);
        for (std::size_t i = 0; i < nodes.size(); i += 2)
            if (nodes[i].sequence != nodes[i+1].sequence)
                throw std::runtime_error("Primary/replica progress differs for " + groups[nodes[i].group].id);
        for (const auto &node : nodes)
        {
            for (const auto &[request, expected] : requests[node.group])
                if (exchange_message(node.socket, request, max_request_size + 32) != expected)
                    throw std::runtime_error("Value mismatch on " + label(destination, node));
            const auto expected_size = "INTEGER " + std::to_string(counts[node.group]) + '\n';
            if (exchange_message(node.socket, "SIZE\n") != expected_size)
                throw std::runtime_error("Key count mismatch on " + label(destination, node));
        }
        for (const auto &node : nodes)
            if (check_identity(destination, node) != node.sequence)
                throw std::runtime_error("Destination changed during verification: " + label(destination, node));
    }
    catch (...)
    {
        for (const auto &node : nodes)
            if (node.socket != -1)
                ::close(node.socket);
        throw;
    }
    for (const auto &node : nodes)
        ::close(node.socket);
    return counts;
}
