/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BATTLEGROUNDTACTICSPOLICY_H
#define PLAYERBOTS_BATTLEGROUNDTACTICSPOLICY_H

#include <algorithm>
#include <cstdint>

struct BattlegroundObjectiveDemand
{
    bool Owned = false;
    bool Assaulted = false;
    bool Neutral = false;
    bool Preferred = false;
    bool Current = false;
    uint32_t Assigned = 0;
    uint32_t Enemies = 0;
    uint32_t Capacity = 1;
    float Distance = 0.0f;
    float Importance = 0.0f;
};

inline uint32_t BattlegroundObjectiveCapacity(uint32_t teamSize, uint32_t attackObjectives,
    bool owned, bool assaulted, uint32_t enemies)
{
    constexpr uint32_t MAX_ASSAULT_SQUAD = 5;
    constexpr uint32_t MAX_REINFORCEMENTS = 8;
    uint32_t reinforcementLimit = std::clamp((teamSize + 2) / 3, 2u, MAX_REINFORCEMENTS);
    if (owned || assaulted)
        return enemies ? std::clamp(enemies + 1, 2u, reinforcementLimit) : assaulted ? 2u : 1u;
    uint32_t objectives = std::max(1u, attackObjectives);
    uint32_t capacity = std::clamp((teamSize + objectives - 1) / objectives, 2u, MAX_ASSAULT_SQUAD);
    return std::max(capacity, std::min(enemies + 1, reinforcementLimit));
}

inline float BattlegroundObjectiveScore(BattlegroundObjectiveDemand const& demand)
{
    constexpr float CROWDING_PENALTY = 1200.0f;
    constexpr float EXTRA_PLAYER_PENALTY = 300.0f;
    constexpr float DISTANCE_WEIGHT = 0.35f;
    bool defending = demand.Owned || demand.Assaulted;
    float score = demand.Importance;
    if (defending)
    {
        score += demand.Enemies ? 900.0f + std::min(demand.Enemies, 5u) * 40.0f
                                : demand.Assaulted ? 350.0f : 150.0f;
        if (!demand.Enemies && demand.Assigned < demand.Capacity)
            score += demand.Owned ? 450.0f : 250.0f;
    }
    else
        score += demand.Neutral ? 700.0f : 500.0f;
    score += demand.Preferred ? 160.0f : 0.0f;
    score += demand.Current ? 80.0f : 0.0f;
    score += demand.Assigned ? std::min(demand.Assigned, 2u) * 25.0f : 90.0f;
    score -= std::min(demand.Distance, 1500.0f) * DISTANCE_WEIGHT;
    if (demand.Assigned >= demand.Capacity)
        score -= CROWDING_PENALTY + (demand.Assigned - demand.Capacity) * EXTRA_PLAYER_PENALTY;
    return score;
}

inline uint32_t BattlegroundEscortCount(uint32_t teamSize)
{
    return std::clamp((teamSize + 3) / 4, 1u, 3u);
}

#endif
