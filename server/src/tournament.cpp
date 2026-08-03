#include "tournament.h"

#include <climits>

namespace tournament {

// 6-max sit-and-go structure: 100 BB starting stack (1000 chips), slow-ish
// early levels, per-player antes from level 5 on, then the blinds ramp up.
static const BlindLevel kSchedule[] = {
        { 5, 10, 0 },    // level 1
        { 10, 20, 0 },   // 2
        { 15, 30, 0 },   // 3
        { 25, 50, 0 },   // 4
        { 40, 80, 5 },   // 5
        { 60, 120, 10 }, // 6
        { 100, 200, 15 }, // 7
        { 150, 300, 25 }, // 8
        { 250, 500, 40 }, // 9
        { 400, 800, 60 }, // 10
        { 600, 1200, 100 }, // 11
        { 1000, 2000, 150 }, // 12
};
static constexpr int kScheduleSize = sizeof(kSchedule) / sizeof(kSchedule[0]);

BlindLevel
level_blinds(int level)
{
        if (level < 1) return { SmallBlind, BigBlind, 0 };
        if (level <= kScheduleSize) return kSchedule[level - 1];
        BlindLevel l = kSchedule[kScheduleSize - 1];
        for (int i = kScheduleSize; i < level; i++) {
                if (l.big > INT_MAX / 2) break; // saturate: never overflow
                l.small *= 2;
                l.big *= 2;
                l.ante *= 2;
        }
        return l;
}

void
set_level(Game_State *state, int level, double now)
{
        state->level = level;
        BlindLevel b = level_blinds(level);
        state->small_blind = b.small;
        state->big_blind   = b.big;
        state->ante        = b.ante;
        state->level_started_at = now;
}

bool
step(Game_State *state, double now)
{
        if (state->level < 1) return false;
        if (now - state->level_started_at < state->level_seconds) return false;
        set_level(state, state->level + 1, now);
        return true;
}

} // namespace tournament
