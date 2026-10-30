/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_EMPTY_INSTANCE_CLEANUP_H
#define PLAYERBOTS_EMPTY_INSTANCE_CLEANUP_H

#include "Define.h"

class Player;

bool IsClientControlledInstancePlayer(Player* player);
void UpdateEmptyInstanceCleanup(uint32 diff);
void AddEmptyInstanceCleanupScripts();

#endif
