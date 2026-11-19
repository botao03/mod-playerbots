/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpCombatTriggers.h"

#include "GameTime.h"
#include "Playerbots.h"
#include "PvpCombatValues.h"
#include "RogueOpeningActions.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"

namespace
{
constexpr uint8 PVP_DEFENSIVE_HEALTH_PCT = 50;
constexpr uint8 PVP_CRITICAL_HEALTH_PCT = 30;
constexpr uint8 PVP_MAGIC_PRESSURE_HEALTH_PCT = 70;
constexpr uint8 PVP_LOW_POWER_PCT = 25;
constexpr uint8 PVP_EXECUTE_HEALTH_PCT = 20;
constexpr time_t PVP_DUEL_STEALTH_GRACE_SECONDS = 3;
constexpr uint32 SPELL_ROGUE_SAP_RANK_1 = 6770;

constexpr PvpSituation Situations[] = {
    PvpSituation::DuelPreparation, PvpSituation::DuelStart,            PvpSituation::Opening,
    PvpSituation::CasterOpening,   PvpSituation::SapOpening,           PvpSituation::SapAfterBlind,
    PvpSituation::RogueRestealth,   PvpSituation::RogueVanishAfterBlind, PvpSituation::RogueReset,
    PvpSituation::MeleePressure,   PvpSituation::EnemyControlled,      PvpSituation::Controlled,
    PvpSituation::Feared,          PvpSituation::FearOrSap,            PvpSituation::Rooted,
    PvpSituation::Slowed,          PvpSituation::Stunned,              PvpSituation::Hurt,
    PvpSituation::Critical,        PvpSituation::IncomingMagic,        PvpSituation::EnemyCasting,
    PvpSituation::EnemyClose,      PvpSituation::Chase,                PvpSituation::Burst,
    PvpSituation::Afflicted,       PvpSituation::MagicDebuff,          PvpSituation::LowPower};
}  // namespace

char const* PvpTriggerName(PvpSituation situation)
{
    switch (situation)
    {
        case PvpSituation::DuelPreparation:
            return "pvp duel preparation";
        case PvpSituation::DuelStart:
            return "pvp duel start";
        case PvpSituation::Opening:
            return "pvp opening";
        case PvpSituation::CasterOpening:
            return "pvp caster opening";
        case PvpSituation::SapOpening:
            return "pvp sap opening";
        case PvpSituation::SapAfterBlind:
            return "pvp sap after blind";
        case PvpSituation::RogueRestealth:
            return "pvp rogue restealth";
        case PvpSituation::RogueVanishAfterBlind:
            return "pvp rogue vanish after blind";
        case PvpSituation::RogueReset:
            return "pvp rogue reset";
        case PvpSituation::MeleePressure:
            return "pvp melee pressure";
        case PvpSituation::EnemyControlled:
            return "pvp enemy controlled";
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

Unit* GetPvpDuelOpponent(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!sPlayerbotAIConfig.PvpCombatTactics || !bot->IsAlive() || !bot->IsInWorld() || bot->IsBeingTeleported() ||
        bot->IsInFlight() || bot->GetVehicle() || bot->m_mover != bot || !IsAcceptedDuel(bot))
        return nullptr;

    Player* opponent = bot->duel->Opponent;
    return opponent && opponent->IsAlive() && opponent->IsInWorld() && bot->IsInMap(opponent) ? opponent : nullptr;
}

Spell* GetPvpEnemyCast(Unit* opponent)
{
    if (!opponent || opponent->HasUnitState(UNIT_STATE_CONTROLLED))
        return nullptr;

    for (CurrentSpellTypes slot : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
    {
        Spell* spell = opponent->GetCurrentSpell(slot);
        if (!spell || (spell->getState() != SPELL_STATE_PREPARING && spell->getState() != SPELL_STATE_CASTING) ||
            !spell->GetSpellInfo())
            continue;

        SpellInfo const* info = spell->GetSpellInfo();
        if (!info->IsPositive() || info->HasEffect(SPELL_EFFECT_HEAL) || info->HasEffect(SPELL_EFFECT_HEAL_PCT) ||
            info->HasAura(SPELL_AURA_PERIODIC_HEAL))
            return spell;
    }
    return nullptr;
}

Aura const* GetPvpRogueControl(Player const* bot, Unit const* target)
{
    if (bot->getClass() != CLASS_ROGUE || !target->IsPlayer())
        return nullptr;
    if (Aura const* sap = target->GetAuraOfRankedSpell(SPELL_ROGUE_SAP_RANK_1, bot->GetGUID()))
        return sap;
    return target->GetAura(PVP_ROGUE_BLIND, bot->GetGUID());
}

Unit* GetPvpOpponent(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!sPlayerbotAIConfig.PvpCombatTactics || !bot->IsAlive() || !bot->IsInWorld() || bot->IsInFlight() ||
        bot->GetVehicle() || bot->IsBeingTeleported() || bot->m_mover != bot)
        return nullptr;

    bool dueling = bot->duel && bot->duel->State == DUEL_STATE_IN_PROGRESS;
    bool inPvpFight = bot->IsInCombat() || dueling;
    if (!inPvpFight && (bot->getClass() != CLASS_ROGUE || botAI->GetState() != BOT_STATE_COMBAT))
        return nullptr;

    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    if (dueling)
        target = bot->duel->Opponent;

    if (!target || !target->IsInWorld() || !target->IsAlive() || target->GetMap() != bot->GetMap() ||
        !target->IsControlledByPlayer() || !bot->CanSeeOrDetect(target) ||
        (!bot->IsValidAttackTarget(target) && !bot->IsCharmed()))
        return nullptr;

    // The combat engine keeps the current target during a rogue's out-of-combat opener.
    if (!inPvpFight && (!target->IsPlayer() || !bot->IsWithinDistInMap(target, sPlayerbotAIConfig.SightDistance) ||
                        (!bot->HasStealthAura() && !GetPvpRogueControl(bot, target))))
        return nullptr;

    if (!inPvpFight && (sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId()) ||
                        sPlayerbotAIConfig.IsPvpProhibited(target->GetZoneId(), target->GetAreaId())))
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
    if (situation == PvpSituation::DuelPreparation || situation == PvpSituation::DuelStart)
    {
        Unit* opponent = GetPvpDuelOpponent(botAI);
        if (!opponent)
            return false;
        Player* bot = botAI->GetBot();
        Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        if (situation == PvpSituation::DuelPreparation)
            return bot->duel->State == DUEL_STATE_COUNTDOWN || (!bot->IsInCombat() && target != opponent);
        if (bot->getClass() == CLASS_ROGUE && !bot->IsInCombat() && !bot->HasStealthAura() &&
            bot->duel->State == DUEL_STATE_IN_PROGRESS &&
            GameTime::GetGameTime().count() < bot->duel->StartTime + PVP_DUEL_STEALTH_GRACE_SECONDS)
        {
            uint32 stealth = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "stealth")->Get();
            if (stealth && bot->HasSpell(stealth) && !bot->HasSpellCooldown(stealth))
                return false;
        }
        return bot->duel->State == DUEL_STATE_IN_PROGRESS && bot->CanSeeOrDetect(opponent) &&
               bot->IsValidAttackTarget(opponent) && (target != opponent || botAI->GetState() != BOT_STATE_COMBAT);
    }

    Unit* opponent = GetPvpOpponent(botAI);
    if (!opponent)
        return false;

    Player* bot = botAI->GetBot();
    bool rooted = bot->HasAuraType(SPELL_AURA_MOD_ROOT);
    switch (situation)
    {
        case PvpSituation::DuelPreparation:
        case PvpSituation::DuelStart:
            return false;
        case PvpSituation::Opening:
            return bot->HasStealthAura() || botAI->HasAura("shadow dance", bot);
        case PvpSituation::CasterOpening:
        {
            Player* opponentPlayer = opponent->ToPlayer();
            return opponentPlayer && (bot->HasStealthAura() || botAI->HasAura("shadow dance", bot)) &&
                   opponentPlayer->getPowerType() == POWER_MANA && !PlayerbotAI::IsMelee(opponentPlayer);
        }
        case PvpSituation::SapOpening:
            return bot->getClass() == CLASS_ROGUE && GetPvpDuelOpponent(botAI) == opponent &&
                   CanRogueSapTarget(opponent) && bot->HasStealthAura() && !bot->IsInCombat() &&
                   !opponent->IsInCombat() && !opponent->HasBreakableByDamageCrowdControlAura();
        case PvpSituation::SapAfterBlind:
        case PvpSituation::RogueVanishAfterBlind:
        {
            if (situation == PvpSituation::SapAfterBlind)
            {
                if (!bot->HasStealthAura() || bot->IsInCombat() || opponent->IsInCombat() ||
                    !CanRogueSapTarget(opponent))
                    return false;
            }
            else if (!bot->IsInCombat() || bot->HasStealthAura() || !bot->IsWithinMeleeRange(opponent))
                return false;

            Aura const* control = GetPvpRogueControl(bot, opponent);
            return control && control->GetId() == PVP_ROGUE_BLIND && control->GetDuration() > 0 &&
                   control->GetDuration() <= PVP_ROGUE_CONTROL_CHAIN_WINDOW_MS;
        }
        case PvpSituation::RogueRestealth:
            return !bot->IsInCombat() && !bot->HasStealthAura() && GetPvpRogueControl(bot, opponent);
        case PvpSituation::RogueReset:
            return bot->getClass() == CLASS_ROGUE && bot->IsInCombat() && !bot->HasStealthAura() &&
                   !GetPvpRogueControl(bot, opponent) &&
                   (bot->HealthBelowPct(PVP_CRITICAL_HEALTH_PCT) ||
                    (bot->HealthBelowPct(PVP_DEFENSIVE_HEALTH_PCT) &&
                     !opponent->HealthBelowPct(PVP_EXECUTE_HEALTH_PCT)) ||
                    (rooted && !bot->IsWithinMeleeRange(opponent)));
        case PvpSituation::EnemyControlled:
            return opponent->HasBreakableByDamageCrowdControlAura();
        case PvpSituation::MeleePressure:
            return !bot->HasStealthAura() && bot->IsWithinMeleeRange(opponent) &&
                   !opponent->HasBreakableByDamageCrowdControlAura();
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
            return GetPvpEnemyCast(opponent) && !opponent->HasAuraType(SPELL_AURA_MOD_SILENCE);
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
                   (!PlayerbotAI::IsMelee(bot) || bot->IsWithinMeleeRange(opponent)) &&
                   (opponent->HealthBelowPct(PVP_DEFENSIVE_HEALTH_PCT) ||
                    opponent->HasUnitState(UNIT_STATE_CONTROLLED) || opponent->HasAuraType(SPELL_AURA_MOD_SILENCE));
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
