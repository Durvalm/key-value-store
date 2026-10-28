#pragma once

#include "hash_ring.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct GroupDefinition
{
    std::string id;
    std::string history_id;
    std::uint16_t primary_port = 0;
    std::uint16_t replica_port = 0;
    std::string primary_log;
    std::string replica_log;
};

// One validated, immutable description of ownership and local group endpoints.
// Loading a layout does not start servers, open logs, or migrate data.
class ClusterLayout
{
public:
    ClusterLayout(const std::string &routing, std::size_t positions_per_group,
                  std::vector<GroupDefinition> groups);

    const std::string &routing() const { return routing_; }
    std::size_t positions_per_group() const { return positions_per_group_; }
    const std::vector<GroupDefinition> &groups() const { return groups_; }
    // Canonical ownership settings only; endpoints/log locations may change.
    std::string ownership_definition() const;
    const GroupDefinition &owner(const std::string &key) const;
    const GroupDefinition &group(const std::string &id) const;

private:
    std::string routing_;
    std::size_t positions_per_group_;
    std::vector<GroupDefinition> groups_;
    std::vector<RingEntry> ring_;
};

// Relative log paths resolve against the directory containing this file.
ClusterLayout read_layout(const std::string &file_path);
