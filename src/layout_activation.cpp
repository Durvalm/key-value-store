#include "layout_activation.h"
#include "destination_transfer.h"
#include "destination_verification.h"
#include "log_lock.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
    // Absolute log paths keep the meaning unchanged when the layout is relocated.
    std::string serialize_layout(const ClusterLayout &layout)
    {
        std::ostringstream output;
        output << "LAYOUT 1 " << layout.routing() << ' ' << layout.positions_per_group() << '\n';
        for (const auto &group : layout.groups())
            output << "GROUP " << std::quoted(group.id) << ' ' << std::quoted(group.history_id)
                   << ' ' << group.primary_port << ' ' << group.replica_port
                   << ' ' << std::quoted(group.primary_log) << ' ' << std::quoted(group.replica_log) << '\n';
        if (!output)
            throw std::runtime_error("Could not serialize active layout");
        return output.str();
    }

    bool same_file(const std::string &a, const std::string &b)
    {
        return std::filesystem::weakly_canonical(a) == std::filesystem::weakly_canonical(b) ||
            (std::filesystem::exists(a) && std::filesystem::exists(b) && std::filesystem::equivalent(a, b));
    }

    void publish(const std::string &contents, const std::string &active)
    {
        // Same directory means rename cannot cross filesystems. mkstemp creates
        // an exclusive file, never truncating an existing log or configuration.
        std::string temporary = active + ".tmp-XXXXXX";
        int descriptor = ::mkstemp(temporary.data());
        if (descriptor == -1)
            throw std::runtime_error("Could not create temporary active layout");
        try
        {
            std::size_t written = 0;
            while (written < contents.size())
            {
                const auto count = ::write(descriptor, contents.data() + written, contents.size() - written);
                if (count == -1 && errno == EINTR)
                    continue;
                if (count <= 0)
                    throw std::runtime_error("Could not write active layout");
                written += static_cast<std::size_t>(count);
            }
            if (::fsync(descriptor) == -1)
                throw std::runtime_error("Could not flush active layout");
            const int close_result = ::close(descriptor);
            descriptor = -1;
            if (close_result == -1)
                throw std::runtime_error("Could not close active layout");
            // The publication point: an existing active layout remains intact
            // until the fully written replacement is ready.
            if (::rename(temporary.c_str(), active.c_str()) == -1)
                throw std::runtime_error("Could not publish active layout");
        }
        catch (...)
        {
            if (descriptor != -1)
                ::close(descriptor);
            ::unlink(temporary.c_str());
            throw;
        }
    }
}

std::size_t activate_layout(const std::string &source_path,
                            const std::string &destination_path,
                            const std::string &active_path)
{
    if (active_path.empty())
        throw std::runtime_error("Active layout path must not be empty");
    const auto source = read_layout(source_path);
    const auto destination = read_layout(destination_path);
    validate_transfer_layouts(source, destination);
    // Do not follow output symlinks; publish only a separate ordinary file.
    for (const auto &path : {active_path, active_path + ".lock"})
        if (std::filesystem::is_symlink(path) ||
            (std::filesystem::exists(path) && !std::filesystem::is_regular_file(path)))
            throw std::runtime_error("Active layout and lock must be ordinary files");
    const std::string active = std::filesystem::weakly_canonical(std::filesystem::absolute(active_path)).string();
    std::vector<std::string> protected_paths{std::filesystem::absolute(source_path).string(),
                                             std::filesystem::absolute(destination_path).string()};
    for (const auto *layout : {&source, &destination})
        for (const auto &group : layout->groups())
            for (const auto &log : {group.primary_log, group.replica_log})
            {
                protected_paths.push_back(log);
                protected_paths.push_back(log + ".lock");
            }
    for (const auto &path : protected_paths)
        if (same_file(active, path) || same_file(active + ".lock", path))
            throw std::runtime_error("Active layout/lock must not overlap input layouts, logs or log locks");
    if (same_file(active, active + ".lock"))
        throw std::runtime_error("Active layout must be separate from its lock");

    LogLock publication_lock(active); // Serialize cooperating activators across rename.
    if (std::filesystem::exists(active) && serialize_layout(read_layout(active)) != serialize_layout(source))
        throw std::runtime_error("Existing active layout does not match the source layout");
    const auto contents = serialize_layout(destination);
    OfflineSources sources(source); // Keep originals locked until after publication.
    const auto snapshot = sources.snapshot();
    verify_destination(snapshot, destination, true);
    publish(contents, active);
    return snapshot.entries.size();
}
