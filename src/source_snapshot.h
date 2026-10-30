#pragma once

#include "cluster_layout.h"

#include <cstddef>
#include <cstdint>
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

// All source nodes must be stopped. Holds every primary/replica log lock during
// collection, requires matching histories, and counts only primary live state.
SourceSnapshot collect_source_snapshot(const ClusterLayout &layout);

// Writes a new file only. Existing output files are never overwritten.
void write_source_snapshot(const SourceSnapshot &snapshot, const std::string &output_path);
