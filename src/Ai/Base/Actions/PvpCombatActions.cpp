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
#include "Opcodes.h"
#include "Pet.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldPacket.h"

namespace
{
constexpr uint32 SPELL_PVP_TRINKET = 42292;
constexpr uint32 SPELL_PVP_TRINKET_TOC = 65547;
constexpr uint32 ITEM_THORIUM_GRENADE = 15993;
constexpr uint32 ITEM_ADAMANTITE_GRENADE = 23737;
constexpr uint32 ITEM_FROST_GRENADE = 32413;

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
    if (_triggerName.empty() || !GetPvpOpponent(botAI))
        return false;
    Trigger* trigger = context->GetTrigger(_triggerName);
    if (!trigger || !trigger->IsActive() || !CastSpellAction::isUseful())
        return false;
    if (botAI->HasAura(spell, GetTarget()))
        return false;
    if (!_selfTarget && GetTarget()->HasBreakableByDamageCrowdControlAura())
        return false;
    return true;
}

bool PvpSpellAction::isPossible()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
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
        Spell* incoming = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!incoming)
            incoming = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
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
        if (spell == "shadowmeld")
            bot->AttackStop();
        cast = CastSpellAction::Execute(event);
    }
    if (cast)
    {
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
        return SelectItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1)) ||
               SelectItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET2));
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
