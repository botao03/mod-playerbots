/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChooseTargetActions.h"
#include "ChooseRpgTargetAction.h"
#include "Event.h"
#include "LootObjectStack.h"
#include "NewRpgStrategy.h"
#include "Playerbots.h"
#include "PossibleRpgTargetsValue.h"
#include "PvpTriggers.h"
#include "RtiTargetValue.h"
#include "ServerFacade.h"
#include "SpellMgr.h"

bool AttackEnemyPlayerAction::isUseful()
{
    if (PlayerHasFlag::IsCapturingFlag(bot))
        return false;

    return !sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId());
}

bool AttackEnemyFlagCarrierAction::isUseful()
{
    Unit* target = context->GetValue<Unit*>("enemy flag carrier")->Get();
    return target && !PlayerbotAI::IsHeal(bot) && !bot->IsNonMeleeSpellCast(false) &&
        ServerFacade::instance().IsDistanceLessOrEqualThan(
            ServerFacade::instance().GetDistance2d(bot, target), 100.0f) &&
        !PlayerHasFlag::IsCapturingFlag(bot) && target != AI_VALUE(Unit*, "current target");
}

bool AggressiveTargetAction::isUseful()
{
    if (bot->IsInCombat())
        return false;

    return true;
}

bool DropTargetAction::Execute(Event /*event*/)
{
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (target && target->isDead())
    {
        ObjectGuid guid = target->GetGUID();
        if (guid)
            context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);
    }

    // ObjectGuid pullTarget = context->GetValue<ObjectGuid>("pull target")->Get();
    // GuidVector possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();

    // if (pullTarget && find(possible.begin(), possible.end(), pullTarget) == possible.end())
    // {
    //     context->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
    // }

    context->GetValue<Unit*>("current target")->Set(nullptr);

    bot->SetTarget(ObjectGuid::Empty);
    bot->SetSelection(ObjectGuid());
    botAI->ChangeEngine(BOT_STATE_NON_COMBAT);
    if (bot->getClass() == CLASS_HUNTER) // Check for Hunter Class
    {
        Spell const* spell = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL); // Get the current spell being cast by the bot
        if (spell && spell->m_spellInfo->Id == 75) //Check spell is not nullptr before accessing m_spellInfo
            bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL); // Interrupt Auto Shot
    }
    bot->AttackStop();

    // if (Pet* pet = bot->GetPet())
    // {
    //     if (CreatureAI* creatureAI = ((Creature*)pet)->AI())
    //     {
    //         pet->SetReactState(REACT_PASSIVE);
    //         pet->GetCharmInfo()->SetCommandState(COMMAND_FOLLOW);
    //         pet->GetCharmInfo()->SetIsCommandFollow(true);
    //         pet->AttackStop();
    //         pet->GetCharmInfo()->IsReturning();
    //         pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    //     }
    // }

    return true;
}

bool TargetRequesterAction::Execute(Event event)
{
    Player* requester = event.getOwner();

    if (!requester)
        return false;

    bot->SetSelection(requester->GetGUID());

    botAI->TellMasterNoFacing(
        PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "target_requester_success",
            "Targeting %target",
            {{"%target", requester->GetName()}}));

    return true;
}

bool AttackAnythingAction::Execute(Event event)
{
    bool result = AttackAction::Execute(event);
    if (result)
    {
        if (Unit* grindTarget = GetTarget())
        {
            context->GetValue<ObjectGuid>("pull target")->Set(grindTarget->GetGUID());
            bot->GetMotionMaster()->Clear();
            // bot->StopMoving();
        }
    }

    return result;
}

bool AttackAnythingAction::isUseful()
{
    if (!bot || !botAI)  // Prevents invalid accesses
        return false;

    if (!botAI->AllowActivity(GRIND_ACTIVITY))  // Bot cannot be active
        return false;

    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    if (bot->IsInCombat())
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsInWorld())  // Checks if the target is valid and in the world
        return false;

    std::string const name = std::string(target->GetName());
    if (!name.empty() &&
        (name.find("Dummy") != std::string::npos ||
         name.find("Charge Target") != std::string::npos ||
         name.find("Melee Target") != std::string::npos ||
         name.find("Ranged Target") != std::string::npos))
    {
        return false;
    }

    return true;
}

bool AttackAnythingAction::isPossible() { return GetTarget() && AttackAction::isPossible(); }

bool DpsAssistAction::isUseful()
{
    if (PlayerHasFlag::IsCapturingFlag(bot))
        return false;

    return true;
}

bool AttackRtiTargetAction::Execute(Event /*event*/)
{
    Unit* rtiTarget = AI_VALUE(Unit*, "rti target");

    // Fallback: if the "rti target" value did not resolve a valid unit yet,
    // try to resolve the raid icon directly from the group.
    if (!rtiTarget)
    {
        if (Group* group = bot->GetGroup())
        {
            std::string const rti = AI_VALUE(std::string, "rti");
            int32 const index = RtiTargetValue::GetRtiIndex(rti);
            if (index >= 0)
            {
                ObjectGuid const guid = group->GetTargetIcon(index);
                if (!guid.IsEmpty())
                    rtiTarget = botAI->GetUnit(guid);
            }
        }
    }

    if (rtiTarget && rtiTarget->IsInWorld() && rtiTarget->GetMapId() == bot->GetMapId())
    {
        botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({rtiTarget->GetGUID()});
        bool result = Attack(botAI->GetUnit(rtiTarget->GetGUID()));
        if (result)
        {
            context->GetValue<ObjectGuid>("pull target")->Set(rtiTarget->GetGUID());
            return true;
        }
    }
    else
        botAI->TellError("I dont see my rti attack target");

    return false;
}

bool AttackRtiTargetAction::isUseful()
{
    if (botAI->ContainsStrategy(STRATEGY_TYPE_HEAL))
        return false;

    return true;
}

Unit* BattlegroundFocusAction::GetTarget()
{
    return botAI->GetUnit(AI_VALUE(ObjectGuid, "bg focus target"));
}

bool BattlegroundFocusAction::isUseful()
{
    if (!sPlayerbotAIConfig.CoordinatedBattlegrounds || !bot->InBattleground() || bot->InArena() ||
        bot->GetVehicle() || bot->IsNonMeleeSpellCast(false) ||
        PlayerHasFlag::IsCapturingFlag(bot) || PlayerbotAI::IsHeal(bot))
        return false;
    Unit* target = GetTarget();
    return target && target->IsAlive() && target != AI_VALUE(Unit*, "current target") &&
        bot->IsValidAttackTarget(target) && !target->HasBreakableByDamageCrowdControlAura();
}

bool BattlegroundControlAction::isUseful()
{
    if (!sPlayerbotAIConfig.CoordinatedBattlegrounds || !bot->InBattleground() || bot->InArena() ||
        bot->GetVehicle() || PlayerHasFlag::IsCapturingFlag(bot))
        return false;
    if (PlayerbotAI::IsHeal(bot))
        if (Unit* patient = AI_VALUE(Unit*, "party member to heal"))
            if (patient->GetHealthPct() < 40.0f)
                return false;
    return !AI_VALUE(ObjectGuid, "bg control target").IsEmpty();
}

bool BattlegroundControlAction::Execute(Event /*event*/)
{
    Unit* target = botAI->GetUnit(AI_VALUE(ObjectGuid, "bg control target"));
    if (!target || !target->IsAlive() || !bot->IsValidAttackTarget(target) ||
        !bot->IsWithinLOSInMap(target) || target->HasUnitState(UNIT_STATE_CONTROLLED) ||
        target->HasBreakableByDamageCrowdControlAura() || bot->IsNonMeleeSpellCast(false))
        return false;
    std::vector<std::string> spells;
    switch (bot->getClass())
    {
        case CLASS_WARRIOR: spells = {"concussion blow", "intercept", "hamstring"}; break;
        case CLASS_PALADIN: spells = {"hammer of justice", "repentance"}; break;
        case CLASS_HUNTER: spells = {"scatter shot", "wyvern sting", "concussive shot"}; break;
        case CLASS_ROGUE: spells = {"kidney shot", "blind", "gouge"}; break;
        case CLASS_PRIEST: spells = {"silence", "psychic horror"}; break;
        case CLASS_DEATH_KNIGHT: spells = {"strangulate", "chains of ice"}; break;
        case CLASS_SHAMAN: spells = {"hex", "frost shock"}; break;
        case CLASS_MAGE: spells = {"deep freeze", "polymorph", "slow"}; break;
        case CLASS_WARLOCK: spells = {"shadowfury", "death coil", "fear"}; break;
        case CLASS_DRUID: spells = {"bash", "cyclone", "entangling roots"}; break;
        default: return false;
    }
    bool focused = target->GetGUID() == AI_VALUE(ObjectGuid, "bg focus target");
    for (std::string const& name : spells)
    {
        uint32 id = AI_VALUE2(uint32, "spell id", name);
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(id);
        if (!id || !spell || !bot->HasSpell(id) || bot->HasSpellCooldown(id) || target->HasAura(id) ||
            !bot->IsWithinDistInMap(target, spell->GetMaxRange(false, bot)))
            continue;
        bool breakable = name == "repentance" || name == "scatter shot" || name == "wyvern sting" ||
            name == "blind" || name == "gouge" || name == "hex" || name == "polymorph" || name == "fear" ||
            name == "entangling roots";
        if ((breakable && (focused || target->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE))) ||
            (name == "cyclone" && focused))
            continue;
        DiminishingGroup group = GetDiminishingReturnsGroupForSpell(spell, false);
        if (group != DIMINISHING_NONE && target->GetDiminishing(group) >= DIMINISHING_LEVEL_IMMUNE)
            continue;
        if (botAI->CanCastSpell(id, target) && botAI->CastSpell(id, target))
            return true;
    }
    return false;
}
