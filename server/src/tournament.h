#pragma once

#include "poker.h"

// Tournament blind schedule and level timer (pure engine, no raylib).
// Levels are 1-based; past the fixed table the blinds keep doubling, so a
// tournament always eventually ends.

struct BlindLevel {
        int small{ SmallBlind };
        int big{ BigBlind };
        int ante{ 0 };
};

namespace tournament {

// blinds of a 1-based level (0 = not started: the cash-mode defaults)
BlindLevel level_blinds(int level);

// set the level and its blinds, restarting the level timer
void set_level(Game_State *state, int level, double now);

// advance the level when its time is up; returns true when it changed
bool step(Game_State *state, double now);

} // namespace tournament
