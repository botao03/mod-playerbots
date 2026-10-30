/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTPOPULATION_H
#define PLAYERBOTS_BOTPOPULATION_H

#include "DataMap.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "SharedDefines.h"
#include <set>

class Player;

enum class BotPopulationPurpose : uint8
{
    World,
    Battleground,
    Arena,
    Dungeon
};

struct BotPopulationAssignment
{
    BotPopulationPurpose Purpose = BotPopulationPurpose::World;
    uint32 Map = 0;
    uint32 Zone = 0;
    uint32 Queue = 0;
    uint32 Bracket = 0;
    TeamId Team = TEAM_NEUTRAL;
    ObjectGuid Owner;
    uint32 Level = 1;
    uint8 Role = 0;
    bool Nearby = false;
    bool Rated = false;
    bool Endgame = false;
    bool ArenaLoadout = false;
    WorldLocation Location;
    std::set<uint32> Dungeons;
};

struct BotPopulationState final : DataMap::Base
{
    static constexpr char const* Key = "Playerbots.Population";
    BotPopulationAssignment Assignment;
    bool Prepared = false;
    bool Ready = false;
};

BotPopulationState const* GetBotPopulation(Player const* bot);
bool IsPopulationTravelAllowed(Player const* bot, WorldLocation const& location);

#endif
