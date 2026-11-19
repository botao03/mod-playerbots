/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StrandOfTheAncientsActions.h"
#include "AreaDefines.h"
#include "BattleGroundTactics.h"
#include "BattlegroundSA.h"
#include "Creature.h"
#include "GameObject.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Vehicle.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>

namespace
{
constexpr uint32 ITEM_MASSIVE_SEAFORIUM_CHARGE = 39213;
constexpr uint32 SPELL_PLACE_SEAFORIUM_CHARGE = 52410;
constexpr uint32 SPELL_OPEN_TITAN_RELIC = 6247;
constexpr float STRAND_OBJECT_APPROACH_OFFSET = 3.5f;
constexpr float STRAND_OBJECT_APPROACH_RANGE = 6.0f;
constexpr float STRAND_OBJECT_FLOOR_TOLERANCE = 2.5f;
constexpr uint32 STRAND_CAPTURE_GROUPS = 3;
constexpr float STRAND_DEFENDER_SHORE_LIMIT_X = 1550.0f;

BattlegroundSA* GetStrand(Player* player)
{
    Battleground* battleground = player->GetBattleground();
    return battleground ? battleground->ToBattlegroundSA() : nullptr;
}

bool IsStrandAttacker(Player* player)
{
    BattlegroundSA* strand = GetStrand(player);
    return strand && sPlayerbotAIConfig.CoordinatedBattlegrounds && strand->IsRoundInProgress() &&
        strand->GetAttackerTeam() == player->GetTeamId() && player->IsAlive() &&
        !player->IsBeingTeleported() && !player->GetTransport();
}
}

bool IsStrandDockTarget(Player& bot, Unit const& target)
{
    if (bot.GetMapId() != MAP_STRAND_OF_THE_ANCIENTS || !sPlayerbotAIConfig.CoordinatedBattlegrounds ||
        bot.GetVehicle() || target.GetEntry() == NPC_DEMOLISHER_SA ||
        (target.GetPositionX() <= STRAND_DEFENDER_SHORE_LIMIT_X && !target.GetTransport()))
        return false;

    BattlegroundSA const* strand = GetStrand(&bot);
    return strand && strand->GetAttackerTeam() != bot.GetTeamId();
}

uint32 GetStrandGate(BattlegroundSA const* battleground, bool right)
{
    if (battleground->GetGateState(BG_SA_ANCIENT_GATE) == BG_SA_GATE_DESTROYED)
        return BG_SA_TITAN_RELIC;
    if (battleground->GetGateState(BG_SA_YELLOW_GATE) == BG_SA_GATE_DESTROYED)
        return BG_SA_ANCIENT_GATE;
    bool redOpen = battleground->GetGateState(BG_SA_RED_GATE) == BG_SA_GATE_DESTROYED;
    bool purpleOpen = battleground->GetGateState(BG_SA_PURPLE_GATE) == BG_SA_GATE_DESTROYED;
    if (redOpen || purpleOpen)
        return BG_SA_YELLOW_GATE;
    bool greenOpen = battleground->GetGateState(BG_SA_GREEN_GATE) == BG_SA_GATE_DESTROYED;
    bool blueOpen = battleground->GetGateState(BG_SA_BLUE_GATE) == BG_SA_GATE_DESTROYED;
    if (greenOpen || blueOpen)
        return blueOpen && (!greenOpen || right) ? BG_SA_RED_GATE : BG_SA_PURPLE_GATE;
    return right ? BG_SA_BLUE_GATE : BG_SA_GREEN_GATE;
}

Position GetStrandGateApproach(Player* player, GameObject const* gate, float distance, float lateral)
{
    float x = gate->GetPositionX() + distance;
    float y = gate->GetPositionY() + lateral;
    float z = gate->GetPositionZ() + 10.0f;
    player->UpdateAllowedPositionZ(x, y, z);
    return Position(x, y, z, gate->GetOrientation());
}

bool StrandCaptureAction::isUseful()
{
    if (!IsStrandAttacker(bot) || bot->HasUnitState(UNIT_STATE_LOST_CONTROL))
        return false;

    BattlegroundSA* strand = GetStrand(bot);
    if (strand->CanCaptureObject(BG_SA_TITAN_RELIC))
        return true;

    return !bot->GetVehicle() && strand->CanCaptureObject(BG_SA_RIGHT_FLAG);
}

bool StrandCaptureAction::Execute(Event /*event*/)
{
    BattlegroundSA* strand = GetStrand(bot);
    if (!strand)
        return false;

    for (CurrentSpellTypes type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
        if (Spell* spell = bot->GetCurrentSpell(type))
            return spell->GetSpellInfo()->Id == SPELL_CAPTURE_BANNER ||
                spell->GetSpellInfo()->Id == SPELL_OPEN_TITAN_RELIC;

    if (strand->CanCaptureObject(BG_SA_TITAN_RELIC))
    {
        if (bot->GetVehicle())
            return botAI->DoSpecificAction("leave vehicle");

        return Capture(strand->GetBGObject(BG_SA_TITAN_RELIC), SPELL_OPEN_TITAN_RELIC, true);
    }

    bool canApproach = bot->GetGUID().GetRawValue() % STRAND_CAPTURE_GROUPS == 0 ||
        (!bot->IsInCombat() && !AI_VALUE(ObjectGuid, "bg focus target"));
    std::array<GameObject*, BG_SA_FLAG_AMOUNT> flags{};
    auto end = flags.begin();
    for (auto const& [index, graveyard] :
        {std::pair{BG_SA_LEFT_FLAG, BG_SA_LEFT_CAPTURABLE_GY},
            std::pair{BG_SA_RIGHT_FLAG, BG_SA_RIGHT_CAPTURABLE_GY},
            std::pair{BG_SA_CENTRAL_FLAG, BG_SA_CENTRAL_CAPTURABLE_GY}})
    {
        if (strand->GetGraveyardOwner(graveyard) == bot->GetTeamId() || !strand->CanCaptureObject(index))
            continue;

        if (GameObject* flag = strand->GetBGObject(index))
            if (canApproach ||
                bot->GetExactDist2d(flag) <= STRAND_OBJECT_APPROACH_OFFSET + STRAND_OBJECT_APPROACH_RANGE)
                *end++ = flag;
    }

    std::sort(flags.begin(), end, [this](GameObject const* first, GameObject const* second)
    {
        return bot->GetDistanceOrder(first, second);
    });

    for (auto flag = flags.begin(); flag != end; ++flag)
        if (Capture(*flag, SPELL_CAPTURE_BANNER, canApproach))
            return true;

    return false;
}

bool StrandCaptureAction::Capture(GameObject* object, uint32 spellId, bool canApproach)
{
    if (!object || !object->isSpawned() || !bot->InSamePhase(object) ||
        object->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE | GO_FLAG_IN_USE))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    // The relic is suspended above the vault floor, and each banner shares its origin with its pole.
    float x = object->GetPositionX() + STRAND_OBJECT_APPROACH_OFFSET;
    float y = object->GetPositionY();
    float z = bot->GetMap()->GetHeight(x, y, object->GetPositionZ() + 1.0f);
    if (z <= INVALID_HEIGHT)
        return false;

    if (!object->IsAtInteractDistance(bot, spellInfo) ||
        bot->GetExactDist2d(x, y) > STRAND_OBJECT_APPROACH_RANGE ||
        std::abs(bot->GetPositionZ() - z) > STRAND_OBJECT_FLOOR_TOLERANCE ||
        !bot->IsWithinLOS(x, y, z, VMAP::ModelIgnoreFlags::M2))
    {
        if (!canApproach)
            return false;

        if (bot->isMoving() && IsDuplicateMove(x, y, z))
            return true;

        PathGenerator path(bot);
        if (!path.CalculatePath(x, y, z) || !(path.GetPathType() & PATHFIND_NORMAL) ||
            (path.GetPathType() & (PATHFIND_INCOMPLETE | PATHFIND_NOPATH | PATHFIND_SHORT)) ||
            (path.GetActualEndPosition() - G3D::Vector3(x, y, z)).squaredLength() >
                STRAND_OBJECT_FLOOR_TOLERANCE * STRAND_OBJECT_FLOOR_TOLERANCE)
            return false;

        return MoveTo(bot->GetMapId(), x, y, z, false, false, true, true, MovementPriority::MOVEMENT_FORCED);
    }

    if (!bot->CanUseBattlegroundObject(object) ||
        bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE)
        return false;

    bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    if (bot->IsInDisallowedMountForm())
        bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

    bot->GetMotionMaster()->Clear(false);
    bot->StopMoving();
    Spell* spell = new Spell(bot, spellInfo, TRIGGERED_NONE);
    spell->m_targets.SetGOTarget(object);
    SpellCastResult result = spell->prepare(&spell->m_targets);
    if (result != SPELL_CAST_OK)
    {
        LOG_DEBUG("playerbots", "Strand objective {} cast {} failed with {} for {}",
            object->GetEntry(), spellId, uint32(result), bot->GetGUID().ToString());
        return false;
    }

    botAI->WaitForSpellCast(spell);
    return true;
}

bool StrandPrepareAction::isUseful()
{
    BattlegroundSA* strand = GetStrand(bot);
    return strand && sPlayerbotAIConfig.CoordinatedBattlegrounds && strand->IsRoundPreparation() &&
        strand->GetAttackerTeam() != bot->GetTeamId() && bot->IsAlive() && !bot->IsInCombat() &&
        !bot->IsBeingTeleported() && !bot->GetVehicle() && !IsSelfBot(bot);
}

bool StrandPrepareAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;
    BattlegroundSA* strand = GetStrand(bot);
    bool right = bot->GetGUID().GetRawValue() % 2;
    GameObject* gate = strand->GetBGObject(GetStrandGate(strand, right));
    if (!gate || bot->IsWithinDistInMap(gate, 60.0f))
        return false;
    float lateral = (int32(bot->GetGUID().GetRawValue() % 5) - 2) * 4.0f;
    Position position = GetStrandGateApproach(bot, gate, 18.0f, lateral);
    // Deploy during preparation, before the intact gates can strand defenders inside the keep.
    return bot->TeleportTo(bot->GetMapId(), position.GetPositionX(), position.GetPositionY(),
        position.GetPositionZ(), position.GetOrientation());
}

bool StrandVehicleAction::isUseful()
{
    return IsStrandAttacker(bot) && !bot->GetVehicle() && !PlayerbotAI::IsHeal(bot) &&
        !bot->HasUnitState(UNIT_STATE_LOST_CONTROL) && !bot->IsNonMeleeSpellCast(false) &&
        !GetStrand(bot)->CanCaptureObject(BG_SA_TITAN_RELIC);
}

bool StrandVehicleAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;

    constexpr float DEMOLISHER_SEARCH_RANGE = 200.0f;
    BattlegroundSA* strand = GetStrand(bot);
    std::array<Creature*, BG_SA_DEMOLISHER_8 - BG_SA_DEMOLISHER_1 + 1> available{};
    auto end = available.begin();
    for (uint32 index = BG_SA_DEMOLISHER_1; index <= BG_SA_DEMOLISHER_8; ++index)
        if (Creature* demolisher = strand->GetBgMap()->GetCreature(strand->BgCreatures[index]))
        {
            Vehicle* vehicle = demolisher->GetVehicleKit();
            if (!demolisher->IsAlive() || !bot->IsWithinDistInMap(demolisher, DEMOLISHER_SEARCH_RANGE) ||
                !demolisher->IsFriendlyTo(bot) || !demolisher->HasNpcFlag(UNIT_NPC_FLAG_SPELLCLICK) ||
                demolisher->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE) ||
                !vehicle || !vehicle->GetAvailableSeatCount() || vehicle->IsVehicleInUse())
                continue;

            *end++ = demolisher;
        }

    std::sort(available.begin(), end,
        [this](Creature const* first, Creature const* second) { return bot->GetDistanceOrder(first, second); });

    for (auto candidate = available.begin(); candidate != end; ++candidate)
    {
        Creature* demolisher = *candidate;
        if (bot->IsWithinDistInMap(demolisher, INTERACTION_DISTANCE) && bot->IsWithinLOSInMap(demolisher))
        {
            if (EnterVehicle(demolisher, false))
                return true;
            continue;
        }

        float x = demolisher->GetPositionX();
        float y = demolisher->GetPositionY();
        float z = demolisher->GetPositionZ();
        float destinationX, destinationY, destinationZ;
        if (bot->isMoving() && bot->GetMotionMaster()->GetDestination(destinationX, destinationY, destinationZ) &&
            demolisher->GetExactDist(destinationX, destinationY, destinationZ) <= INTERACTION_DISTANCE)
            return true;

        if (MoveTo(bot->GetMapId(), x, y, z, false, false, true, false, MovementPriority::MOVEMENT_COMBAT))
        {
            bot->AttackStop();
            return true;
        }
    }
    return false;
}

bool StrandDefenseAction::isUseful()
{
    BattlegroundSA* strand = GetStrand(bot);
    return strand && sPlayerbotAIConfig.CoordinatedBattlegrounds && strand->IsRoundInProgress() &&
        strand->GetAttackerTeam() != bot->GetTeamId() && bot->IsAlive() && !bot->IsBeingTeleported() &&
        !bot->GetTransport() && bot->GetSession()->IsHeadless() && !bot->IsMovementPreventedByCasting() &&
        strand->GetGateState(BG_SA_YELLOW_GATE) == BG_SA_GATE_DESTROYED;
}

bool StrandDefenseAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;

    BattlegroundSA* strand = GetStrand(bot);
    GameObject* gate = strand->GetBGObject(BG_SA_YELLOW_GATE);
    if (!gate)
        return false;

    // Include dead defenders so casualties and respawns do not reassign the survivors.
    uint32 count = 0;
    uint32 slot = 0;
    for (auto const& [guid, player] : strand->GetPlayers())
        if (player->GetTeamId() == bot->GetTeamId() && player->GetSession()->IsHeadless())
        {
            ++count;
            if (guid < bot->GetGUID())
                ++slot;
        }

    constexpr uint32 FORMATION_DIVISIONS = 10;
    constexpr uint32 GATE_DIVISIONS = 8;
    constexpr float HOLD_RADIUS = 12.0f;
    uint32 gateCount = (count * GATE_DIVISIONS + FORMATION_DIVISIONS / 2) / FORMATION_DIVISIONS;
    uint32 rearCount = (count - gateCount + 1) / 2;

    PositionInfo& objective = AI_VALUE(PositionMap&, "position")["bg objective"];
    Position post;
    if (slot < gateCount)
    {
        constexpr uint32 ROW_WIDTH = 4;
        float depth = -8.0f - 5.0f * (slot / ROW_WIDTH);
        float lateral = 5.0f * (slot % ROW_WIDTH) - 7.5f;
        post = GetStrandGateApproach(bot, gate, depth, lateral);
    }
    else if (slot < gateCount + rearCount)
    {
        static Position const courtyard(987.0f, -82.0f, 92.08f);
        static Position const vaultEntrance(922.0f, -85.0f, 98.3f);
        post = (slot - gateCount) % 2 == 0 ? courtyard : vaultEntrance;
        if (rearCount == 1)
        {
            bool atVaultPost = objective.isSet() && objective.x == vaultEntrance.GetPositionX();
            post = atVaultPost ? vaultEntrance : courtyard;
            if (bot->GetExactDist(post) <= HOLD_RADIUS && !AI_VALUE(ObjectGuid, "bg focus target"))
                post = atVaultPost ? courtyard : vaultEntrance;
        }
    }
    else
    {
        float lateral = slot % 2 == 0 ? -7.0f : 7.0f;
        post = GetStrandGateApproach(bot, gate, 25.0f, lateral);
    }

    objective.Set(post.GetPositionX(), post.GetPositionY(), post.GetPositionZ(), bot->GetMapId());
    if (bot->GetExactDist(post) <= HOLD_RADIUS)
        return false;

    if (bot->GetVehicle())
        return botAI->DoSpecificAction("leave vehicle");

    float x, y, z;
    if (bot->isMoving() && bot->GetMotionMaster()->GetDestination(x, y, z) &&
        post.GetExactDist(x, y, z) <= HOLD_RADIUS)
        return true;

    if (!MoveTo(bot->GetMapId(), post.GetPositionX(), post.GetPositionY(), post.GetPositionZ(),
        false, false, true, false, MovementPriority::MOVEMENT_FORCED))
        return false;

    bot->AttackStop();
    return true;
}

bool StrandBombAction::isUseful()
{
    return IsStrandAttacker(bot) && !bot->GetVehicle() && !PlayerbotAI::IsHeal(bot) &&
        !bot->IsInCombat() && !bot->IsNonMeleeSpellCast(false) && !AI_VALUE(ObjectGuid, "bg focus target");
}

bool StrandBombAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;
    BattlegroundSA* strand = GetStrand(bot);
    uint32 gateIndex = GetStrandGate(strand, bot->GetGUID().GetRawValue() % 2);
    if (gateIndex == BG_SA_TITAN_RELIC)
        return false;
    if (Item* charge = bot->GetItemByEntry(ITEM_MASSIVE_SEAFORIUM_CHARGE))
    {
        GameObject* gate = strand->GetBGObject(gateIndex);
        if (!gate)
            return false;
        Position position = GetStrandGateApproach(bot, gate, 5.0f);
        if (bot->GetExactDist2d(position) > 2.0f)
            return MoveTo(bot->GetMapId(), position.GetPositionX(), position.GetPositionY(), position.GetPositionZ());
        if (!botAI->CanCastSpell(SPELL_PLACE_SEAFORIUM_CHARGE, bot, false, nullptr, charge))
            return false;
        bot->StopMoving();
        SpellCastTargets targets;
        targets.SetUnitTarget(bot);
        WorldPacket packet(CMSG_USE_ITEM);
        packet << charge->GetBagSlot() << charge->GetSlot() << uint8(1) << SPELL_PLACE_SEAFORIUM_CHARGE
            << charge->GetGUID() << uint32(0) << uint8(0);
        targets.Write(packet);
        bot->GetSession()->HandleUseItemOpcode(packet);
        SetDuration(sPlayerbotAIConfig.GlobalCoolDown);
        return true;
    }
    GameObject* selected = nullptr;
    float distance = 60.0f;
    for (uint32 index = BG_SA_BOMB; index < BG_SA_MAXOBJ; ++index)
        if (GameObject* bomb = strand->GetBgMap()->GetGameObject(strand->BgObjects[index]))
            if (bomb->isSpawned() && !bomb->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
            {
                float current = bot->GetExactDist(bomb);
                if (current < distance)
                {
                    selected = bomb;
                    distance = current;
                }
            }
    if (!selected)
        return false;
    if (!bot->IsWithinDistInMap(selected, INTERACTION_DISTANCE))
        return MoveTo(selected);
    selected->Use(bot);
    SetDuration(sPlayerbotAIConfig.GlobalCoolDown);
    return true;
}
