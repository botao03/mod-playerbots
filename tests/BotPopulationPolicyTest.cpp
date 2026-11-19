/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotPopulationPolicy.h"
#include <cassert>
#include <numeric>

int main()
{
    assert(!IsPopulationStaged(true, 1));
    assert(!IsPopulationStaged(true, 2));
    assert(!IsPopulationStaged(true, 3));
    assert(!IsPopulationStaged(true, 128));
    assert(IsPopulationStaged(true, BOT_POPULATION_STAGING_PHASE_MASK));
    assert(IsPopulationStaged(false, 1));
    assert(IsPopulationStaged(false, 2));
    assert(IsPopulationStaged(false, BOT_POPULATION_STAGING_PHASE_MASK));

    assert(IsPopulationPatrolZone(1519, 1519));
    assert(!IsPopulationPatrolZone(1519, 12));
    assert(!IsPopulationPatrolZone(1519, 14));
    assert(!IsPopulationPatrolZone(12, 1519));
    assert(IsPopulationPatrolZone(12, 12));
    assert(IsPopulationPatrolZone(40, 40));
    assert(!IsPopulationPatrolZone(12, 40));
    assert(!IsPopulationPatrolZone(40, 12));
    for (auto const& [capital, outskirts] : std::array<std::pair<uint32_t, uint32_t>, 10>{{
        {1519, 12}, {1537, 1}, {1657, 141}, {1637, 14}, {1497, 85},
        {1638, 215}, {3487, 3430}, {3557, 3524}, {3703, 3519}, {4395, 2817}}})
    {
        assert(IsPopulationCapital(capital));
        assert(!IsPopulationCapital(outskirts));
        assert(IsPopulationPatrolZone(capital, capital));
        assert(!IsPopulationPatrolZone(capital, outskirts));
        assert(!IsPopulationPatrolZone(outskirts, capital));
    }
    for (uint32_t zone : {1519u, 1537u, 1657u, 1637u, 1497u, 1638u, 3487u, 3557u})
    {
        assert(PopulationCapitalLevelRange(zone) == std::make_pair(10u, 79u));
        uint32_t maximumLevel = 0;
        for (uint32_t roll = 0; roll < 100; ++roll)
            for (uint32_t level = 10; level < 80; ++level)
            {
                uint32_t selected = PopulationCapitalLevel(zone, roll, level);
                maximumLevel += selected == 80;
                assert(selected == 80 || (selected >= 10 && selected <= 79));
                if (roll >= 80)
                    assert(selected == level);
            }
        assert(maximumLevel == 80 * 70);
        assert(PopulationCapitalLevel(zone, 80, 0) == 10);
        assert(PopulationCapitalLevel(zone, 99, 100) == 79);
    }
    assert(PopulationCapitalLevelRange(4395) == std::make_pair(80u, 80u));
    assert(PopulationCapitalLevelRange(3703) == std::make_pair(70u, 79u));
    for (uint32_t roll = 0; roll < 100; ++roll)
        for (uint32_t level = 0; level <= 100; ++level)
        {
            assert(PopulationCapitalLevel(4395, roll, level) == 80);
            uint32_t shattrath = PopulationCapitalLevel(3703, roll, level);
            assert(shattrath >= 70 && shattrath <= 80);
            assert(shattrath == (roll < 80 ? 80u : std::clamp(level, 70u, 79u)));
        }

    assert(PlanWorldPopulation({}, 50, 5).empty());
    assert(PlanWorldPopulation({{0, 12, 0}}, 50, 5).empty());
    assert(PlanWorldPopulation({{0, 12, 1}}, 0, 5).empty());
    auto crowded = PlanWorldPopulation({{0, 12, 1000}, {1, 14, 1}}, 50, 5);
    assert(crowded.size() == 2 && crowded[0].Bots + crowded[1].Bots == 50);
    auto cityAndOutdoors = PlanWorldPopulation({{0, 1519, 1}, {0, 12, 1}, {0, 40, 1}}, 50, 5);
    assert(cityAndOutdoors.size() == 3);
    assert(cityAndOutdoors[0].Zone == 12 && cityAndOutdoors[1].Zone == 40 && cityAndOutdoors[2].Zone == 1519);
    assert(cityAndOutdoors[0].Bots + cityAndOutdoors[1].Bots + cityAndOutdoors[2].Bots == 50);

    std::vector<PopulationZoneDemand> dispersed;
    for (uint32_t index = 0; index < 100; ++index)
        dispersed.push_back({index % 2, index, index + 1});
    auto limited = PlanWorldPopulation(dispersed, 50, 5);
    assert(limited.size() == 10 && limited.front().Zone == 99 && limited.back().Zone == 90);

    for (uint32_t count = 0; count <= 500; ++count)
        for (uint32_t minimum : {0u, 1u, 5u, 50u})
        {
            auto plan = PlanWorldPopulation(dispersed, count, minimum);
            uint32_t sum = std::accumulate(plan.begin(), plan.end(), 0u,
                [](uint32_t total, auto const& zone) { return total + zone.Bots; });
            assert(sum == count);
            for (auto const& zone : plan)
                assert(zone.Players && zone.Bots);
            if (!plan.empty())
                assert(plan.front().Bots - plan.back().Bots <= 1);
        }
    uint32_t reserved = 0;
    assert(ReserveArenaTeamSlots(3, 2, reserved));
    assert(reserved == 2);
    assert(!ReserveArenaTeamSlots(3, 2, reserved));
    assert(reserved == 2);
    assert(!ReserveArenaTeamSlots(1, 2, reserved));
    assert(!ReserveArenaTeamSlots(10, 0, reserved));
    assert(reserved == 2);

    auto stable = PlanWorldPopulation({{1, 14, 1}, {0, 12, 1}}, 1, 5);
    assert(stable.size() == 1 && stable[0].Map == 0 && stable[0].Zone == 12);

    assert(SplitWorldPopulationByFaction(50, 0, 0) == std::make_pair(0u, 0u));
    assert(SplitWorldPopulationByFaction(0, 1, 1) == std::make_pair(0u, 0u));
    assert(SplitWorldPopulationByFaction(50, 3, 0) == std::make_pair(50u, 0u));
    assert(SplitWorldPopulationByFaction(50, 0, 3) == std::make_pair(0u, 50u));
    assert(SplitWorldPopulationByFaction(50, 1, 1) == std::make_pair(25u, 25u));
    assert(SplitWorldPopulationByFaction(40, 3, 1) == std::make_pair(30u, 10u));
    assert(SplitWorldPopulationByFaction(40, 1, 3) == std::make_pair(10u, 30u));
    assert(SplitWorldPopulationByFaction(50, 1000, 1) == std::make_pair(49u, 1u));
    assert(SplitWorldPopulationByFaction(50, 1, 1000) == std::make_pair(1u, 49u));
    assert(SplitWorldPopulationByFaction(UINT32_MAX, UINT32_MAX, UINT32_MAX) ==
        std::make_pair(2147483648u, 2147483647u));
    for (uint32_t bots = 0; bots <= 50; ++bots)
        for (uint32_t alliance = 0; alliance <= 10; ++alliance)
            for (uint32_t horde = 0; horde <= 10; ++horde)
            {
                auto [allianceBots, hordeBots] = SplitWorldPopulationByFaction(bots, alliance, horde);
                assert(allianceBots + hordeBots == (alliance || horde ? bots : 0u));
                assert(alliance || !allianceBots);
                assert(horde || !hordeBots);
                if (bots >= 2 && alliance && horde)
                    assert(allianceBots && hordeBots);
            }
}
