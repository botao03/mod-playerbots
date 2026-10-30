/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidShapeshiftActions.h"

#include <ctime>

#include "DruidPvpEscapePolicy.h"
#include "Playerbots.h"
#include "PvpCombatTriggers.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"

uint32 PvpBreakRootAction::GetFormSpell()
{
    switch (bot->GetShapeshiftForm())
    {
        case FORM_BEAR:
            return AI_VALUE2(uint32, "spell id", "bear form");
        case FORM_DIREBEAR:
            return AI_VALUE2(uint32, "spell id", "dire bear form");
        case FORM_CAT:
            return AI_VALUE2(uint32, "spell id", "cat form");
        case FORM_MOONKIN:
            return AI_VALUE2(uint32, "spell id", "moonkin form");
        case FORM_TREE:
            return AI_VALUE2(uint32, "spell id", "tree of life");
        case FORM_TRAVEL:
            return AI_VALUE2(uint32, "spell id", "travel form");
        case FORM_AQUA:
            return AI_VALUE2(uint32, "spell id", "aquatic form");
        default:
            return AI_VALUE2(uint32, "spell id", "cat form");
    }
}

bool PvpBreakRootAction::isUseful()
{
    if (!IsPvpSituationActive(botAI, PvpSituation::Slowed) || bot->HasAuraType(SPELL_AURA_MOD_STUN) ||
        bot->HasAuraType(SPELL_AURA_MOD_FEAR) || bot->HasAuraType(SPELL_AURA_MOD_CONFUSE))
        return false;

    Unit* opponent = GetPvpOpponent(botAI);
    if (!opponent)
        return false;

    DruidPvpEscapeState state;
    state.Rooted = bot->HasAuraType(SPELL_AURA_MOD_ROOT);
    state.Melee = PlayerbotAI::IsMelee(bot);
    state.EnemyInMeleeRange = bot->IsWithinMeleeRange(opponent);
    state.EnemyMoving = opponent->isMoving();
    state.EnemyPressuring = opponent->GetVictim() == bot || opponent->GetTarget() == bot->GetGUID();
    state.EnemyProtected =
        opponent->HasBreakableByDamageCrowdControlAura() ||
        (opponent->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) && opponent->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC));
    state.Casting = bot->IsNonMeleeSpellCast(false);
    state.HealthPct = bot->GetHealthPct();
    state.EnemyDistance = bot->GetDistance(opponent);
    time_t lastEscape = AI_VALUE2(time_t, "last spell cast time", "pvp break root");
    if (lastEscape)
        state.SecondsSinceEscape = int64_t(std::time(nullptr)) - int64_t(lastEscape);

    // Keep NPC roots on the PvE path, even during a duel or a fight involving both players and NPCs.
    AuraType impairment = state.Rooted ? SPELL_AURA_MOD_ROOT : SPELL_AURA_MOD_DECREASE_SPEED;
    for (AuraEffect const* effect : bot->GetAuraEffectsByType(impairment))
    {
        Unit* caster = effect->GetCaster();
        if (!caster || !caster->IsInWorld() || caster->GetMap() != bot->GetMap() ||
            !caster->GetCharmerOrOwnerPlayerOrPlayerItself() || !bot->IsValidAttackTarget(caster))
            continue;
        state.PlayerControl = true;
        state.ControlRemainingMs = effect->GetBase()->GetDuration();
        if (ShouldDruidPvpEscape(state))
            return true;
    }
    return false;
}

bool PvpBreakRootAction::isPossible()
{
    uint32 spellId = GetFormSpell();
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info || !bot->HasSpell(spellId) || bot->HasSpellCooldown(spellId))
        return false;

    uint32 cost = uint32(std::max(info->CalcPowerCost(bot, info->GetSchoolMask()), 0));
    if (!CanAffordDruidPvpEscape(bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA), cost, bot->GetHealthPct()))
        return false;

    // Clients cancel the active form before sending the next shapeshift cast.
    Spell check(bot, info, TriggerCastFlags(TRIGGERED_IGNORE_CAST_IN_PROGRESS | TRIGGERED_IGNORE_SHAPESHIFT));
    check.m_targets.SetUnitTarget(bot);
    return check.CheckCast(true) == SPELL_CAST_OK;
}

bool PvpBreakRootAction::Execute(Event /*event*/)
{
    if (!isUseful() || !isPossible())
        return false;

    uint32 spellId = GetFormSpell();
    bot->RemoveAurasDueToSpell(spellId);
    // Rate-limit cancellations too if returning to the form fails.
    context->GetValue<time_t>("last spell cast time", "pvp break root")->Set(std::time(nullptr));
    bool cast = botAI->CastSpell(spellId, bot);
    LOG_DEBUG("playerbots", "PvP root escape: bot {}, form {}, cast {}, still rooted {}", bot->GetName(), spellId, cast,
              bot->HasAuraType(SPELL_AURA_MOD_ROOT));
    if (cast)
        SetDuration(sPlayerbotAIConfig.ReactDelay);
    return cast;
}

bool CastBearFormAction::isUseful()
{
    return CastBuffSpellAction::isUseful() && !botAI->HasAura("dire bear form", GetTarget());
}

bool CastBearFormAction::isPossible()
{
    return CastBuffSpellAction::isPossible() && !botAI->HasAura("dire bear form", GetTarget());
}

std::vector<NextAction> CastDireBearFormAction::getAlternatives()
{
    return NextAction::merge({NextAction("bear form")}, CastSpellAction::getAlternatives());
}

bool CastTravelFormAction::isUseful()
{
    bool firstmount = bot->GetLevel() >= 20;

    // useful if no mount or with wsg flag
    return !bot->IsMounted() && (!firstmount || (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))) &&
           !botAI->HasAura("dash", bot);
}

bool CastCasterFormAction::Execute(Event /*event*/)
{
    botAI->RemoveShapeshift();
    return true;
}

bool CastCasterFormAction::isUseful()
{
    return botAI->HasAnyAuraOf(GetTarget(), "dire bear form", "bear form", "cat form", "travel form", "aquatic form",
                               "flight form", "swift flight form", "moonkin form", nullptr) &&
           AI_VALUE2(uint8, "mana", "self target") > sPlayerbotAIConfig.MediumHealth;
}

bool CastCancelDruidAction::Execute(Event /*event*/)
{
    botAI->RemoveAura(auraName);
    return true;
}

bool CastCancelDruidAction::isUseful() { return bot->HasAura(auraId); }

bool CastTreeFormAction::isUseful()
{
    constexpr uint32 SPELL_TREE_OF_LIFE = 33891;
    return GetTarget() && CastSpellAction::isUseful() && !bot->HasAura(SPELL_TREE_OF_LIFE);
}
