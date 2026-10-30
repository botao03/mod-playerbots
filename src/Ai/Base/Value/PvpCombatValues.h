/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PVPCOMBATVALUES_H
#define PLAYERBOTS_PVPCOMBATVALUES_H

#include "ObjectGuid.h"
#include "Value.h"

inline constexpr uint32 PVP_TACTICAL_CACHE_INTERVAL = 200;

enum PvpDebuffFlags : uint32
{
    PVP_DEBUFF_SLEEP = 1,
    PVP_DEBUFF_STUN = 2,
    PVP_DEBUFF_STONEFORM = 4,
    PVP_DEBUFF_MAGIC = 8,
    PVP_DEBUFF_SAPPED = 16,
    PVP_OPPONENT_BREAKABLE_CC = 32
};

class PvpDebuffFlagsValue : public CalculatedValue<uint32>
{
public:
    PvpDebuffFlagsValue(PlayerbotAI* botAI)
        : CalculatedValue<uint32>(botAI, "pvp debuff flags", PVP_TACTICAL_CACHE_INTERVAL)
    {
    }
    uint32 Calculate() override;
};

class PvpMagicCasterValue : public CalculatedValue<ObjectGuid>
{
public:
    PvpMagicCasterValue(PlayerbotAI* botAI)
        : CalculatedValue<ObjectGuid>(botAI, "pvp magic caster", PVP_TACTICAL_CACHE_INTERVAL)
    {
    }
    ObjectGuid Calculate() override;
};

#endif
