/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTRIDINGPOLICY_H
#define PLAYERBOTS_BOTRIDINGPOLICY_H

#include <array>
#include <cstdint>

enum BotRidingSkillRank : uint16_t
{
    BOT_RIDING_APPRENTICE = 75,
    BOT_RIDING_JOURNEYMAN = 150,
    BOT_RIDING_EXPERT = 225,
    BOT_RIDING_ARTISAN = 300
};

struct BotRidingRank
{
    uint8_t Level;
    uint16_t Skill;
    uint32_t Spell;
};

inline constexpr std::array<BotRidingRank, 4> BotRidingRanks = {{{20, BOT_RIDING_APPRENTICE, 33388},
    {40, BOT_RIDING_JOURNEYMAN, 33391}, {60, BOT_RIDING_EXPERT, 34090}, {70, BOT_RIDING_ARTISAN, 34091}}};
inline constexpr uint8_t BotColdWeatherFlyingLevel = 77;

constexpr uint16_t BotRidingSkillForLevel(uint8_t level)
{
    uint16_t skill = 0;
    for (auto const& rank : BotRidingRanks)
        if (level >= rank.Level)
            skill = rank.Skill;
    return skill;
}

enum class BotFlightFollowAction
{
    Follow,
    Wait,
    Return
};

struct BotFlightFollowState
{
    uint64_t Master = 0;
    uint64_t Group = 0;

    void Clear()
    {
        Master = 0;
        Group = 0;
    }

    BotFlightFollowAction Update(uint64_t master, uint64_t group, bool flyingMount, bool riding, bool canFly)
    {
        if (!master || !group || Master != master || Group != group)
            Clear();

        if (!master || !group)
            return BotFlightFollowAction::Follow;

        if (flyingMount)
        {
            if (canFly)
            {
                Clear();
                return BotFlightFollowAction::Follow;
            }
            Master = master;
            Group = group;
            return BotFlightFollowAction::Wait;
        }

        if (!Master)
            return BotFlightFollowAction::Follow;

        return riding ? BotFlightFollowAction::Wait : BotFlightFollowAction::Return;
    }
};

#endif
