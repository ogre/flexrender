#pragma once

#include <cstdint>
#include <string>

#include "msgpack.hpp"

#include "utils/tostring.hpp"

namespace fr {

struct TraversalState {
    enum State {
        NONE         = 0,
        FROM_PARENT  = 1,
        FROM_SIBLING = 2,
        FROM_CHILD   = 3
    };

    explicit TraversalState();

    /// The index of the current LinearNode we're visiting.
    size_t current;

    /// The current state of the 3-state automaton.
    uint32_t state;

    /// Whether or not a hit has been registered during traversal. This is
    /// technically a boolean, but padded to 4 bytes for alignment.
    uint32_t hit;

    TOSTRINGABLE(TraversalState);
};

std::string ToString(const TraversalState& state, const std::string& indent = "");

} // namespace fr
