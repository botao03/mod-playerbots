/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTPOPULATIONPOLICY_H
#define PLAYERBOTS_BOTPOPULATIONPOLICY_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

inline bool IsPopulationCapital(uint32_t zone)
{
    constexpr std::array<uint32_t, 10> capitals = {1519, 1537, 1657, 1637, 1497, 1638, 3487, 3557, 3703, 4395};
    return std::find(capitals.begin(), capitals.end(), zone) != capitals.end();
}

inline bool IsPopulationPatrolZone(uint32_t anchor, uint32_t zone)
{
    return anchor == zone;
}

inline std::pair<uint32_t, uint32_t> PopulationCapitalLevelRange(uint32_t zone)
{
    constexpr uint32_t ZONE_DALARAN = 4395;
    constexpr uint32_t ZONE_SHATTRATH = 3703;
    if (zone == ZONE_DALARAN)
        return {80, 80};
    if (zone == ZONE_SHATTRATH)
        return {70, 79};
    return {10, 79};
}

inline uint32_t PopulationCapitalLevel(uint32_t zone, uint32_t roll, uint32_t lowerLevel)
{
    auto [low, high] = PopulationCapitalLevelRange(zone);
    return roll < 80 ? 80 : std::clamp(lowerLevel, low, high);
}

inline bool PopulationOperationDue(uint32_t now, uint32_t next)
{
    return now >= next;
}

struct PopulationZoneDemand
{
    uint32_t Map;
    uint32_t Zone;
    uint32_t Players;
    uint32_t Bots = 0;
};

inline std::vector<PopulationZoneDemand> PlanWorldPopulation(std::vector<PopulationZoneDemand> zones,
    uint32_t totalBots, uint32_t minimumPerZone)
{
    if (!totalBots || zones.empty())
        return {};
    zones.erase(std::remove_if(zones.begin(), zones.end(), [](auto const& zone)
    {
        return !zone.Players;
    }), zones.end());
    std::stable_sort(zones.begin(), zones.end(), [](auto const& left, auto const& right)
    {
        if (left.Players != right.Players)
            return left.Players > right.Players;
        return left.Map < right.Map || (left.Map == right.Map && left.Zone < right.Zone);
    });
    uint32_t limit = std::max(1u, totalBots / std::max(1u, minimumPerZone));
    if (zones.size() > limit)
        zones.resize(limit);
    for (uint32_t index = 0; index < zones.size(); ++index)
        zones[index].Bots = totalBots / zones.size() + (index < totalBots % zones.size() ? 1u : 0u);
    return zones;
}

inline bool ReserveArenaTeamSlots(uint32_t requested, uint32_t size, uint32_t& reserved)
{
    if (!size || reserved > requested || size > requested - reserved)
        return false;
    reserved += size;
    return true;
}

#endif
