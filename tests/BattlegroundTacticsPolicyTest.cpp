/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BattlegroundTacticsPolicy.h"
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace
{
struct Point
{
    float X;
    float Y;
};

std::vector<uint32_t> AssignTeam(uint32_t teamSize, std::vector<Point> const& nodes, Point start,
    int32_t owned = -1, int32_t threatened = -1)
{
    std::vector<uint32_t> counts(nodes.size());
    for (uint32_t player = 0; player < teamSize; ++player)
    {
        uint32_t selected = 0;
        float best = -std::numeric_limits<float>::max();
        for (uint32_t node = 0; node < nodes.size(); ++node)
        {
            BattlegroundObjectiveDemand demand;
            demand.Owned = int32_t(node) == owned || int32_t(node) == threatened;
            demand.Neutral = !demand.Owned;
            demand.Enemies = int32_t(node) == threatened ? 10 : 0;
            demand.Preferred = node == (player / 3) % nodes.size();
            demand.Assigned = counts[node];
            demand.Capacity = BattlegroundObjectiveCapacity(teamSize, nodes.size() - (owned >= 0),
                demand.Owned, false, demand.Enemies);
            demand.Distance = std::hypot(nodes[node].X - start.X, nodes[node].Y - start.Y);
            float score = BattlegroundObjectiveScore(demand);
            if (score > best)
            {
                best = score;
                selected = node;
            }
        }
        ++counts[selected];
    }
    return counts;
}
}

int main()
{
    std::vector<Point> arathi = {{1166.8f, 1200.1f}, {977.0f, 1046.6f}, {806.2f, 874.3f},
        {856.1f, 1148.9f}, {1146.9f, 848.2f}};
    for (Point spawn : {Point{1286.1f, 1282.5f}, Point{702.9f, 703.0f}})
        for (uint32_t size : {8u, 15u})
        {
            auto counts = AssignTeam(size, arathi, spawn);
            assert(std::accumulate(counts.begin(), counts.end(), 0u) == size);
            assert(*std::min_element(counts.begin(), counts.end()) >= 1);
            assert(*std::max_element(counts.begin(), counts.end()) <= 3);
        }
    auto relief = AssignTeam(15, arathi, arathi[1], -1, 1);
    assert(relief[1] <= 5);
    assert(relief[3] && relief[4]);

    BattlegroundObjectiveDemand guard;
    guard.Owned = true;
    guard.Current = true;
    guard.Capacity = 1;
    BattlegroundObjectiveDemand capture;
    capture.Neutral = true;
    capture.Distance = 300.0f;
    capture.Capacity = 3;
    assert(BattlegroundObjectiveScore(guard) > BattlegroundObjectiveScore(capture));
    guard.Assigned = 1;
    assert(BattlegroundObjectiveScore(guard) < BattlegroundObjectiveScore(capture));

    std::vector<Point> eye = {{2044.2f, 1727.5f}, {2048.3f, 1395.1f},
        {2286.2f, 1404.7f}, {2284.7f, 1728.5f}};
    auto bases = AssignTeam(15, eye, {2200.0f, 1550.0f});
    assert(*std::min_element(bases.begin(), bases.end()) >= 3);
    assert(*std::max_element(bases.begin(), bases.end()) <= 4);

    auto isle = AssignTeam(40, {{773.0f, -880.0f}, {726.0f, -360.0f}, {780.0f, -1150.0f},
        {251.0f, -1159.0f}, {1266.0f, -400.0f}}, {425.0f, -800.0f});
    assert(*std::min_element(isle.begin(), isle.end()) > 0);
    assert(*std::max_element(isle.begin(), isle.end()) < 15);

    for (uint32_t size = 1; size <= 40; ++size)
    {
        assert(BattlegroundEscortCount(size) <= 3);
        assert(BattlegroundEscortCount(size) <= size);
        assert(BattlegroundObjectiveCapacity(size, 0, true, false, 0) == 1);
        assert(BattlegroundObjectiveCapacity(size, 0, false, true, 0) == 2);
        assert(BattlegroundObjectiveCapacity(size, 3, true, false, 40) <= 8);
    }
}
