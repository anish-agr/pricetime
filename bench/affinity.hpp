#pragma once

// Affinity helpers moved into the library (include/pricetime/thread_util.hpp)
// once the engine needed them too; this forwarder keeps bench code unchanged.
#include "pricetime/thread_util.hpp"

namespace pricetime::bench {
using pricetime::pick_performance_core;
using pricetime::pin_current_thread;
using pricetime::raise_priority;
}  // namespace pricetime::bench
