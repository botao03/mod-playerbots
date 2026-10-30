/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpCombatValues.h"

#include "Playerbots.h"
#include "PvpCombatTriggers.h"
#include "SpellAuras.h"
#include "SpellInfo.h"

ObjectGuid PvpMagicCasterValue::Calculate()
{
    Unit* opponent = GetPvpOpponent(botAI);
    if (!opponent)
        return ObjectGuid::Empty;

    if (GetIncomingMagicSpell(opponent, bot))
        return opponent->GetGUID();

    for (ObjectGuid guid : AI_VALUE(GuidVector, "attackers"))
    {
        Unit* attacker = botAI->GetUnit(guid);
        if (attacker && attacker->IsControlledByPlayer() && bot->IsValidAttackTarget(attacker) &&
            GetIncomingMagicSpell(attacker, bot))
            return guid;
    }
    return ObjectGuid::Empty;
}

uint32 PvpDebuffFlagsValue::Calculate()
{
    Unit* opponent = GetPvpOpponent(botAI);
    if (!opponent)
        return 0;

    uint32 flags = 0;
    if (opponent->HasBreakableByDamageCrowdControlAura())
        flags |= PVP_OPPONENT_BREAKABLE_CC;
    for (auto const& entry : bot->GetAppliedAuras())
    {
        if (entry.second->IsPositive())
            continue;
        SpellInfo const* info = entry.second->GetBase()->GetSpellInfo();
        uint64 mechanics = info->GetAllEffectsMechanicMask();
        if (mechanics & (1ULL << MECHANIC_SLEEP))
            flags |= PVP_DEBUFF_SLEEP;
        if (mechanics & (1ULL << MECHANIC_SAPPED))
            flags |= PVP_DEBUFF_SAPPED;
        if (mechanics & (1ULL << MECHANIC_STUN))
            flags |= PVP_DEBUFF_STUN;
        if (info->Dispel == DISPEL_POISON || info->Dispel == DISPEL_DISEASE || (mechanics & (1ULL << MECHANIC_BLEED)))
            flags |= PVP_DEBUFF_STONEFORM;
        if (info->GetSchoolMask() & SPELL_SCHOOL_MASK_MAGIC)
            flags |= PVP_DEBUFF_MAGIC;
    }
    return flags;
}
