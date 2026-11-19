/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GroupTeleportFollow.h"

#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "AiObjectContext.h"
#include "BotPopulation.h"
#include "BotPopulationPolicy.h"
#include "BotRiding.h"
#include "BotRidingPolicy.h"
#include "CheckMountStateAction.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "Group.h"
#include "GroupFollowerPhasePolicy.h"
#include "LastMovementValue.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Transport.h"
#include "Vehicle.h"
#include "WorldSession.h"

namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::size_t MAX_PENDING_LEADERS = 64;
constexpr float MIN_TELEPORT_DISTANCE = 60.0f;
constexpr float ARRIVAL_DISTANCE = 10.0f;
constexpr float COMPANION_DISTANCE = 10.0f;
constexpr std::chrono::seconds REQUEST_LIFETIME{60};
constexpr std::chrono::milliseconds UPDATE_INTERVAL{500};
constexpr std::chrono::seconds RECALL_CHECK_INTERVAL{5};
constexpr std::chrono::seconds PHASE_SYNC_INTERVAL{1};
constexpr std::chrono::seconds MOUNT_SYNC_INTERVAL{1};
constexpr std::chrono::seconds RETURN_RETRY_INTERVAL{5};
constexpr float LANDING_TOLERANCE = 2.0f;
constexpr uint32 SPELL_COLD_WEATHER_FLYING = 54197;

struct FlightFollower final : DataMap::Base
{
    static constexpr char const* Key = "Playerbots.FlightFollower";
    BotFlightFollowState State;
    Clock::time_point NextReturnAttempt;
    bool Stopped = false;
};

struct FlightFollowers final : DataMap::Base
{
    static constexpr char const* Key = "Playerbots.FlightFollowers";
    Clock::time_point NextUpdate;
    std::set<ObjectGuid> Bots;
};

void ClearFlightFollower(ObjectGuid master, ObjectGuid guid)
{
    if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid))
        if (auto* follower = bot->CustomData.Get<FlightFollower>(FlightFollower::Key))
            if (follower->State.Master == master.GetRawValue())
                bot->CustomData.Erase(FlightFollower::Key);
}

bool IsHumanMaster(Player* master)
{
    return master && master->GetSession() && !master->GetSession()->IsHeadless() &&
           !master->GetSession()->IsSocketClosed() && !master->GetSession()->IsLoggingOut() && master->GetGroup();
}

bool IsFlightCompanion(Player* master, Player* bot)
{
    if (!bot || IsAcceptedDuel(bot) || bot == master || !bot->GetSession() || !bot->GetSession()->IsHeadless() ||
        bot->GetSession()->IsLoggingOut() || bot->GetGroup() != master->GetGroup() || bot->InBattleground() ||
        IsPopulationBotStaged(bot))
        return false;
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    return ai && ai->IsBotAI() && ai->GetMaster() == master && bot->IsFriendlyTo(master);
}

bool CanFollowFlyingMaster(Player* master, Player* bot, bool atMaster = false)
{
    if (!HasBotFlightRiding(bot))
        return false;
    if (GetVirtualMapForMapAndZone(master->GetMapId(), master->GetZoneId()) == MAP_NORTHREND &&
        !bot->HasSpell(SPELL_COLD_WEATHER_FLYING))
        return false;
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(atMaster ? master->GetAreaId() : bot->GetAreaId());
    return area && area->IsFlyable() && !(area->flags & AREA_FLAG_NO_FLY_ZONE);
}

bool CanReturnToMaster(Player* master)
{
    if (!master->IsAlive() || master->IsInCombat() || master->IsInFlight() || master->IsFalling() ||
        master->GetVehicle() || master->GetTransport() || master->GetMap()->IsBattlegroundOrArena())
        return false;
    float const floor = master->GetFloorZ();
    return floor != INVALID_HEIGHT && floor != VMAP_INVALID_HEIGHT_VALUE &&
           std::abs(master->GetPositionZ() - floor) <= LANDING_TOLERANCE;
}

struct FollowerPhase final : DataMap::Base
{
    static constexpr char const* Key = "Playerbots.FollowerPhase";
    GroupFollowerPhaseState State;
};

struct LeaderPhases final : DataMap::Base
{
    static constexpr char const* Key = "Playerbots.LeaderPhases";
    Clock::time_point NextUpdate;
    std::set<ObjectGuid> Bots;
};

struct RecallTracker final : DataMap::Base
{
    static constexpr char const* Key = "Playerbots.GroupRecallTracker";
    Clock::time_point NextUpdate;
};

struct RecallArrival final : DataMap::Base
{
    static constexpr char const* Key = "Playerbots.GroupRecallArrival";
    ObjectGuid Master;
    ObjectGuid Group;
    WorldLocation Origin;
    Clock::time_point Expires;
    Clock::time_point NextAttempt;
    bool Arriving = false;
};

bool IsRecallCompanion(Player* master, Player* bot)
{
    if (!bot || IsAcceptedDuel(bot) || !bot->GetSession() || !bot->GetSession()->IsHeadless() ||
        bot->GetSession()->IsLoggingOut() || bot->GetGroup() != master->GetGroup() || IsSelfBot(bot) ||
        IsPopulationBotStaged(bot) || bot->InBattleground() || !bot->IsFriendlyTo(master))
        return false;
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    return ai && ai->IsBotAI();
}

Vehicle* RecallVehicle(Player* master)
{
    return master->GetVehicle() ? master->GetVehicle() : master->GetVehicleKit();
}

int8 RecallPassengerSeat(Vehicle* vehicle)
{
    if (vehicle)
        for (auto const& [id, seat] : vehicle->Seats)
            if (seat.IsEmpty() && seat.SeatInfo->CanEnterOrExit() && !seat.SeatInfo->CanControl())
                return id;
    return -1;
}

bool FinishRecall(Player* master, Player* bot, RecallArrival& arrival)
{
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->GetMap() != master->GetMap())
        return false;
    SynchronizeGroupFollowerPhase(master, bot);
    if (Transport* transport = master->GetTransport())
    {
        if (bot->GetTransport() != transport)
        {
            if (Transport* previous = bot->GetTransport())
                previous->RemovePassenger(bot, true);
            transport->AddPassenger(bot, true);
        }
    }
    Vehicle* vehicle = RecallVehicle(master);
    if (vehicle && bot->GetVehicle() != vehicle)
    {
        int8 const seat = RecallPassengerSeat(vehicle);
        if (seat >= 0)
        {
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            Unit* base = vehicle->GetBase();
            if (base->IsPlayer())
                bot->EnterVehicle(base, seat);
            else
                base->HandleSpellClick(bot, seat);
        }
    }
    bool const mounted = CheckMountStateAction::MatchRecallMount(bot, master);
    if (mounted && IsUsingFlyingMount(bot) && master->IsFlying())
    {
        bot->AddUnitMovementFlag(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY);
        bot->SendMovementFlagUpdate();
    }
    if (!bot->GetVehicle() && !bot->GetTransport() && !IsUsingFlyingMount(bot))
    {
        float const ground = bot->GetMap()->GetHeight(bot->GetPhaseMask(), bot->GetPositionX(),
            bot->GetPositionY(), bot->GetPositionZ(), true, MAX_HEIGHT);
        if (ground == INVALID_HEIGHT || ground == VMAP_INVALID_HEIGHT_VALUE)
        {
            // A full or restricted airborne vehicle must not leave a passenger over the void.
            bot->TeleportTo(arrival.Origin);
            GET_PLAYERBOT_AI(bot)->HandleTeleportAck();
            arrival.Arriving = false;
            arrival.NextAttempt = Clock::now() + RETURN_RETRY_INTERVAL;
            return false;
        }
        if (bot->GetPositionZ() > ground + LANDING_TOLERANCE && !master->IsInWater() &&
            (master->IsFlying() || master->GetVehicle() || master->IsFalling()))
        {
            bot->TeleportTo(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), ground, bot->GetOrientation());
            GET_PLAYERBOT_AI(bot)->HandleTeleportAck();
        }
    }
    return mounted || bot->GetVehicle() || vehicle || !CanFollowFlyingMaster(master, bot, true);
}

void RecallCompanion(Player* master, Player* bot, RecallArrival& arrival)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    InitializeBotRiding(bot);
    float x = master->GetPositionX();
    float y = master->GetPositionY();
    float z = master->GetPositionZ();
    bool const hasSeat = RecallPassengerSeat(RecallVehicle(master)) >= 0;
    bool const canFly = CanFollowFlyingMaster(master, bot, true);
    bool const airborne = master->IsFlying() || master->GetVehicle() || master->IsFalling();
    if (!master->GetTransport() && airborne && !hasSeat && !(IsUsingFlyingMount(master) && canFly))
    {
        z = master->GetMap()->GetHeight(master->GetPhaseMask(), x, y, z, true, MAX_HEIGHT);
        if (z == INVALID_HEIGHT || z == VMAP_INVALID_HEIGHT_VALUE)
            return;
    }
    else if (!airborne && !hasSeat && !master->GetTransport())
        master->GetClosePoint(x, y, z, bot->GetCombatReach(), sPlayerbotAIConfig.FollowDistance);

    bot->CustomData.Erase(FlightFollower::Key);
    ai->Reset();
    bot->GetMotionMaster()->Clear();
    bot->StopMovingOnCurrentPos();
    ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().clear();
    if (!bot->TeleportTo(master->GetMapId(), x, y, z, master->GetOrientation()))
        return;
    ai->SetMaster(master);
    ai->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
    arrival.Arriving = true;
    ai->HandleTeleportAck();
    LOG_DEBUG("playerbots", "Out-of-sight companion {} recalled to {} on map {}",
              bot->GetName(), master->GetName(), master->GetMapId());
}

LeaderPhases& PhaseTracker(Player* leader)
{
    auto* tracker = leader->CustomData.Get<LeaderPhases>(LeaderPhases::Key);
    if (!tracker)
    {
        tracker = new LeaderPhases();
        leader->CustomData.Set(LeaderPhases::Key, tracker);
    }
    return *tracker;
}

void RestoreFollowerPhase(ObjectGuid leader, ObjectGuid guid)
{
    Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
    if (!bot)
        return;
    auto* phase = bot->CustomData.Get<FollowerPhase>(FollowerPhase::Key);
    if (!phase || phase->State.Leader != leader.GetRawValue())
        return;
    if (bot->GetSession() && bot->GetSession()->IsHeadless())
        if (auto original = phase->State.OriginalFor(leader.GetRawValue(), bot->GetPhaseMask()))
            bot->SetPhaseMask(*original, true);
    bot->CustomData.Erase(FollowerPhase::Key);
}

struct GroupTeleport
{
    ObjectGuid GroupGuid;
    WorldLocation Destination;
    std::vector<ObjectGuid> Members;
    Clock::time_point Created;
    bool Arrived = false;
    bool SkipOutdoorBots = false;
};

std::mutex incomingMutex;
std::map<ObjectGuid, GroupTeleport> incoming;

bool IsHumanLeader(Player* leader)
{
    return leader && leader->GetSession() && !leader->GetSession()->IsHeadless() &&
           !leader->GetSession()->IsSocketClosed() && !leader->GetSession()->IsLoggingOut() && leader->GetGroup() &&
           leader->GetGroup()->IsLeader(leader->GetGUID());
}

uint32 GetGroupFollowerPhase(Player const* leader)
{
    uint32 phase = leader->IsGameMaster() ? leader->GetPhaseByAuras() : leader->GetPhaseMask();
    return (sPlayerbotAIConfig.FollowLeaderTeleport || sPlayerbotAIConfig.FollowGroupOutOfSight) && phase &&
                   !(phase & BOT_POPULATION_STAGING_PHASE_MASK)
               ? phase
               : PHASEMASK_NORMAL;
}

bool FollowLeader(Player* leader, Player* bot, bool retryRejected = false)
{
    if (IsAcceptedDuel(bot))
        return true;

    if (!retryRejected && IsWaitingForFlyingMaster(bot))
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->IsBotAI() || bot->GetGroup() != leader->GetGroup() || !bot->GetSession() ||
        !bot->GetSession()->IsHeadless() || bot->GetSession()->IsLoggingOut() || bot->InBattleground() ||
        !bot->IsFriendlyTo(leader))
        return true;
    if (bot->IsBeingTeleported() || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
        bot->GetVehicle() || IsPopulationBotStaged(bot))
        return false;
    SynchronizeGroupFollowerPhase(leader, bot);
    if (bot->GetMap() == leader->GetMap() && bot->IsWithinDistInMap(leader, COMPANION_DISTANCE))
    {
        botAI->SetMaster(leader);
        botAI->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
        return true;
    }

    float x, y, z;
    if (!leader->GetClosePoint(x, y, z, bot->GetCombatReach(), sPlayerbotAIConfig.FollowDistance))
    {
        x = leader->GetPositionX();
        y = leader->GetPositionY();
        z = leader->GetPositionZ();
    }
    bool const newInstance = bot->GetMapId() == leader->GetMapId() && bot->GetInstanceId() != leader->GetInstanceId();
    botAI->Reset();
    botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().clear();
    if (bot->TeleportTo(leader->GetMapId(), x, y, z, leader->GetOrientation(), 0, nullptr, newInstance))
    {
        botAI->SetMaster(leader);
        botAI->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
        return true;
    }
    // A rejected destination must not bypass entry restrictions or be retried every tick.
    return !retryRejected;
}
}  // namespace

bool IsWaitingForFlyingMaster(Player* bot)
{
    if (!sPlayerbotAIConfig.WaitForFlyingMaster || !bot || !bot->IsAlive() || bot->IsInCombat())
        return false;
    auto const* follower = bot->CustomData.Get<FlightFollower>(FlightFollower::Key);
    return follower && follower->State.Master;
}

void ClearGroupMountFollow(Player* master)
{
    if (!master)
        return;
    if (auto* tracker = master->CustomData.Get<FlightFollowers>(FlightFollowers::Key))
    {
        for (ObjectGuid guid : tracker->Bots)
            ClearFlightFollower(master->GetGUID(), guid);
        master->CustomData.Erase(FlightFollowers::Key);
    }
}

void UpdateGroupMountFollow(Player* master)
{
    if (!master || !master->GetSession() || master->GetSession()->IsHeadless())
        return;
    if (!sPlayerbotAIConfig.Enabled || !sPlayerbotAIConfig.WaitForFlyingMaster || !IsHumanMaster(master) ||
        !master->IsAlive() || master->InBattleground())
    {
        ClearGroupMountFollow(master);
        return;
    }
    if (!master->IsInWorld() || master->IsBeingTeleported())
        return;

    bool const flyingMount = IsUsingFlyingMount(master);
    auto* tracker = master->CustomData.Get<FlightFollowers>(FlightFollowers::Key);
    if (!tracker)
    {
        if (!flyingMount)
            return;
        tracker = new FlightFollowers();
        master->CustomData.Set(FlightFollowers::Key, tracker);
    }
    Clock::time_point const now = Clock::now();
    if (now < tracker->NextUpdate)
        return;
    tracker->NextUpdate = now + MOUNT_SYNC_INTERVAL;

    for (auto iterator = tracker->Bots.begin(); iterator != tracker->Bots.end();)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(*iterator);
        if (!IsFlightCompanion(master, bot))
        {
            ClearFlightFollower(master->GetGUID(), *iterator);
            iterator = tracker->Bots.erase(iterator);
        }
        else
            ++iterator;
    }

    for (GroupReference* member = master->GetGroup()->GetFirstMember(); member; member = member->next())
    {
        Player* bot = member->GetSource();
        if (!IsFlightCompanion(master, bot) || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsInFlight() ||
            bot->GetVehicle())
            continue;
        bool const canFly = CanFollowFlyingMaster(master, bot);
        auto* follower = bot->CustomData.Get<FlightFollower>(FlightFollower::Key);
        if (!follower)
        {
            if (!flyingMount || canFly)
                continue;
            follower = new FlightFollower();
            bot->CustomData.Set(FlightFollower::Key, follower);
            LOG_DEBUG("playerbots", "Flight companion {} level {} riding {} waits for {} to dismount",
                      bot->GetName(), bot->GetLevel(), bot->GetPureSkillValue(SKILL_RIDING), master->GetName());
        }
        BotFlightFollowAction const action = follower->State.Update(master->GetGUID().GetRawValue(),
            master->GetGroup()->GetGUID().GetRawValue(), flyingMount, IsUsingMountOrTravelForm(master), canFly);
        if (action == BotFlightFollowAction::Follow)
        {
            bot->CustomData.Erase(FlightFollower::Key);
            tracker->Bots.erase(bot->GetGUID());
            continue;
        }
        if (action == BotFlightFollowAction::Return && sPlayerbotAIConfig.FollowGroupOutOfSight &&
            !master->GetMap()->Instanceable() && !bot->GetMap()->Instanceable())
        {
            bot->CustomData.Erase(FlightFollower::Key);
            tracker->Bots.erase(bot->GetGUID());
            continue;
        }
        tracker->Bots.insert(bot->GetGUID());
        if (!bot->IsAlive() || bot->IsInCombat())
        {
            follower->Stopped = false;
            continue;
        }
        if (!follower->Stopped)
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMovingOnCurrentPos();
            GET_PLAYERBOT_AI(bot)->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().clear();
            follower->Stopped = true;
        }
        if (action != BotFlightFollowAction::Return || !CanReturnToMaster(master) || now < follower->NextReturnAttempt)
            continue;
        follower->NextReturnAttempt = now + RETURN_RETRY_INTERVAL;
        if (FollowLeader(master, bot, true))
        {
            LOG_DEBUG("playerbots", "Waiting flight companion {} returned to {} after dismount",
                      bot->GetName(), master->GetName());
            bot->CustomData.Erase(FlightFollower::Key);
            tracker->Bots.erase(bot->GetGUID());
        }
    }
}

void SynchronizeGroupFollowerPhase(Player* leader, Player* bot)
{
    if (!sPlayerbotAIConfig.Enabled ||
        (!sPlayerbotAIConfig.FollowLeaderTeleport && !sPlayerbotAIConfig.FollowGroupOutOfSight) ||
        !(sPlayerbotAIConfig.FollowGroupOutOfSight ? IsHumanMaster(leader) : IsHumanLeader(leader)) ||
        !leader->IsInWorld() || leader->IsBeingTeleported() || leader->InBattleground() ||
        leader->GetMap()->IsBattlegroundOrArena() || !bot || !bot->GetSession() || !bot->GetSession()->IsHeadless() ||
        bot->GetSession()->IsLoggingOut() || !GET_PLAYERBOT_AI(bot) || IsSelfBot(bot) || !bot->IsInWorld() ||
        bot->IsBeingTeleported() || bot->GetGroup() != leader->GetGroup() || bot->GetMap() != leader->GetMap() ||
        bot->InBattleground() || IsPopulationBotStaged(bot))
        return;

    Player* master = GET_PLAYERBOT_AI(bot)->GetMaster();
    if (sPlayerbotAIConfig.FollowGroupOutOfSight && IsHumanMaster(master) &&
        master->GetGroup() == leader->GetGroup() && master != leader)
        return;
    if (sPlayerbotAIConfig.FollowGroupOutOfSight && !IsHumanMaster(master) && !IsHumanLeader(leader))
        return;

    uint32 current = bot->GetPhaseMask();
    uint32 desired = GetGroupFollowerPhase(leader);
    auto* phase = bot->CustomData.Get<FollowerPhase>(FollowerPhase::Key);
    if (!phase && current == desired)
        return;
    if (!phase)
    {
        phase = new FollowerPhase();
        bot->CustomData.Set(FollowerPhase::Key, phase);
    }
    phase->State.Adopt(leader->GetGUID().GetRawValue(), current, desired);
    PhaseTracker(leader).Bots.insert(bot->GetGUID());
    if (current != desired)
        bot->SetPhaseMask(desired, true);
}

void RestoreGroupFollowerPhases(Player* leader)
{
    if (!leader)
        return;
    if (auto* tracker = leader->CustomData.Get<LeaderPhases>(LeaderPhases::Key))
    {
        for (ObjectGuid guid : tracker->Bots)
            RestoreFollowerPhase(leader->GetGUID(), guid);
        tracker->Bots.clear();
    }
}

void UpdateGroupFollowerPhases(Player* leader)
{
    if (!leader || !leader->GetSession() || leader->GetSession()->IsHeadless())
        return;
    if (!sPlayerbotAIConfig.Enabled ||
        (!sPlayerbotAIConfig.FollowLeaderTeleport && !sPlayerbotAIConfig.FollowGroupOutOfSight) ||
        !(sPlayerbotAIConfig.FollowGroupOutOfSight ? IsHumanMaster(leader) : IsHumanLeader(leader)) ||
        leader->InBattleground())
    {
        RestoreGroupFollowerPhases(leader);
        return;
    }
    if (!leader->IsInWorld() || leader->IsBeingTeleported())
        return;
    LeaderPhases& tracker = PhaseTracker(leader);
    Clock::time_point now = Clock::now();
    if (now < tracker.NextUpdate)
        return;
    tracker.NextUpdate = now + PHASE_SYNC_INTERVAL;
    for (auto iterator = tracker.Bots.begin(); iterator != tracker.Bots.end();)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(*iterator);
        if (!bot || bot->GetGroup() != leader->GetGroup())
        {
            RestoreFollowerPhase(leader->GetGUID(), *iterator);
            iterator = tracker.Bots.erase(iterator);
        }
        else
            ++iterator;
    }
    for (GroupReference* member = leader->GetGroup()->GetFirstMember(); member; member = member->next())
        SynchronizeGroupFollowerPhase(leader, member->GetSource());
}

void UpdateGroupOutOfSightFollow(Player* player)
{
    if (!sPlayerbotAIConfig.Enabled || !sPlayerbotAIConfig.FollowGroupOutOfSight || !IsHumanMaster(player))
        return;

    auto* tracker = player->CustomData.Get<RecallTracker>(RecallTracker::Key);
    if (!tracker)
    {
        tracker = new RecallTracker();
        player->CustomData.Set(RecallTracker::Key, tracker);
    }
    Clock::time_point const now = Clock::now();
    if (now < tracker->NextUpdate)
        return;
    tracker->NextUpdate = now + RECALL_CHECK_INTERVAL;
    if (!player->IsInWorld() || player->IsBeingTeleported() || !player->IsAlive() || player->IsInFlight() ||
        player->InBattleground() || player->GetMap()->Instanceable())
        return;

    std::vector<Player*> humans;
    for (auto const& member : player->GetGroup()->GetMemberSlots())
    {
        Player* human = ObjectAccessor::FindConnectedPlayer(member.guid);
        if (IsHumanMaster(human))
            humans.push_back(human);
    }
    if (humans.empty() || humans.front() != player)
        return;

    for (auto const& member : player->GetGroup()->GetMemberSlots())
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
        if (!IsRecallCompanion(player, bot))
            continue;
        auto* arrival = bot->CustomData.Get<RecallArrival>(RecallArrival::Key);
        if (arrival && (arrival->Master != player->GetGUID() || arrival->Group != player->GetGroup()->GetGUID() ||
                        now >= arrival->Expires))
        {
            bot->CustomData.Erase(RecallArrival::Key);
            arrival = nullptr;
        }
        if (!bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive() || bot->GetMap()->Instanceable())
            continue;
        if (GET_PLAYERBOT_AI(bot)->GetMaster() == player)
            SynchronizeGroupFollowerPhase(player, bot);
        if (arrival && arrival->Arriving)
        {
            if (FinishRecall(player, bot, *arrival))
                bot->CustomData.Erase(RecallArrival::Key);
            continue;
        }
        bool inSight = false;
        for (Player* human : humans)
        {
            if (human->IsInWorld() && !human->IsBeingTeleported() && human->GetMap() == bot->GetMap() &&
                !human->IsWorldObjectOutOfSightRange(bot))
            {
                inSight = true;
                break;
            }
        }
        if (inSight)
        {
            bot->CustomData.Erase(RecallArrival::Key);
            continue;
        }
        if (!arrival)
        {
            arrival = new RecallArrival();
            arrival->Master = player->GetGUID();
            arrival->Group = player->GetGroup()->GetGUID();
            arrival->Origin = WorldLocation(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                bot->GetPositionZ(), bot->GetOrientation());
            arrival->Expires = now + REQUEST_LIFETIME;
            bot->CustomData.Set(RecallArrival::Key, arrival);
        }
        if (now < arrival->NextAttempt)
            continue;
        arrival->NextAttempt = now + RETURN_RETRY_INTERVAL;
        RecallCompanion(player, bot, *arrival);
        if (arrival->Arriving && FinishRecall(player, bot, *arrival))
            bot->CustomData.Erase(RecallArrival::Key);
    }
}

void QueueGroupTeleportFollow(Player* leader, uint32 mapId, float x, float y, float z)
{
    if (!sPlayerbotAIConfig.Enabled || !sPlayerbotAIConfig.FollowLeaderTeleport || !IsHumanLeader(leader) ||
        leader->InBattleground() ||
        (leader->GetMapId() == mapId && leader->GetExactDist(x, y, z) < MIN_TELEPORT_DISTANCE))
        return;

    GroupTeleport request;
    request.GroupGuid = leader->GetGroup()->GetGUID();
    request.Destination = WorldLocation(mapId, x, y, z);
    request.Created = Clock::now();
    if (sPlayerbotAIConfig.FollowGroupOutOfSight)
        if (MapEntry const* destination = sMapStore.LookupEntry(mapId))
            request.SkipOutdoorBots = !destination->Instanceable();
    for (auto const& member : leader->GetGroup()->GetMemberSlots())
        if (member.guid != leader->GetGUID())
            request.Members.push_back(member.guid);
    std::lock_guard<std::mutex> lock(incomingMutex);
    if (incoming.count(leader->GetGUID()) || incoming.size() < MAX_PENDING_LEADERS)
        incoming[leader->GetGUID()] = std::move(request);
}

void UpdateGroupTeleportFollow()
{
    if (!sPlayerbotAIConfig.Enabled || !sPlayerbotAIConfig.FollowLeaderTeleport)
        return;
    static Clock::time_point nextUpdate;
    Clock::time_point const now = Clock::now();
    if (now < nextUpdate)
        return;
    nextUpdate = now + UPDATE_INTERVAL;

    static std::map<ObjectGuid, GroupTeleport> pending;
    {
        std::lock_guard<std::mutex> lock(incomingMutex);
        for (auto& [guid, request] : incoming)
            if (pending.count(guid) || pending.size() < MAX_PENDING_LEADERS)
                pending[guid] = std::move(request);
        incoming.clear();
    }
    for (auto iterator = pending.begin(); iterator != pending.end();)
    {
        Player* leader = ObjectAccessor::FindConnectedPlayer(iterator->first);
        GroupTeleport& request = iterator->second;
        if (now - request.Created > REQUEST_LIFETIME || !IsHumanLeader(leader) ||
            leader->GetGroup()->GetGUID() != request.GroupGuid || leader->InBattleground())
        {
            iterator = pending.erase(iterator);
            continue;
        }
        if (!leader->IsInWorld() || leader->IsBeingTeleported() || !leader->IsAlive() || leader->IsInFlight() ||
            leader->GetVehicle())
        {
            ++iterator;
            continue;
        }
        if (!request.Arrived)
            request.Arrived = leader->GetMapId() == request.Destination.GetMapId() &&
                              leader->GetExactDist(&request.Destination) < ARRIVAL_DISTANCE;
        if (!request.Arrived || leader->GetMap()->IsBattlegroundOrArena())
        {
            ++iterator;
            continue;
        }
        for (auto member = request.Members.begin(); member != request.Members.end();)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(*member);
            if (bot && request.SkipOutdoorBots && bot->IsInWorld() && !bot->IsBeingTeleported() &&
                !bot->GetMap()->Instanceable())
            {
                member = request.Members.erase(member);
                continue;
            }
            if (!bot || FollowLeader(leader, bot))
                member = request.Members.erase(member);
            else
                ++member;
        }
        if (request.Members.empty())
            iterator = pending.erase(iterator);
        else
            ++iterator;
    }
}
