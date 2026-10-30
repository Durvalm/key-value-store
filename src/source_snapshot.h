#pragma once

#include "cluster_layout.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct SourceProgress
{
    std::string group_id;
    std::string history_id;
    std::uint64_t last_sequence;
    std::size_t live_keys;
};

struct SourceSnapshot
{
    std::string ownership_definition;
    std::vector<SourceProgress> sources;
    std::vector<std::pair<std::string, std::string>> entries;
};

class KeyValueStore;

// Own the read-only stores/locks for the whole offline operation, not just capture.
class OfflineSources
{
public:
    explicit OfflineSources(const ClusterLayout &layout);
    OfflineSources(const OfflineSources &) = delete;
    OfflineSources &operator=(const OfflineSources &) = delete;
    ~OfflineSources();
    SourceSnapshot snapshot() const;

private:
    ClusterLayout layout_;
    std::vector<std::unique_ptr<KeyValueStore>> primaries_;
    std::vector<std::unique_ptr<KeyValueStore>> replicas_;
};

// All source nodes must be stopped. Holds every primary/replica log lock during
// collection, requires matching histories, and counts only primary live state.
SourceSnapshot collect_source_snapshot(const ClusterLayout &layout);

// Writes a new file only. Existing output files are never overwritten.
void write_source_snapshot(const SourceSnapshot &snapshot, const std::string &output_path);
