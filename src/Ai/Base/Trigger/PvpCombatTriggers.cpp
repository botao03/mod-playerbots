/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpCombatTriggers.h"

#include "Playerbots.h"
#include "PvpCombatValues.h"
#include "Spell.h"
#include "SpellInfo.h"

namespace
{
constexpr uint8 PVP_DEFENSIVE_HEALTH_PCT = 50;
constexpr uint8 PVP_CRITICAL_HEALTH_PCT = 30;
constexpr uint8 PVP_MAGIC_PRESSURE_HEALTH_PCT = 70;
constexpr uint8 PVP_LOW_POWER_PCT = 25;

constexpr PvpSituation Situations[] = {
    PvpSituation::Controlled,    PvpSituation::Feared,       PvpSituation::FearOrSap,   PvpSituation::Rooted,
    PvpSituation::Slowed,        PvpSituation::Stunned,      PvpSituation::Hurt,        PvpSituation::Critical,
    PvpSituation::IncomingMagic, PvpSituation::EnemyCasting, PvpSituation::EnemyClose,  PvpSituation::Chase,
    PvpSituation::Burst,         PvpSituation::Afflicted,    PvpSituation::MagicDebuff, PvpSituation::LowPower};
}  // namespace

char const* PvpTriggerName(PvpSituation situation)
{
    switch (situation)
    {
        case PvpSituation::Controlled:
            return "pvp controlled";
        case PvpSituation::Feared:
            return "pvp feared";
        case PvpSituation::FearOrSap:
            return "pvp feared or sapped";
        case PvpSituation::Rooted:
            return "pvp rooted";
        case PvpSituation::Slowed:
            return "pvp slowed";
        case PvpSituation::Stunned:
            return "pvp stunned";
        case PvpSituation::Hurt:
            return "pvp hurt";
        case PvpSituation::Critical:
            return "pvp critical";
        case PvpSituation::IncomingMagic:
            return "pvp incoming magic";
        case PvpSituation::EnemyCasting:
            return "pvp enemy casting";
        case PvpSituation::EnemyClose:
            return "pvp enemy close";
        case PvpSituation::Chase:
            return "pvp chase";
        case PvpSituation::Burst:
            return "pvp burst";
        case PvpSituation::LowPower:
            return "pvp low power";
        case PvpSituation::Afflicted:
            return "pvp afflicted";
        case PvpSituation::MagicDebuff:
            return "pvp magic debuff";
    }
    return "";
}

Unit* GetPvpOpponent(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!sPlayerbotAIConfig.PvpCombatTactics || !bot->IsAlive() || !bot->IsInWorld() || bot->IsInFlight() ||
        bot->GetVehicle() || bot->IsBeingTeleported() || bot->m_mover != bot)
        return nullptr;

    bool dueling = bot->duel && bot->duel->State == DUEL_STATE_IN_PROGRESS;
    if (!bot->IsInCombat() && !dueling)
        return nullptr;

    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (dueling)
        target = bot->duel->Opponent;

    if (!target || !target->IsInWorld() || !target->IsAlive() || target->GetMap() != bot->GetMap() ||
        !target->IsControlledByPlayer() || (!bot->IsValidAttackTarget(target) && !bot->IsCharmed()))
        return nullptr;

    return target;
}

Spell* GetIncomingMagicSpell(Unit* caster, Player* bot)
{
    if (!caster || !caster->IsAlive() || !caster->IsInWorld() || caster->GetMap() != bot->GetMap())
        return nullptr;

    for (CurrentSpellTypes slot : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
    {
        Spell* spell = caster->GetCurrentSpell(slot);
        if (spell && spell->getState() != SPELL_STATE_FINISHED && spell->GetSpellInfo() &&
            !spell->GetSpellInfo()->IsPositive() &&
            (spell->GetSpellInfo()->GetSchoolMask() & SPELL_SCHOOL_MASK_MAGIC) &&
            spell->m_targets.GetUnitTargetGUID() == bot->GetGUID())
            return spell;
    }
    return nullptr;
}

bool IsPvpSituationActive(PlayerbotAI* botAI, PvpSituation situation)
{
    Unit* opponent = GetPvpOpponent(botAI);
    if (!opponent)
        return false;

    Player* bot = botAI->GetBot();
    bool rooted = bot->HasAuraType(SPELL_AURA_MOD_ROOT);
    switch (situation)
    {
        case PvpSituation::Controlled:
            return rooted || bot->HasAuraType(SPELL_AURA_MOD_STUN) || bot->HasAuraType(SPELL_AURA_MOD_FEAR) ||
                   bot->HasAuraType(SPELL_AURA_MOD_CONFUSE) || bot->HasAuraType(SPELL_AURA_MOD_CHARM) ||
                   bot->HasAuraType(SPELL_AURA_AOE_CHARM) ||
                   (botAI->GetAiObjectContext()->GetValue<uint32>("pvp debuff flags")->Get() & PVP_DEBUFF_SLEEP);
        case PvpSituation::Feared:
            return bot->HasAuraType(SPELL_AURA_MOD_FEAR) || bot->HasAuraType(SPELL_AURA_MOD_CHARM) ||
                   bot->HasAuraType(SPELL_AURA_AOE_CHARM) ||
                   (botAI->GetAiObjectContext()->GetValue<uint32>("pvp debuff flags")->Get() & PVP_DEBUFF_SLEEP);
        case PvpSituation::FearOrSap:
            return bot->HasAuraType(SPELL_AURA_MOD_FEAR) ||
                   (botAI->GetAiObjectContext()->GetValue<uint32>("pvp debuff flags")->Get() & PVP_DEBUFF_SAPPED);
        case PvpSituation::Rooted:
            return rooted;
        case PvpSituation::Slowed:
            return rooted || (bot->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) &&
                              (PlayerbotAI::IsMelee(bot) != bot->IsWithinMeleeRange(opponent)));
        case PvpSituation::Stunned:
            return botAI->GetAiObjectContext()->GetValue<uint32>("pvp debuff flags")->Get() & PVP_DEBUFF_STUN;
        case PvpSituation::Hurt:
            return bot->HealthBelowPct(PVP_DEFENSIVE_HEALTH_PCT);
        case PvpSituation::Critical:
            return bot->HealthBelowPct(PVP_CRITICAL_HEALTH_PCT);
        case PvpSituation::IncomingMagic:
        {
            ObjectGuid caster = botAI->GetAiObjectContext()->GetValue<ObjectGuid>("pvp magic caster")->Get();
            return GetIncomingMagicSpell(botAI->GetUnit(caster), bot);
        }
        case PvpSituation::EnemyCasting:
        {
            Spell* spell = opponent->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!spell)
                spell = opponent->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            return spell && spell->GetSpellInfo() && spell->getState() != SPELL_STATE_FINISHED &&
                   !opponent->HasAuraType(SPELL_AURA_MOD_SILENCE);
        }
        case PvpSituation::EnemyClose:
            return bot->IsWithinMeleeRange(opponent) &&
                   (opponent->GetVictim() == bot || opponent->GetTarget() == bot->GetGUID()) &&
                   !(botAI->GetAiObjectContext()->GetValue<uint32>("pvp debuff flags")->Get() &
                     PVP_OPPONENT_BREAKABLE_CC);
        case PvpSituation::Chase:
            return PlayerbotAI::IsMelee(bot) && (!bot->IsWithinMeleeRange(opponent) || opponent->isMoving()) &&
                   !opponent->HasAuraType(SPELL_AURA_MOD_ROOT) &&
                   !opponent->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) &&
                   !(botAI->GetAiObjectContext()->GetValue<uint32>("pvp debuff flags")->Get() &
                     PVP_OPPONENT_BREAKABLE_CC);
        case PvpSituation::Burst:
            return !bot->HealthBelowPct(PVP_DEFENSIVE_HEALTH_PCT) &&
                   bot->IsWithinDistInMap(opponent, botAI->GetRange("spell")) &&
                   !(botAI->GetAiObjectContext()->GetValue<uint32>("pvp debuff flags")->Get() &
                     PVP_OPPONENT_BREAKABLE_CC) &&
                   !opponent->HasAuraType(SPELL_AURA_SCHOOL_IMMUNITY) &&
                   (!PlayerbotAI::IsMelee(bot) || bot->IsWithinMeleeRange(opponent));
        case PvpSituation::LowPower:
        {
            Powers power = bot->getPowerType();
            uint32 maximum = bot->GetMaxPower(power);
            return maximum && bot->GetPower(power) < CalculatePct(maximum, PVP_LOW_POWER_PCT);
        }
        case PvpSituation::Afflicted:
            return botAI->GetAiObjectContext()->GetValue<uint32>("pvp debuff flags")->Get() & PVP_DEBUFF_STONEFORM;
        case PvpSituation::MagicDebuff:
            return bot->HealthBelowPct(PVP_MAGIC_PRESSURE_HEALTH_PCT) &&
                   (botAI->GetAiObjectContext()->GetValue<uint32>("pvp debuff flags")->Get() & PVP_DEBUFF_MAGIC);
    }
    return false;
}

bool PvpCombatTrigger::IsActive() { return IsPvpSituationActive(botAI, _situation); }

void RegisterPvpCombatTriggers(NamedObjectFactory<Trigger>& factory)
{
    for (PvpSituation situation : Situations)
        factory.creators[PvpTriggerName(situation)] = [situation](PlayerbotAI* botAI)
        { return new PvpCombatTrigger(botAI, situation); };
}
