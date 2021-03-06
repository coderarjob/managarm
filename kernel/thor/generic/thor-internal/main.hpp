#pragma once

#include <thor-internal/initgraph.hpp>

namespace thor {

extern initgraph::Engine basicInitEngine;
extern initgraph::Engine extendedInitEngine;
initgraph::Stage *getTaskingAvailableStage();

} // namespace thor
