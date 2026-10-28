#include "cluster_layout.h"
#include "partition.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
    bool valid_id(const std::string &id)
    {
        return !id.empty() && id.size() <= 128 &&
            id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos;
    }

    bool reached_end(std::istringstream &input)
    {
        input >> std::ws;
        return input.eof();
    }

    bool read_quoted(std::istringstream &input, std::string &value)
    {
        input >> std::ws;
        return input.peek() == '"' && static_cast<bool>(input >> std::quoted(value));
    }

    std::size_t read_number(std::istringstream &input, std::size_t maximum)
    {
        std::string text;
        input >> text;
        std::size_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > maximum)
            throw std::runtime_error("Invalid layout number: " + text);
        return value;
    }
}

ClusterLayout::ClusterLayout(const std::string &routing, std::size_t positions_per_group,
                             std::vector<GroupDefinition> groups)
    : routing_(routing), positions_per_group_(positions_per_group), groups_(std::move(groups))
{
    if (routing_ != "fnv1a64-mod-v1" && routing_ != "fnv1a64-fmix64-ring-v1")
        throw std::runtime_error("Unknown layout routing rule");
    if (positions_per_group_ == 0 || positions_per_group_ > 1024 ||
        (routing_ == "fnv1a64-mod-v1" && positions_per_group_ != 1))
        throw std::runtime_error("Use 1 position for modulo or 1-1024 for ring routing");
    if (groups_.empty() || groups_.size() > 64)
        throw std::runtime_error("Layout requires 1-64 groups");

    std::vector<std::string> ids, history_ids, paths;
    std::vector<std::uint16_t> ports;
    for (GroupDefinition &group : groups_)
    {
        if (!valid_id(group.id) || !valid_id(group.history_id))
            throw std::runtime_error("Group/history IDs require 1-128 letters, digits, dots, underscores or hyphens");
        if (std::find(ids.begin(), ids.end(), group.id) != ids.end() ||
            std::find(history_ids.begin(), history_ids.end(), group.history_id) != history_ids.end())
            throw std::runtime_error("Duplicate group or history ID in layout");
        ids.push_back(group.id);
        history_ids.push_back(group.history_id);
        for (const auto port : {group.primary_port, group.replica_port})
        {
            if (port == 0 || std::find(ports.begin(), ports.end(), port) != ports.end())
                throw std::runtime_error("Each local endpoint needs a distinct nonzero port");
            ports.push_back(port);
        }
        for (std::string *path : {&group.primary_log, &group.replica_log})
        {
            if (path->empty() || path->find_first_of("\r\n") != std::string::npos || path->find('\0') != std::string::npos)
                throw std::runtime_error("Log paths must be nonempty and single-line");
            *path = std::filesystem::weakly_canonical(std::filesystem::absolute(*path)).string();
            if (std::find(paths.begin(), paths.end(), *path) != paths.end())
                throw std::runtime_error("Each node needs a separate log path");
            paths.push_back(*path);
        }
    }
    if (routing_ == "fnv1a64-fmix64-ring-v1")
        ring_ = build_ring(ids, positions_per_group_);
}

std::string ClusterLayout::ownership_definition() const
{
    std::vector<std::string> ids;
    for (const GroupDefinition &group : groups_)
        ids.push_back(group.id);
    // Ring membership order has no meaning; modulo indices do.
    if (routing_ == "fnv1a64-fmix64-ring-v1")
        std::sort(ids.begin(), ids.end());
    std::ostringstream definition;
    definition << routing_ << ' ' << positions_per_group_;
    for (const std::string &id : ids)
        definition << ' ' << std::quoted(id);
    return definition.str();
}

const GroupDefinition &ClusterLayout::group(const std::string &id) const
{
    for (const GroupDefinition &group : groups_)
        if (group.id == id)
            return group;
    throw std::runtime_error("Unknown group in layout: " + id);
}

const GroupDefinition &ClusterLayout::owner(const std::string &key) const
{
    if (routing_ == "fnv1a64-mod-v1")
        return groups_[stable_key_hash(key) % groups_.size()];
    return group(group_for_key(ring_, key));
}

ClusterLayout read_layout(const std::string &file_path)
{
    std::ifstream file(file_path);
    if (!file)
        throw std::runtime_error("Could not read layout: " + file_path);
    std::string line;
    std::getline(file, line);
    std::istringstream header(line);
    std::string marker, version, routing;
    header >> marker >> version >> routing;
    const std::size_t positions = read_number(header, 1024);
    if (marker != "LAYOUT" || version != "1" || !reached_end(header))
        throw std::runtime_error("Expected LAYOUT 1 <routing-rule> <positions-per-group>");

    const auto directory = std::filesystem::absolute(file_path).parent_path();
    std::vector<GroupDefinition> groups;
    while (std::getline(file, line))
    {
        std::istringstream input(line);
        input >> std::ws;
        if (input.eof())
            continue;
        GroupDefinition group;
        input >> marker;
        if (marker != "GROUP" || !read_quoted(input, group.id) || !read_quoted(input, group.history_id))
            throw std::runtime_error("Expected GROUP with quoted group/history IDs");
        group.primary_port = static_cast<std::uint16_t>(read_number(input, 65535));
        group.replica_port = static_cast<std::uint16_t>(read_number(input, 65535));
        if (!read_quoted(input, group.primary_log) || !read_quoted(input, group.replica_log) || !reached_end(input))
            throw std::runtime_error("Expected two quoted log paths and no extra fields");
        if (group.primary_log.empty() || group.replica_log.empty())
            throw std::runtime_error("Log paths must not be empty");
        group.primary_log = (directory / group.primary_log).string();
        group.replica_log = (directory / group.replica_log).string();
        groups.push_back(std::move(group));
    }
    if (file.bad())
        throw std::runtime_error("Could not finish reading layout");
    return ClusterLayout(routing, positions, std::move(groups));
}
