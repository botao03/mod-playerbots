/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYREPLACEMENT_H
#define PLAYERBOTS_ACTIVITYREPLACEMENT_H

#include "BotPopulation.h"
#include <vector>

class Player;
class WorldPacket;
class WorldSession;

struct ActivityReplacementRequest
{
    BotPopulationAssignment Assignment;
    uint32 Count = 0;
};

void ObserveActivityKickVote(WorldSession* session, WorldPacket const& packet);
void UpdateActivityReplacements(uint32 diff);
std::vector<ActivityReplacementRequest> GetDungeonReplacementRequests();
bool HasPendingDungeonReplacement(ObjectGuid group);
uint32 GetBattlegroundReplacementHold(uint32 queue, uint32 bracket, TeamId team);
bool IsActivityReplacementExcluded(BotPopulationAssignment const& assignment, ObjectGuid bot);
void AttachDungeonReplacement(Player* bot, BotPopulationAssignment const& assignment);
void AddActivityReplacementScripts();

#endif
