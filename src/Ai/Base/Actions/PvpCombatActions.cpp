/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpCombatActions.h"

#include <algorithm>

#include "GenericBuffUtils.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "MotionMaster.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Playerbots.h"
#include "RacialSpells.h"
#include "RogueActions.h"
#include "RogueOpeningActions.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "WorldPacket.h"

namespace
{
constexpr uint32 SPELL_PVP_TRINKET = 42292;
constexpr uint32 SPELL_PVP_TRINKET_TOC = 65547;
constexpr uint32 ITEM_THORIUM_GRENADE = 15993;
constexpr uint32 ITEM_ADAMANTITE_GRENADE = 23737;
constexpr uint32 ITEM_FROST_GRENADE = 32413;
constexpr uint8 PVP_CRITICAL_HEALTH_PCT = 30;
constexpr int32 PVP_MIN_CONTROL_REMAINING_MS = 1500;
constexpr float PVP_MIN_SHADOWSTEP_DISTANCE = 8.0f;

bool HasOtherBreakableControl(Unit const* target, Aura const* control)
{
    Aura const* blind = target->GetAura(PVP_ROGUE_BLIND, control->GetCasterGUID());
    for (AuraType type :
         {SPELL_AURA_MOD_CONFUSE, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_STUN, SPELL_AURA_MOD_ROOT, SPELL_AURA_TRANSFORM})
        for (AuraEffect const* effect : target->GetAuraEffectsByType(type))
            if (effect->GetBase() != control && effect->GetBase() != blind &&
                (effect->GetSpellInfo()->AuraInterruptFlags & AURA_INTERRUPT_FLAG_TAKE_DAMAGE))
                return true;
    return false;
}

bool HasPeriodicDamage(Unit const* unit)
{
    return unit->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) || unit->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT) ||
           unit->HasAuraType(SPELL_AURA_PERIODIC_LEECH);
}

bool ShouldBreakPvpControl(PlayerbotAI* botAI, std::string const& spell)
{
    Player* bot = botAI->GetBot();
    uint64 mechanics = 0;
    if (spell == "blink")
        mechanics = (1ULL << MECHANIC_ROOT) | (1ULL << MECHANIC_STUN);
    else if (spell == "hand of freedom" || spell == "master's call" || spell == "escape artist")
        mechanics = (1ULL << MECHANIC_ROOT) | (1ULL << MECHANIC_SNARE);
    else if (spell == "will of the forsaken")
        mechanics = (1ULL << MECHANIC_FEAR) | (1ULL << MECHANIC_CHARM) | (1ULL << MECHANIC_SLEEP);
    else if (spell == "berserker rage")
        mechanics = (1ULL << MECHANIC_FEAR) | (1ULL << MECHANIC_SAPPED) | (1ULL << MECHANIC_KNOCKOUT);
    else if (spell == "pvp trinket" || spell == "every man for himself")
        mechanics = (1ULL << MECHANIC_ROOT) | (1ULL << MECHANIC_STUN) | (1ULL << MECHANIC_FEAR) |
                    (1ULL << MECHANIC_CHARM) | (1ULL << MECHANIC_DISORIENTED) | (1ULL << MECHANIC_POLYMORPH) |
                    (1ULL << MECHANIC_SLEEP) | (1ULL << MECHANIC_SAPPED) | (1ULL << MECHANIC_KNOCKOUT);
    else
        return true;

    Unit* opponent = GetPvpOpponent(botAI);
    if (!opponent)
        return false;
    for (auto const& entry : bot->GetAppliedAuras())
    {
        Aura* aura = entry.second->GetBase();
        if (entry.second->IsPositive() || !(aura->GetSpellInfo()->GetAllEffectsMechanicMask() & mechanics))
            continue;
        if (aura->GetDuration() >= 0 && aura->GetDuration() <= PVP_MIN_CONTROL_REMAINING_MS &&
            !bot->HealthBelowPct(PVP_CRITICAL_HEALTH_PCT))
            continue;
        uint64 mask = aura->GetSpellInfo()->GetAllEffectsMechanicMask();
        bool movementOnly = (mask & mechanics & ~((1ULL << MECHANIC_ROOT) | (1ULL << MECHANIC_SNARE))) == 0;
        if (movementOnly && !bot->HealthBelowPct(PVP_CRITICAL_HEALTH_PCT))
        {
            if (PlayerbotAI::IsMelee(bot) ? bot->IsWithinMeleeRange(opponent)
                                          : !IsPvpSituationActive(botAI, PvpSituation::EnemyClose))
                continue;
        }
        return true;
    }
    return false;
}

bool CheckCast(Unit* caster, SpellInfo const* info, Unit* target, Item* item = nullptr)
{
    if (!caster || !info || !target || !caster->IsAlive() || caster->HasSpellCooldown(info->Id))
        return false;

    if (Player* player = caster->ToPlayer())
        if (ai::spell::HasSpellOrCategoryCooldown(player, info->Id))
            return false;

    // Reactions interrupt our cast only after selection; keep every other core restriction.
    Spell check(caster, info, TRIGGERED_IGNORE_CAST_IN_PROGRESS);
    check.m_CastItem = item;
    check.m_targets.SetUnitTarget(target);
    if (info->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION)
        check.m_targets.SetDst(*target);
    return check.CheckCast(true) == SPELL_CAST_OK;
}
}  // namespace

bool PreparePvpDuelAction::isUseful()
{
    return IsPvpSituationActive(botAI, PvpSituation::DuelPreparation) &&
           (bot->isMoving() || bot->IsMounted() || bot->GetVictim() || !bot->IsStandState());
}

bool PreparePvpDuelAction::Execute(Event /*event*/)
{
    if (!isUseful() || bot->IsFlying())
        return false;
    bot->AttackStop();
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    bot->SetStandState(UNIT_STAND_STATE_STAND);
    if (bot->IsMounted())
    {
        WorldPacket packet;
        bot->GetSession()->HandleCancelMountAuraOpcode(packet);
    }
    return true;
}

bool StopPvpAttackAction::isUseful()
{
    if (!IsPvpSituationActive(botAI, PvpSituation::EnemyControlled))
        return false;
    Unit* target = GetPvpOpponent(botAI);
    Pet* pet = bot->GetPet();
    if (bot->GetVictim() == target || (pet && pet->GetVictim() == target))
        return true;
    for (CurrentSpellTypes slot : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL, CURRENT_AUTOREPEAT_SPELL})
        if (Spell* cast = bot->GetCurrentSpell(slot))
            if (!cast->GetSpellInfo()->IsPositive() && cast->m_targets.GetUnitTargetGUID() == target->GetGUID())
                return true;
    return false;
}

bool StopPvpAttackAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;
    Unit* target = GetPvpOpponent(botAI);
    for (CurrentSpellTypes slot : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL, CURRENT_AUTOREPEAT_SPELL})
        if (Spell* cast = bot->GetCurrentSpell(slot))
            if (!cast->GetSpellInfo()->IsPositive() && cast->m_targets.GetUnitTargetGUID() == target->GetGUID())
                bot->InterruptSpell(slot);
    bot->AttackStop();
    if (Pet* pet = bot->GetPet())
        if (pet->GetVictim() == GetPvpOpponent(botAI))
            pet->AttackStop();
    return true;
}

bool ShouldDelayPvpDefensiveSpell(PlayerbotAI* botAI, std::string const& spell)
{
    if ((spell == "ice block" || spell == "divine shield" || spell == "deterrence" || spell == "dispersion" ||
         spell == "anti-magic shell" || spell == "cloak of shadows" || spell == "barkskin" ||
         spell == "icebound fortitude" || spell == "divine protection" || spell == "pain suppression" ||
         spell == "shield wall" || spell == "shamanistic rage") &&
        botAI->HasAnyAuraOf(botAI->GetBot(), "ice block", "divine shield", "deterrence", "dispersion",
                            "anti-magic shell", "cloak of shadows", "barkskin", "icebound fortitude",
                            "divine protection", "pain suppression", "shield wall", "shamanistic rage", nullptr))
        return true;
    return false;
}

bool ShouldDelayPvpSpell(PlayerbotAI* botAI, std::string const& spell, Unit* target)
{
    Player* bot = botAI->GetBot();
    Unit* opponent = GetPvpOpponent(botAI);
    if (!target || !opponent)
        return false;
    if (target == bot)
        return ShouldDelayPvpDefensiveSpell(botAI, spell);
    if (!target->IsControlledByPlayer() || !bot->IsValidAttackTarget(target))
        return false;

    uint32 id = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
    SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
    if (!info)
        return false;
    bool chainingControl = false;
    if (target->HasBreakableByDamageCrowdControlAura())
    {
        Aura const* control = bot->HasStealthAura() ? GetPvpRogueControl(bot, target) : nullptr;
        if (!control || HasOtherBreakableControl(target, control))
            return true;

        bool opener = spell == "cheap shot" || spell == "garrote" || spell == "ambush";
        bool finishingControl =
            control->GetDuration() > 0 && control->GetDuration() <= PVP_ROGUE_CONTROL_CHAIN_WINDOW_MS;
        chainingControl = (opener && finishingControl) ||
                          (spell == "sap" && target == opponent &&
                           IsPvpSituationActive(botAI, PvpSituation::SapAfterBlind));
        if (!chainingControl && spell != "premeditation")
            return true;
    }
    if (target->IsImmunedToSpell(info))
        return true;

    DiminishingGroup group = GetDiminishingReturnsGroupForSpell(info, false);
    if (group != DIMINISHING_NONE && target->GetDiminishing(group) >= DIMINISHING_LEVEL_IMMUNE)
        return true;
    constexpr uint64 CONTROL_MECHANICS = (1ULL << MECHANIC_STUN) | (1ULL << MECHANIC_FEAR) |
                                         (1ULL << MECHANIC_DISORIENTED) | (1ULL << MECHANIC_POLYMORPH) |
                                         (1ULL << MECHANIC_SLEEP) | (1ULL << MECHANIC_SAPPED);
    if (!chainingControl && (info->GetAllEffectsMechanicMask() & CONTROL_MECHANICS) &&
        target->HasUnitState(UNIT_STATE_CONTROLLED))
        return true;

    if (info->HasEffect(SPELL_EFFECT_INTERRUPT_CAST))
    {
        Spell* incoming = GetPvpEnemyCast(target);
        if (!incoming)
            return true;
        SpellInfo const* castInfo = incoming->GetSpellInfo();
        bool channel = incoming == target->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (channel ? !(castInfo->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT)
                    : !(castInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT))
            return true;
        // Let a normal cast progress before committing an interrupt; channels are already dealing effects.
        if (!channel && !bot->HealthBelowPct(PVP_CRITICAL_HEALTH_PCT) &&
            incoming->GetCastTimeRemaining() > incoming->GetCastTime() / 2)
            return true;
    }
    return false;
}

void PvpSpellAction::Qualify(std::string const value)
{
    Qualified::Qualify(value);
    _triggerName.clear();
    spell.clear();
    size_t separator = value.find('|');
    if (separator == std::string::npos)
        return;
    _triggerName = value.substr(0, separator);
    spell = value.substr(separator + 1);
}

Unit* PvpSpellAction::GetTarget() { return _selfTarget ? bot : GetPvpOpponent(botAI); }

bool PvpSpellAction::isUseful()
{
    bool preparation = _selfTarget && _triggerName == PvpTriggerName(PvpSituation::DuelPreparation);
    if (_triggerName.empty() || (!preparation && !GetPvpOpponent(botAI)))
        return false;
    Trigger* trigger = context->GetTrigger(_triggerName);
    if (!trigger || !trigger->IsActive() || !CastSpellAction::isUseful())
        return false;
    Unit* target = GetTarget();
    if (botAI->HasAura(spell, target))
        return false;
    bool openingSap = _triggerName == PvpTriggerName(PvpSituation::SapOpening);
    if (spell == "sap" &&
        (!CanRogueSapTarget(target) || (openingSap && _sappedDuel == bot->GetGuidValue(PLAYER_DUEL_ARBITER))))
        return false;
    if ((spell == "sap" || spell == "blind") && HasPeriodicDamage(target))
        return false;
    if (spell == "premeditation" && AI_VALUE2(uint8, "combo", "current target") > 3)
        return false;
    if (spell == "kidney shot" && AI_VALUE2(uint8, "combo", "current target") < 4)
        return false;
    if (spell == "eviscerate" && AI_VALUE2(uint8, "combo", "current target") < 4)
        return false;
    if (spell == "shadowstep" && bot->IsWithinDistInMap(target, PVP_MIN_SHADOWSTEP_DISTANCE))
        return false;
    if ((spell == "stealth" || spell == "vanish") && (IsRogueCarryingFlag(bot) || bot->HasStealthAura()))
        return false;
    if ((spell == "vanish" || _triggerName == PvpTriggerName(PvpSituation::RogueRestealth)) && HasPeriodicDamage(bot))
        return false;
    if (!preparation && ShouldDelayPvpSpell(botAI, spell, target))
        return false;
    return true;
}

bool PvpSpellAction::isPossible()
{
    if (_selfTarget && !ShouldBreakPvpControl(botAI, spell))
        return false;
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info || info->IsPassive())
        return false;

    if (spellId == Acore::Racials::Stoneform && sSpellMgr->GetSpellInfo(Acore::Racials::StoneformImmunity) &&
        bot->HasAura(Acore::Racials::Stoneform))
        return false;

    Unit* caster = bot;
    if (!bot->HasSpell(spellId))
    {
        Pet* pet = bot->GetPet();
        if (!pet || !pet->HasSpell(spellId))
            return false;
        caster = pet;
    }

    _interruptCast = caster == bot;
    _interruptMovement = caster == bot && info->CalcCastTime(caster) > 0;
    Unit* target = GetTarget();
    if (!target || (!_selfTarget && target->IsImmunedToSpell(info)))
        return false;

    if (_selfTarget && _triggerName == "pvp enemy casting")
    {
        float radius = 0.0f;
        for (SpellEffectInfo const& effect : info->Effects)
            radius = std::max(radius, effect.CalcRadius(caster));
        Unit* opponent = GetPvpOpponent(botAI);
        if (!opponent || !radius || !bot->IsWithinDistInMap(opponent, radius))
            return false;
    }

    if (info->HasEffect(SPELL_EFFECT_INTERRUPT_CAST))
    {
        Spell* incoming = GetPvpEnemyCast(target);
        if (!incoming || !incoming->GetSpellInfo())
            return false;
        if (!(incoming->GetSpellInfo()->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) &&
            !(incoming->GetSpellInfo()->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT))
            return false;
    }

    if (spell == "shadowmeld" && bot->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE))
        return false;
    if (spell == "sprint" && bot->IsWithinMeleeRange(GetPvpOpponent(botAI)))
        return false;
    if (_selfTarget && ShouldDelayPvpDefensiveSpell(botAI, spell))
        return false;
    return CheckCast(caster, info, target);
}

bool PvpSpellAction::Execute(Event event)
{
    if (!isUseful() || !isPossible())
        return false;

    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    bool cast = false;
    if (!bot->HasSpell(spellId))
    {
        Pet* pet = bot->GetPet();
        cast = pet && pet->CastSpell(GetTarget(), spellId, TRIGGERED_NONE) == SPELL_CAST_OK;
    }
    else
    {
        if (spell == "shadowmeld" || spell == "vanish" || spell == "blind")
            bot->AttackStop();
        cast = CastSpellAction::Execute(event);
    }
    if (cast)
    {
        if (spell == "sap" && _triggerName == PvpTriggerName(PvpSituation::SapOpening))
        {
            _sappedDuel = bot->GetGuidValue(PLAYER_DUEL_ARBITER);
            LOG_DEBUG("playerbots", "PvP duel: {} uses sap before the damage opener (arbiter {})", bot->GetName(),
                      _sappedDuel.ToString());
        }
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        uint32 castTime = bot->HasSpell(spellId) && info ? info->CalcCastTime(bot) : 0;
        SetDuration(std::max(sPlayerbotAIConfig.ReactDelay, castTime));
    }
    return cast;
}

bool PvpUseItemAction::isUseful()
{
    return _grenade
               ? sPlayerbotAIConfig.PvpUseEngineeringItems && !bot->InArena() && bot->HasSkill(SKILL_ENGINEERING) &&
                     (IsPvpSituationActive(botAI, PvpSituation::EnemyCasting) ||
                      IsPvpSituationActive(botAI, PvpSituation::Chase))
               : IsPvpSituationActive(botAI, PvpSituation::Controlled);
}

bool PvpUseItemAction::SelectItem(Item* item)
{
    if (!item || bot->CanUseItem(item) != EQUIP_ERR_OK)
        return false;
    ItemTemplate const* proto = item->GetTemplate();
    if (_grenade && !IsPvpSituationActive(botAI, item->GetEntry() == ITEM_FROST_GRENADE ? PvpSituation::Chase
                                                                                        : PvpSituation::EnemyCasting))
        return false;
    if (bot->InArena() &&
        (proto->HasFlag(ITEM_FLAG_NOT_USEABLE_IN_ARENA) ||
         (proto->Class == ITEM_CLASS_CONSUMABLE && !proto->HasFlag(ITEM_FLAG_IGNORE_DEFAULT_ARENA_RESTRICTIONS))))
        return false;

    for (auto const& entry : proto->Spells)
    {
        if (entry.SpellId <= 0 || entry.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;
        uint32 spellId = entry.SpellId;
        if (!_grenade && spellId != SPELL_PVP_TRINKET && spellId != SPELL_PVP_TRINKET_TOC)
            continue;
        Unit* target = _grenade ? GetPvpOpponent(botAI) : bot;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!CheckCast(bot, info, target, item))
            continue;
        if (_grenade)
        {
            if (bot->isMoving() || target->IsImmunedToSpell(info) || target->HasBreakableByDamageCrowdControlAura())
                continue;
            float radius = 0.0f;
            for (SpellEffectInfo const& effect : info->Effects)
                radius = std::max(radius, effect.CalcRadius(bot));
            bool crowdControlNearby = false;
            for (ObjectGuid guid : AI_VALUE(GuidVector, "possible targets"))
            {
                Unit* nearby = botAI->GetUnit(guid);
                if (nearby && nearby->IsInWorld() && nearby->GetMap() == target->GetMap() &&
                    nearby->IsWithinDistInMap(target, radius) && nearby->HasBreakableByDamageCrowdControlAura())
                {
                    crowdControlNearby = true;
                    break;
                }
            }
            if (crowdControlNearby)
                continue;
        }
        _itemGuid = item->GetGUID();
        _spellId = spellId;
        return true;
    }
    return false;
}

bool PvpUseItemAction::isPossible()
{
    _itemGuid.Clear();
    _spellId = 0;
    if (!isUseful())
        return false;
    if (!_grenade)
        return ShouldBreakPvpControl(botAI, "pvp trinket") &&
               (SelectItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1)) ||
                SelectItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET2)));
    for (uint32 entry : {ITEM_ADAMANTITE_GRENADE, ITEM_THORIUM_GRENADE, ITEM_FROST_GRENADE})
        if (SelectItem(bot->GetItemByEntry(entry)))
            return true;
    return false;
}

bool PvpUseItemAction::Execute(Event /*event*/)
{
    if (!isUseful() || !isPossible())
        return false;
    Item* item = bot->GetItemByGuid(_itemGuid);
    Unit* target = _grenade ? GetPvpOpponent(botAI) : bot;
    if (!item || !target)
        return false;

    SpellCastTargets targets;
    targets.SetUnitTarget(target);
    if (_grenade)
        targets.SetDst(*target);
    WorldPacket packet(CMSG_USE_ITEM);
    packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << _spellId << item->GetGUID() << uint32(0) << uint8(0);
    targets.Write(packet);
    bot->GetSession()->HandleUseItemOpcode(packet);
    SetDuration(sPlayerbotAIConfig.GlobalCoolDown);
    return true;
}
