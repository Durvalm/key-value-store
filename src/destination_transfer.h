#pragma once

#include "cluster_layout.h"
#include "source_snapshot.h"

#include <cstddef>
#include <vector>

// Reject shared histories, log/lock path overlap and physical aliases.
void validate_transfer_layouts(const ClusterLayout &source, const ClusterLayout &destination);

// Destination pairs must be running, fresh and otherwise unused. Preflight every
// group before sending SETs; return acknowledged counts in destination group order.
// Throws on first unconfirmed/rejected operation. No retries or layout activation.
std::vector<std::size_t> transfer_snapshot(const SourceSnapshot &snapshot,
                                         const ClusterLayout &destination);
