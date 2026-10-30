#include "destination_transfer.h"
#include "command_parser.h"
#include "line_connection.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
    bool same_path(const std::string &a, const std::string &b)
    {
        if (std::filesystem::weakly_canonical(std::filesystem::absolute(a)) ==
            std::filesystem::weakly_canonical(std::filesystem::absolute(b)))
            return true;
        return std::filesystem::exists(a) && std::filesystem::exists(b) && std::filesystem::equivalent(a, b);
    }

    std::vector<std::string> log_paths(const ClusterLayout &layout)
    {
        std::vector<std::string> paths;
        for (const auto &group : layout.groups())
        {
            paths.push_back(group.primary_log);
            paths.push_back(group.replica_log);
        }
        return paths;
    }

    void require_fresh_logs(const ClusterLayout &layout)
    {
        for (const auto &group : layout.groups())
        {
            const std::string expected = layout.ownership_header(group.id) + "\nHISTORY " + group.history_id + '\n';
            for (const auto &path : {group.primary_log, group.replica_log})
            {
                if (!std::filesystem::is_regular_file(path))
                    throw std::runtime_error("Destination log is missing: start the new nodes with their layout first");
                std::ifstream file(path);
                std::string actual(expected.size() + 1, '\0');
                file.read(actual.data(), static_cast<std::streamsize>(actual.size()));
                actual.resize(static_cast<std::size_t>(file.gcount()));
                if (file.bad() || actual != expected)
                    throw std::runtime_error("Destination must have a fresh header-only history: " + path);
            }
        }
    }
}

void validate_transfer_layouts(const ClusterLayout &source, const ClusterLayout &destination)
{
    for (const auto &target : destination.groups())
        for (const auto &old : source.groups())
            if (target.history_id == old.history_id)
                throw std::runtime_error("Destination history IDs must be new and separate from source histories");
    const auto sources = log_paths(source);
    const auto targets = log_paths(destination);
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        for (const auto &old : sources)
            if (same_path(targets[i], old) || same_path(targets[i], old + ".lock") ||
                same_path(targets[i] + ".lock", old) || same_path(targets[i] + ".lock", old + ".lock"))
                throw std::runtime_error("Source/destination log and lock files must not overlap");
        for (std::size_t j = 0; j < i; ++j)
            if (same_path(targets[i], targets[j]) || same_path(targets[i], targets[j] + ".lock") ||
                same_path(targets[i] + ".lock", targets[j]) || same_path(targets[i] + ".lock", targets[j] + ".lock"))
                throw std::runtime_error("Destination nodes must use separate log and lock files");
    }
}

std::vector<std::size_t> transfer_snapshot(const SourceSnapshot &snapshot, const ClusterLayout &destination)
{
    std::vector<std::pair<std::size_t, std::string>> requests;
    for (const auto &[key, value] : snapshot.entries)
    {
        std::ostringstream line;
        line << "SET " << std::quoted(key) << ' ' << std::quoted(value);
        const std::string request = line.str();
        const auto parsed = parse_command(request);
        if (!line || !parsed.error_response.empty())
            throw std::runtime_error("Snapshot entry cannot fit the destination SET protocol");
        const auto &owner = destination.owner(key);
        const auto &groups = destination.groups();
        std::size_t index = 0;
        while (groups[index].id != owner.id)
            ++index;
        requests.push_back({index, request + '\n'});
    }
    require_fresh_logs(destination);
    std::vector<int> sockets(destination.groups().size(), -1);
    std::vector<std::size_t> counts(sockets.size(), 0);
    try
    {
        for (std::size_t i = 0; i < sockets.size(); ++i)
            sockets[i] = connect_loopback(destination.groups()[i].primary_port);
        // No mutation is sent until every target has confirmed its identity and
        // reserved an import session with sequence zero.
        for (std::size_t i = 0; i < sockets.size(); ++i)
        {
            const auto &group = destination.groups()[i];
            std::ostringstream request;
            request << "IMPORT " << group.history_id << ' '
                    << std::quoted(destination.ownership_header(group.id)) << '\n';
            const auto response = exchange_message(sockets[i], request.str());
            if (response != "READY_IMPORT\n")
                throw std::runtime_error("Destination " + group.id + " refused import: " + response);
        }
        for (const auto &[index, request] : requests)
        {
            const auto response = exchange_message(sockets[index], request);
            if (response != "OK\n")
                throw std::runtime_error("Destination write was not acknowledged: " + response);
            ++counts[index];
        }
    }
    catch (...)
    {
        for (const int socket : sockets)
            if (socket != -1)
                ::close(socket);
        throw;
    }
    for (const int socket : sockets)
        ::close(socket);
    return counts;
}
