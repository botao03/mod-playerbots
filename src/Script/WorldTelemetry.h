/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_WORLDTELEMETRY_H
#define PLAYERBOTS_WORLDTELEMETRY_H

#include "Define.h"

class Player;

void InitializeWorldTelemetry();
void SampleWorldTelemetry(Player* player, uint32 diff);
void RemoveWorldTelemetry(Player* player);
void UpdateWorldTelemetry(uint32 diff);

#endif
