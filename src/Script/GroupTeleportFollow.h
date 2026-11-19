/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GROUPTELEPORTFOLLOW_H
#define PLAYERBOTS_GROUPTELEPORTFOLLOW_H

#include "Define.h"

class Player;

void QueueGroupTeleportFollow(Player* leader, uint32 mapId, float x, float y, float z);
void UpdateGroupTeleportFollow();
void UpdateGroupOutOfSightFollow(Player* player);
// Visibility updates and companion phase changes run on the world thread.
void SynchronizeGroupFollowerPhase(Player* leader, Player* bot);
void UpdateGroupFollowerPhases(Player* leader);
void RestoreGroupFollowerPhases(Player* leader);
void UpdateGroupMountFollow(Player* master);
void ClearGroupMountFollow(Player* master);
bool IsWaitingForFlyingMaster(Player* bot);

#endif
