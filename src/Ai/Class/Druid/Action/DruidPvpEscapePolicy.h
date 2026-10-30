/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DRUIDPVPESCAPEPOLICY_H
#define PLAYERBOTS_DRUIDPVPESCAPEPOLICY_H

#include <algorithm>
#include <cstdint>
#include <limits>

struct DruidPvpEscapeState
{
    bool PlayerControl = false;
    bool Rooted = false;
    bool Melee = false;
    bool EnemyInMeleeRange = false;
    bool EnemyMoving = false;
    bool EnemyPressuring = false;
    bool EnemyProtected = false;
    bool Casting = false;
    float HealthPct = 100.0f;
    float EnemyDistance = 0.0f;
    int32_t ControlRemainingMs = 0;
    int64_t SecondsSinceEscape = std::numeric_limits<int64_t>::max();
};

inline bool ShouldDruidPvpEscape(DruidPvpEscapeState const& state)
{
    constexpr float CRITICAL_HEALTH_PCT = 25.0f;
    constexpr float PRESSURED_HEALTH_PCT = 50.0f;
    constexpr float ESCAPE_DISTANCE = 10.0f;
    constexpr int64_t NORMAL_ESCAPE_INTERVAL = 8;
    constexpr int64_t CRITICAL_ESCAPE_INTERVAL = 3;
    constexpr int32_t MIN_ROOT_REMAINING_MS = 2000;
    constexpr int32_t MIN_SNARE_REMAINING_MS = 4000;
    bool critical = state.HealthPct <= CRITICAL_HEALTH_PCT;
    int64_t interval = critical ? CRITICAL_ESCAPE_INTERVAL : NORMAL_ESCAPE_INTERVAL;
    int32_t minimumDuration = state.Rooted ? MIN_ROOT_REMAINING_MS : MIN_SNARE_REMAINING_MS;
    if (!state.PlayerControl || state.EnemyProtected || state.SecondsSinceEscape < interval ||
        (state.ControlRemainingMs >= 0 && state.ControlRemainingMs <= minimumDuration))
        return false;

    if (state.Melee)
        return !state.EnemyInMeleeRange && !state.Casting &&
               (state.Rooted || (state.EnemyMoving && state.EnemyDistance >= ESCAPE_DISTANCE));

    return state.Rooted && state.EnemyPressuring && state.HealthPct <= PRESSURED_HEALTH_PCT &&
           state.EnemyDistance <= ESCAPE_DISTANCE && (!state.Casting || critical);
}

inline bool CanAffordDruidPvpEscape(uint32_t mana, uint32_t maximumMana, uint32_t cost, float healthPct)
{
    constexpr float CRITICAL_HEALTH_PCT = 25.0f;
    constexpr uint32_t NORMAL_MANA_RESERVE_PCT = 40;
    constexpr uint32_t CRITICAL_MANA_RESERVE_PCT = 20;
    constexpr uint32_t PERCENT = 100;
    if (!maximumMana || mana < cost)
        return false;
    if (!cost)
        return true;

    uint32_t reservePct = healthPct <= CRITICAL_HEALTH_PCT ? CRITICAL_MANA_RESERVE_PCT : NORMAL_MANA_RESERVE_PCT;
    uint64_t reserve = std::max(uint64_t(maximumMana) * reservePct / PERCENT, uint64_t(cost));
    return uint64_t(mana - cost) >= reserve;
}

#endif
