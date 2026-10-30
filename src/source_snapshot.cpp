#include "source_snapshot.h"
#include "key_value_store.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

SourceSnapshot collect_source_snapshot(const ClusterLayout &layout)
{
    // Refuse missing files and aliases before opening a store. In particular,
    // a primary and replica must not just be two hard links to the same log.
    std::vector<std::string> paths;
    for (const auto &group : layout.groups())
        for (const auto &path : {group.primary_log, group.replica_log})
        {
            if (!std::filesystem::is_regular_file(path))
                throw std::runtime_error("Source log is missing or not a regular file: " + path);
            for (const auto &previous : paths)
                if (std::filesystem::equivalent(previous, path))
                    throw std::runtime_error("Source nodes must use separate physical log files");
            paths.push_back(path);
        }

    // Keep the stores alive together: none of these logs can be reopened by a
    // cooperating server until every group has been checked and copied.
    std::vector<std::unique_ptr<KeyValueStore>> primaries, replicas;
    for (const auto &group : layout.groups())
    {
        primaries.push_back(std::make_unique<KeyValueStore>(group.primary_log, layout, group.id, true));
        replicas.push_back(std::make_unique<KeyValueStore>(group.replica_log, layout, group.id, true));
    }

    SourceSnapshot snapshot;
    snapshot.ownership_definition = layout.ownership_definition();
    for (std::size_t i = 0; i < layout.groups().size(); ++i)
    {
        const auto &group = layout.groups()[i];
        const auto &primary = *primaries[i];
        const auto &replica = *replicas[i];
        if (primary.history_records() != replica.history_records())
            throw std::runtime_error("Source group " + group.id +
                " is not synchronized; run primary/replica catch-up, then stop both nodes");
        auto entries = primary.snapshot();
        snapshot.sources.push_back({group.id, primary.history_id(), primary.last_sequence(), entries.size()});
        snapshot.entries.insert(snapshot.entries.end(), entries.begin(), entries.end());
    }
    std::sort(snapshot.entries.begin(), snapshot.entries.end());
    for (std::size_t i = 1; i < snapshot.entries.size(); ++i)
        if (snapshot.entries[i-1].first == snapshot.entries[i].first)
            throw std::runtime_error("Duplicate key across source groups");
    return snapshot;
}

void write_source_snapshot(const SourceSnapshot &snapshot, const std::string &output_path)
{
    // Serialize the verified copy before creating any output file.
    std::ostringstream output;
    output << "SNAPSHOT 1 " << snapshot.ownership_definition << '\n';
    for (const auto &source : snapshot.sources)
        output << "SOURCE " << std::quoted(source.group_id) << ' ' << std::quoted(source.history_id)
               << ' ' << source.last_sequence << ' ' << source.live_keys << '\n';
    output << "DATA\n";
    for (const auto &[key, value] : snapshot.entries)
        output << "SET " << std::quoted(key) << ' ' << std::quoted(value) << '\n';
    output << "END " << snapshot.entries.size() << '\n';
    if (!output)
        throw std::runtime_error("Could not serialize snapshot");
    const std::string contents = output.str();

    // O_EXCL prevents overwriting a source log, layout, or previous snapshot,
    // including when the output path is a symlink or hard link to an existing file.
    const int descriptor = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor == -1)
        throw std::runtime_error("Could not create new snapshot output: " + std::string(std::strerror(errno)));
    try
    {
        std::size_t written = 0;
        while (written < contents.size())
        {
            const ssize_t count = ::write(descriptor, contents.data() + written, contents.size() - written);
            if (count == -1 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::runtime_error("Could not finish writing snapshot");
            written += static_cast<std::size_t>(count);
        }
    }
    catch (...)
    {
        ::close(descriptor);
        ::unlink(output_path.c_str());
        throw;
    }
    if (::close(descriptor) == -1)
    {
        ::unlink(output_path.c_str());
        throw std::runtime_error("Could not close snapshot output");
    }
}
