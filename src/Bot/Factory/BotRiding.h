/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTRIDING_H
#define PLAYERBOTS_BOTRIDING_H

class Player;

void InitializeLoadoutRidingSkills(Player& player);
void InitializeLoadoutMounts(Player& player);
void InitializeBotRiding(Player* bot);
void UpdateBotRiding(Player* bot);
bool HasBotFlightRiding(Player const* bot);
bool IsUsingFlyingMount(Player const* player);
bool IsUsingMountOrTravelForm(Player const* player);

#endif
