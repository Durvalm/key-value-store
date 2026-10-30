#pragma once

#include <cstddef>
#include <string>

// Offline operation: source nodes stopped, all clients stopped, candidate nodes
// restarted after transfer. Reverify before atomically publishing a full layout.
// Does not restart processes or resume traffic. Returns verified live key count.
std::size_t activate_layout(const std::string &source_path,
                            const std::string &destination_path,
                            const std::string &active_path);
