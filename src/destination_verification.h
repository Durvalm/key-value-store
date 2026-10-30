#pragma once

#include "cluster_layout.h"
#include "source_snapshot.h"

#include <cstddef>
#include <vector>

// Read-only verification of both copies: identities, exact values/counts, and
// unchanged matching progress. Returns verified key counts in group order.
// Normal destination traffic must remain paused. Does not activate a layout.
// for_activation also requires primaries to have cleared import/failure flags.
std::vector<std::size_t> verify_destination(const SourceSnapshot &snapshot,
                                           const ClusterLayout &destination,
                                           bool for_activation = false);
