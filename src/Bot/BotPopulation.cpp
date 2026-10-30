/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityReplacement.h"
#include "AcceptInvitationAction.h"
#include "BotPopulation.h"
#include "BotPopulationPolicy.h"
#include "RandomPlayerbotMgr.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "EmptyInstanceCleanup.h"
#include "Event.h"
#include "Group.h"
#include "GroupMgr.h"
#include "InstanceExitCleanup.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapCollisionData.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RaceMgr.h"
#include "Random.h"
#include "TravelMgr.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace
{
constexpr uint32 LOGIN_TIMEOUT = 120;
constexpr uint32 LOGINS_PER_UPDATE = 2;
constexpr uint32 WORLD_RETRY_PRIORITY = 3;
constexpr uint32 MAX_PLAYER_LEVEL = 80;
constexpr uint32 DEATH_KNIGHT_MIN_LEVEL = 55;
constexpr uint32 STAGING_PHASE_MASK = 1u << 30;
constexpr float NEARBY_SPAWN_MIN_DISTANCE = 20.0f;
constexpr float NEARBY_SPAWN_MAX_DISTANCE = 40.0f;

bool IsPopulationClient(Player const* player)
{
    return player && player->IsInWorld() && player->GetSession() &&
        !player->GetSession()->IsSocketClosed() && !player->GetSession()->IsLoggingOut() &&
        !player->IsSpectator() && !player->IsGMSpectator();
}

bool SameRequest(BotPopulationAssignment const& left, BotPopulationAssignment const& right)
{
    return std::tie(left.Purpose, left.Map, left.Zone, left.Queue, left.Bracket, left.Team,
               left.Role, left.Rated, left.Dungeons, left.Nearby) ==
        std::tie(right.Purpose, right.Map, right.Zone, right.Queue, right.Bracket, right.Team,
            right.Role, right.Rated, right.Dungeons, right.Nearby) &&
        (!left.Nearby || left.Owner == right.Owner) &&
        (left.Purpose != BotPopulationPurpose::Dungeon ||
            (left.Owner == right.Owner && left.Level == right.Level));
}

bool SupportsRole(uint8 playerClass, uint8 role)
{
    if (role == lfg::PLAYER_ROLE_TANK)
        return playerClass == CLASS_WARRIOR || playerClass == CLASS_PALADIN ||
            playerClass == CLASS_DEATH_KNIGHT || playerClass == CLASS_DRUID;
    if (role == lfg::PLAYER_ROLE_HEALER)
        return playerClass == CLASS_PRIEST || playerClass == CLASS_PALADIN ||
            playerClass == CLASS_SHAMAN || playerClass == CLASS_DRUID;
    return true;
}

bool FindWorldLocation(TeamId team, BotPopulationAssignment& assignment)
{
    Map* map = sMapMgr->FindMap(assignment.Map, 0);
    ChrRacesEntry const* race = sChrRacesStore.LookupEntry(team == TEAM_ALLIANCE ? RACE_HUMAN : RACE_ORC);
    FactionTemplateEntry const* faction = race ? sFactionTemplateStore.LookupEntry(race->FactionID) : nullptr;
    if (!map || !faction)
        return false;
    if (assignment.Nearby)
    {
        Player* owner = ObjectAccessor::FindConnectedPlayer(assignment.Owner);
        if (!IsPopulationClient(owner) || owner->IsBeingTeleported() || owner->GetMap() != map ||
            !IsPopulationPatrolZone(assignment.Zone, owner->GetZoneId()))
            return false;
        for (uint32 attempt = 0; attempt < 80; ++attempt)
        {
            float angle = frand(0.0f, 2.0f * M_PI);
            float radius = frand(NEARBY_SPAWN_MIN_DISTANCE, NEARBY_SPAWN_MAX_DISTANCE);
            float x = owner->GetPositionX() + std::cos(angle) * radius;
            float y = owner->GetPositionY() + std::sin(angle) * radius;
            float z = map->GetHeight(PHASEMASK_NORMAL, x, y, owner->GetPositionZ() + 3.0f, true, 12.0f);
            if (!std::isfinite(z) || z <= INVALID_HEIGHT || std::abs(z - owner->GetPositionZ()) > 10.0f ||
                map->IsInWater(PHASEMASK_NORMAL, x, y, z, 2.0f) ||
                !map->isInLineOfSight(owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ() + 1.0f,
                    x, y, z + 1.0f, PHASEMASK_NORMAL, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                continue;
            WorldLocation location(assignment.Map, x, y, z + 0.1f, angle);
            if (!IsPopulationPatrolZone(assignment.Zone, sMapMgr->GetZoneId(PHASEMASK_NORMAL, location)) ||
                !sTravelMgr.IsPopulationLocationSafe(location, race->FactionID))
                continue;
            assignment.Location = location;
            return true;
        }
        return false;
    }
    auto const& locations = sTravelMgr.GetPopulationLocations(assignment.Map, assignment.Zone);
    if (locations.empty())
        return false;
    bool capital = IsPopulationCapital(assignment.Zone);
    for (uint32 attempt = 0; attempt < 80; ++attempt)
    {
        auto const& [anchor, factionId] = locations[urand(0, locations.size() - 1)];
        FactionTemplateEntry const* npcFaction = sFactionTemplateStore.LookupEntry(factionId);
        if (!npcFaction || npcFaction->IsHostileTo(*faction) || (capital && !npcFaction->IsFriendlyTo(*faction)))
            continue;
        float angle = frand(0.0f, 2.0f * M_PI);
        float radius = frand(3.0f, 12.0f);
        float x = anchor.GetPositionX() + std::cos(angle) * radius;
        float y = anchor.GetPositionY() + std::sin(angle) * radius;
        float z = map->GetHeight(PHASEMASK_NORMAL, x, y, anchor.GetPositionZ() + 3.0f, true, 8.0f);
        if (!std::isfinite(z) || z <= INVALID_HEIGHT || std::abs(z - anchor.GetPositionZ()) > 3.0f ||
            map->IsInWater(PHASEMASK_NORMAL, x, y, z, 2.0f) ||
            !map->isInLineOfSight(anchor.GetPositionX(), anchor.GetPositionY(), anchor.GetPositionZ() + 1.0f,
                x, y, z + 1.0f, PHASEMASK_NORMAL, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
            continue;
        WorldLocation location(assignment.Map, x, y, z + 0.1f, angle);
        if (!IsPopulationPatrolZone(assignment.Zone, sMapMgr->GetZoneId(PHASEMASK_NORMAL, location)) ||
            !sTravelMgr.IsPopulationLocationSafe(location, race->FactionID))
            continue;
        assignment.Location = location;
        return true;
    }
    return false;
}

bool ResolveWorldAssignment(uint8 playerClass, TeamId team, BotPopulationAssignment& assignment)
{
    bool capital = IsPopulationCapital(assignment.Zone);
    auto [low, high] = sTravelMgr.GetZoneLevelRange(assignment.Zone);
    if (!high)
        low = high = assignment.Level;
    if (capital)
        std::tie(low, high) = PopulationCapitalLevelRange(assignment.Zone);
    if (playerClass == CLASS_DEATH_KNIGHT)
    {
        if (high < DEATH_KNIGHT_MIN_LEVEL)
            return false;
        low = std::max(DEATH_KNIGHT_MIN_LEVEL, low);
    }
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(assignment.Zone);
    if (area && ((area->team == 4 && team == TEAM_ALLIANCE) ||
        (area->team == 2 && team == TEAM_HORDE)))
        return false;
    assignment.Level = urand(std::max(1u, low), std::min(MAX_PLAYER_LEVEL, high));
    if (capital)
        assignment.Level = PopulationCapitalLevel(assignment.Zone, urand(0, 99), assignment.Level);
    assignment.Endgame = capital && assignment.Level == MAX_PLAYER_LEVEL && urand(0, 99) < 80;
    assignment.ArenaLoadout = assignment.Endgame && urand(0, 1);
    return true;
}

bool HasClientInInstance(Player* bot)
{
    Map* map = bot->FindMap();
    if (!map || !map->Instanceable())
        return false;
    for (auto const& reference : map->GetPlayers())
        if (IsPopulationClient(reference.GetSource()))
            return true;
    return false;
}

bool HasClientInGroup(Player const* bot)
{
    Group const* group = bot->GetGroup();
    if (!group)
        return false;
    for (auto const& member : group->GetMemberSlots())
        if (IsClientControlledInstancePlayer(ObjectAccessor::FindConnectedPlayer(member.guid)))
            return true;
    return false;
}

bool HasClientGroupInvite(Player* bot)
{
    Group const* group = bot->GetGroupInvite();
    return group && IsClientControlledInstancePlayer(ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID()));
}
}

BotPopulationState const* GetBotPopulation(Player const* bot)
{
    return bot && sPlayerbotAIConfig.PopulationEnabled
        ? bot->CustomData.Get<BotPopulationState>(BotPopulationState::Key) : nullptr;
}

bool IsPopulationTravelAllowed(Player const* bot, WorldLocation const& location)
{
    BotPopulationState const* state = GetBotPopulation(bot);
    if (!state || state->Assignment.Purpose != BotPopulationPurpose::World || HasClientInGroup(bot))
        return true;
    return state->Ready && state->Assignment.Map == location.GetMapId() &&
        IsPopulationPatrolZone(state->Assignment.Zone, sMapMgr->GetZoneId(PHASEMASK_NORMAL, location));
}

void RandomPlayerbotMgr::PreparePopulationBot(Player* bot, BotPopulationAssignment const& assignment)
{
    if (assignment.Purpose == BotPopulationPurpose::World && HasClientInGroup(bot))
        return;

    auto* state = bot->CustomData.Get<BotPopulationState>(BotPopulationState::Key);
    if (!state)
    {
        state = new BotPopulationState();
        state->Assignment = assignment;
        bot->CustomData.Set(BotPopulationState::Key, state);
    }
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai)
        return;
    ai->SetMaster(nullptr);
    bot->SetPhaseMask(STAGING_PHASE_MASK, true);
    if (bot->GetGroup())
        ai->LeaveOrDisbandGroup();
    if (!state->Prepared)
    {
        SetValue(bot, "level", assignment.Level);
        PlayerbotFactory factory(bot, assignment.Level);
        if (assignment.Purpose == BotPopulationPurpose::World && !assignment.Endgame)
            factory.Randomize(false, true);
        else
            factory.InitPopulationLoadout(assignment.Purpose == BotPopulationPurpose::World
                    ? assignment.ArenaLoadout : assignment.Purpose != BotPopulationPurpose::Dungeon,
                assignment.Purpose == BotPopulationPurpose::Arena || assignment.ArenaLoadout,
                assignment.Role, assignment.Endgame);
        bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
        ai->Reset(true);
        state->Prepared = true;
    }
    // Nearby owners may have moved since this slot was reserved.
    bot->SetPhaseMask(STAGING_PHASE_MASK, true);
    if (assignment.Purpose == BotPopulationPurpose::World &&
        !FindWorldLocation(bot->GetTeamId(), state->Assignment))
    {
        LOG_WARN("playerbots", "Population: no safe spawn on map {} in region {}, retrying later",
            assignment.Map, assignment.Zone);
        return;
    }
    state->Ready = true;
    if (!bot->TeleportTo(state->Assignment.Location))
    {
        state->Ready = false;
        LOG_WARN("playerbots", "Population: failed to stage {} on map {}",
            bot->GetGUID().ToString(), state->Assignment.Location.GetMapId());
        return;
    }
    bot->SaveToDB(false, false);
    LOG_INFO("playerbots", "Population: initialized {} purpose {} level {} zone {} queue {} role {}",
        bot->GetGUID().ToString(), static_cast<uint32>(assignment.Purpose), assignment.Level,
        assignment.Zone, assignment.Queue, assignment.Role);
}

void RandomPlayerbotMgr::UpdatePopulation()
{
    uint32 now = NowSeconds();
    if (!_populationLoaded)
    {
        for (uint32 account : rndBotTypeAccounts)
        {
            auto* statement = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_INFO_CHARS);
            statement->SetData(0, account);
            if (PreparedQueryResult result = CharacterDatabase.Query(statement))
                do
                {
                    Field* fields = result->Fetch();
                    _populationCandidates.push_back({fields[0].Get<uint32>(), fields[4].Get<uint8>(),
                        fields[3].Get<uint8>()});
                } while (result->NextRow());
        }
        std::shuffle(_populationCandidates.begin(), _populationCandidates.end(), RandomEngine::Instance());
        _populationLoaded = true;
        LOG_INFO("playerbots", "Population: {} available characters, world target {}, activity limit {}",
            _populationCandidates.size(), sPlayerbotAIConfig.PopulationWorldCount,
            sPlayerbotAIConfig.PopulationActivityLimit);
    }

    struct Request
    {
        BotPopulationAssignment Assignment;
        uint32 Count = 0;
        uint32 Reserved = 0;
        uint32 Progress = 0;
    };
    std::vector<Request> requests;
    struct Region
    {
        uint32 Players = 0;
        uint32 Level = 1;
    };
    std::map<std::pair<uint32, uint32>, Region> regions;
    std::set<ObjectGuid> onlineClients;
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (!player || !player->GetSession() || player->GetSession()->IsSocketClosed() ||
            player->GetSession()->IsLoggingOut() || player->IsSpectator() || player->IsGMSpectator())
            continue;
        onlineClients.insert(guid);
        if (player->IsInWorld() && !player->IsBeingTeleported() && !player->GetMap()->Instanceable())
        {
            uint32 map = player->GetMapId();
            if (std::find(sPlayerbotAIConfig.RandomBotMaps.begin(), sPlayerbotAIConfig.RandomBotMaps.end(), map) ==
                sPlayerbotAIConfig.RandomBotMaps.end())
            {
                _populationOutdoorPresence.erase(guid);
                continue;
            }
            _populationOutdoorPresence[guid] = {map, player->GetMap()->GetZoneId(PHASEMASK_NORMAL,
                player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()), player->GetLevel()};
        }
        else if (!_populationOutdoorPresence.contains(guid) && player->IsInWorld() &&
            player->GetMap()->Instanceable())
        {
            WorldLocation const& entry = player->GetEntryPoint();
            MapEntry const* map = sMapStore.LookupEntry(entry.GetMapId());
            if (map && !map->Instanceable() && std::find(sPlayerbotAIConfig.RandomBotMaps.begin(),
                sPlayerbotAIConfig.RandomBotMaps.end(), entry.GetMapId()) != sPlayerbotAIConfig.RandomBotMaps.end())
                _populationOutdoorPresence[guid] = {entry.GetMapId(),
                    sMapMgr->GetZoneId(PHASEMASK_NORMAL, entry), player->GetLevel()};
        }
        auto presence = _populationOutdoorPresence.find(guid);
        if (presence == _populationOutdoorPresence.end())
            continue;
        auto const& outdoor = presence->second;
        auto& region = regions[{outdoor.Map, outdoor.Zone}];
        ++region.Players;
        region.Level = outdoor.Level;
    }
    std::erase_if(_populationOutdoorPresence, [&](auto const& entry) { return !onlineClients.contains(entry.first); });
    std::vector<PopulationZoneDemand> zoneDemands;
    for (auto const& [key, region] : regions)
        zoneDemands.push_back({key.first, key.second, region.Players});
    for (auto const& quota : PlanWorldPopulation(std::move(zoneDemands),
        sPlayerbotAIConfig.PopulationWorldCount, sPlayerbotAIConfig.PopulationMinPerZone))
    {
        Region const& region = regions.at({quota.Map, quota.Zone});
        BotPopulationAssignment assignment;
        assignment.Map = quota.Map;
        assignment.Zone = quota.Zone;
        auto [low, high] = sTravelMgr.GetZoneLevelRange(assignment.Zone);
        if (!high)
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(assignment.Zone);
            low = area && area->area_level > 0
                ? std::min<uint32>(MAX_PLAYER_LEVEL, area->area_level) : region.Level;
        }
        assignment.Level = low;
        uint32 ordinary = quota.Bots;
        if (sPlayerbotAIConfig.PopulationNearbySpawns)
            for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            {
                if (!ordinary || !IsPopulationClient(player) || player->IsBeingTeleported() ||
                    player->GetMapId() != quota.Map || player->GetMap()->Instanceable() ||
                    player->GetZoneId() != quota.Zone)
                    continue;
                auto [count, inserted] = _populationNearbyCounts.try_emplace(guid, 0);
                if (inserted)
                    count->second = urand(1, 3);
                BotPopulationAssignment nearby = assignment;
                nearby.Nearby = true;
                nearby.Owner = guid;
                nearby.Team = player->GetTeamId();
                uint32 slots = std::min(ordinary, count->second);
                requests.push_back({nearby, slots});
                ordinary -= slots;
            }
        if (ordinary)
            requests.push_back({assignment, ordinary});
    }

    CheckBgQueue();
    CheckLfgQueue();
    if (sPlayerbotAIConfig.RandomBotJoinBG)
        for (auto const& [queueId, brackets] : BattlegroundData)
            for (auto const& [bracket, data] : brackets)
            {
                Battleground* battleground = sBattlegroundMgr->GetBattlegroundTemplate(
                    sBattlegroundMgr->BGTemplateId(BattlegroundQueueTypeId(queueId)));
                if (!battleground)
                    continue;
                uint32 arenaSize = BattlegroundMgr::BGArenaType(BattlegroundQueueTypeId(queueId));
                for (uint32 mode = 0; mode < (arenaSize ? 2u : 1u); ++mode)
                {
                    uint32 realCount = arenaSize ? (mode ? data.ratedArenaPlayerCount : data.skirmishArenaPlayerCount)
                                                : data.bgAlliancePlayerCount + data.bgHordePlayerCount;
                    if (!realCount)
                        continue;
                    uint32 instances = arenaSize
                        ? (mode ? data.ratedArenaInstanceCount : data.skirmishArenaInstanceCount)
                        : data.bgInstanceCount;
                    uint32 activeQueue = arenaSize ? (mode ? data.activeRatedArenaQueue : data.activeSkirmishArenaQueue)
                                                   : data.activeBgQueue;
                    uint32 capacity = arenaSize ? arenaSize : battleground->GetMaxPlayersPerTeam();
                    for (TeamId team : {TEAM_ALLIANCE, TEAM_HORDE})
                    {
                        if (arenaSize && team == TEAM_HORDE)
                            continue;
                        uint32 playersInTeam = arenaSize ? realCount :
                            (team == TEAM_ALLIANCE ? data.bgAlliancePlayerCount : data.bgHordePlayerCount);
                        uint32 target = capacity * (instances + activeQueue) * (arenaSize ? 2u : 1u);
                        uint32 held = arenaSize ? 0 : GetBattlegroundReplacementHold(queueId, bracket, team);
                        if (target <= playersInTeam || target - playersInTeam <= held)
                            continue;
                        BotPopulationAssignment assignment;
                        assignment.Purpose = arenaSize ? BotPopulationPurpose::Arena
                                                       : BotPopulationPurpose::Battleground;
                        assignment.Queue = queueId;
                        assignment.Bracket = bracket;
                        assignment.Team = arenaSize ? TEAM_NEUTRAL : team;
                        assignment.Rated = mode != 0;
                        assignment.Level = std::min(MAX_PLAYER_LEVEL, data.maxLevel);
                        if (!assignment.Level)
                            continue;
                        requests.push_back({assignment, target - playersInTeam - held});
                    }
                }
            }

    std::set<ObjectGuid> seenGroups;
    if (sPlayerbotAIConfig.RandomBotJoinLfg)
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            // Dungeon queues and groups survive transfers, when connected clients briefly leave the world.
            if (!IsClientControlledInstancePlayer(player))
                continue;
            Group* group = player->GetGroup();
            ObjectGuid owner = group ? group->GetGUID() : guid;
            if (HasPendingDungeonReplacement(owner))
                continue;
            if (!seenGroups.insert(owner).second)
                continue;
            lfg::LfgState status = sLFGMgr->GetState(owner);
            if (status != lfg::LFG_STATE_QUEUED && status != lfg::LFG_STATE_ROLECHECK)
                continue;
            auto const& dungeons = sLFGMgr->GetSelectedDungeons(guid);
            if (dungeons.empty())
                continue;
            lfg::LfgRolesMap queuedRoles;
            auto collectRole = [&](Player* member)
            {
                if (member && member->GetSession() && !member->GetSession()->IsLoggingOut())
                    queuedRoles[member->GetGUID()] = sLFGMgr->GetRoles(member->GetGUID());
            };
            if (group)
                for (auto const& member : group->GetMemberSlots())
                    collectRole(ObjectAccessor::FindConnectedPlayer(member.guid));
            else
                collectRole(player);
            if (!lfg::LFGMgr::CheckGroupRoles(queuedRoles))
                continue;
            std::array<uint32, 3> missing = {1, 1, 3};
            for (auto const& [member, role] : queuedRoles)
            {
                if (role & lfg::PLAYER_ROLE_TANK)
                    --missing[0];
                else if (role & lfg::PLAYER_ROLE_HEALER)
                    --missing[1];
                else if (role & lfg::PLAYER_ROLE_DAMAGE)
                    --missing[2];
            }
            constexpr std::array<uint8, 3> roles =
                {lfg::PLAYER_ROLE_TANK, lfg::PLAYER_ROLE_HEALER, lfg::PLAYER_ROLE_DAMAGE};
            for (uint32 index = 0; index < missing.size(); ++index)
            {
                if (!missing[index])
                    continue;
                BotPopulationAssignment assignment;
                assignment.Purpose = BotPopulationPurpose::Dungeon;
                assignment.Owner = owner;
                assignment.Team = player->GetTeamId();
                assignment.Level = player->GetLevel();
                assignment.Role = roles[index];
                assignment.Dungeons = dungeons;
                requests.push_back({assignment, missing[index]});
            }
        }

    for (auto const& replacement : GetDungeonReplacementRequests())
        requests.push_back({replacement.Assignment, replacement.Count});

    std::erase_if(_populationNearbyCounts, [&](auto const& entry)
    {
        return std::none_of(requests.begin(), requests.end(), [&](Request const& request)
        { return request.Assignment.Nearby && request.Assignment.Owner == entry.first; });
    });
    auto priority = [](BotPopulationAssignment const& assignment)
    {
        return assignment.Purpose != BotPopulationPurpose::World ? 0u : assignment.Nearby ? 1u : 2u;
    };
    std::stable_sort(requests.begin(), requests.end(), [&](Request const& left, Request const& right)
    { return priority(left.Assignment) < priority(right.Assignment); });
    bool operationUsed = false;
    auto operationDue = [&](BotPopulationAssignment const& assignment)
    {
        return !operationUsed && PopulationOperationDue(now, assignment.Purpose == BotPopulationPurpose::World
            ? _populationNextOperation : _populationNextActivityOperation);
    };
    auto finishOperation = [&](BotPopulationAssignment const& assignment)
    {
        if (assignment.Purpose == BotPopulationPurpose::World)
            _populationNextOperation = NowSeconds() + sPlayerbotAIConfig.PopulationOperationInterval;
        else
            _populationNextActivityOperation = NowSeconds() + sPlayerbotAIConfig.PopulationActivityOperationInterval;
        operationUsed = true;
    };
    std::map<ObjectGuid, uint32> arenaTeamSizes;
    for (auto const& [id, reservation] : _populationReservations)
        if (reservation.Assignment.Rated)
            ++arenaTeamSizes[reservation.Assignment.Owner];
    std::set<ObjectGuid> wantedArenaTeams;
    for (auto const& [id, reservation] : _populationReservations)
    {
        auto const& assignment = reservation.Assignment;
        if (!assignment.Rated || wantedArenaTeams.contains(assignment.Owner))
            continue;
        uint32 size = BattlegroundMgr::BGArenaType(BattlegroundQueueTypeId(assignment.Queue));
        if (arenaTeamSizes[assignment.Owner] != size)
            continue;
        for (Request& request : requests)
            if (SameRequest(request.Assignment, assignment) &&
                ReserveArenaTeamSlots(request.Count, size, request.Reserved))
            {
                wantedArenaTeams.insert(assignment.Owner);
                break;
            }
    }

    std::set<uint32> retainedWorldBots;
    std::set<uint32> invitedWorldBots;
    for (auto const& [id, reservation] : _populationReservations)
        if (reservation.Assignment.Purpose == BotPopulationPurpose::World)
            if (Player* bot = GetPlayerBot(id))
            {
                if (HasClientInGroup(bot))
                    retainedWorldBots.insert(id);
                else if (HasClientGroupInvite(bot))
                {
                    invitedWorldBots.insert(id);
                    BotPopulationState const* state = GetBotPopulation(bot);
                    if (!state || !state->Prepared)
                        continue;
                    retainedWorldBots.insert(id);
                    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                        if (bot->IsInWorld() && !bot->IsBeingTeleported() && !bot->GetSession()->IsLoggingOut())
                        {
                            AcceptInvitationAction accept(ai);
                            accept.Execute(Event());
                        }
                }
            }

    std::set<uint32> wantedWorldBots;
    auto reserveWorldBot = [&](uint32 id, PopulationReservation const& reservation)
    {
        for (Request& request : requests)
            if (request.Reserved < request.Count && SameRequest(request.Assignment, reservation.Assignment))
            {
                ++request.Reserved;
                wantedWorldBots.insert(id);
                break;
            }
    };
    // Keep recruited bots in their matching quota before assigning ordinary world bots.
    for (uint32 id : retainedWorldBots)
        reserveWorldBot(id, _populationReservations.at(id));
    for (auto const& [id, reservation] : _populationReservations)
        if (reservation.Assignment.Purpose == BotPopulationPurpose::World && !retainedWorldBots.contains(id))
            reserveWorldBot(id, reservation);

    auto hasRequest = [&](uint32 id, PopulationReservation const& reservation)
    {
        if (IsActivityReplacementExcluded(reservation.Assignment, ObjectGuid::Create<HighGuid::Player>(id)))
            return false;
        return std::any_of(requests.begin(), requests.end(), [&](Request const& request)
        {
            return SameRequest(request.Assignment, reservation.Assignment);
        });
    };
    for (Request& request : requests)
    {
        for (auto const& [id, reservation] : _populationReservations)
            if (reservation.Ready && SameRequest(request.Assignment, reservation.Assignment) &&
                !IsActivityReplacementExcluded(reservation.Assignment, ObjectGuid::Create<HighGuid::Player>(id)))
                ++request.Progress;
        auto const& assignment = request.Assignment;
        if (assignment.Purpose == BotPopulationPurpose::Battleground)
        {
            auto const& queue = BattlegroundData[assignment.Queue][assignment.Bracket];
            request.Progress += assignment.Team == TEAM_ALLIANCE
                ? queue.bgAlliancePlayerCount : queue.bgHordePlayerCount;
        }
    }
    auto progress = [&](BotPopulationAssignment const& assignment)
    {
        auto request = std::find_if(requests.begin(), requests.end(), [&](Request const& value)
        { return SameRequest(assignment, value.Assignment); });
        return request == requests.end() || assignment.Purpose == BotPopulationPurpose::World
            ? 0u : request->Progress;
    };
    uint32 operationCandidate = 0;
    auto operationOrder = std::make_tuple(WORLD_RETRY_PRIORITY + 1, std::numeric_limits<uint32>::max(), true, true,
        std::numeric_limits<uint32>::max(),
        std::numeric_limits<uint32>::max());
    for (auto const& [id, reservation] : _populationReservations)
    {
        if (retainedWorldBots.contains(id) || !operationDue(reservation.Assignment))
            continue;
        if (reservation.Ready)
        {
            Player* bot = GetPlayerBot(id);
            if (reservation.Assignment.Purpose == BotPopulationPurpose::World && bot && bot->IsInWorld() &&
                !bot->IsBeingTeleported() && wantedWorldBots.contains(id) &&
                (bot->GetMapId() != reservation.Assignment.Map ||
                    !IsPopulationPatrolZone(reservation.Assignment.Zone, bot->GetZoneId())))
            {
                auto order = std::make_tuple(priority(reservation.Assignment), 0u, false, false, 0u, id);
                if (order < operationOrder)
                {
                    operationOrder = order;
                    operationCandidate = id;
                }
            }
            continue;
        }
        if (!invitedWorldBots.contains(id) && (!hasRequest(id, reservation) ||
            (reservation.Assignment.Purpose == BotPopulationPurpose::World && !wantedWorldBots.contains(id))))
            continue;
        Player* bot = GetPlayerBot(id);
        if (reservation.LoginRequested && (!bot || !bot->IsInWorld() || bot->IsBeingTeleported()))
            continue;
        BotPopulationState const* state = GetBotPopulation(bot);
        bool prepared = state && state->Prepared;
        if (prepared && now - reservation.Created < 60)
            continue;
        // A failed nearby spawn must not starve the remaining world's first initialization.
        uint32 operationPriority = prepared && reservation.Assignment.Purpose == BotPopulationPurpose::World
            ? WORLD_RETRY_PRIORITY : priority(reservation.Assignment);
        auto order = std::make_tuple(operationPriority, progress(reservation.Assignment),
            prepared, !reservation.LoginRequested,
            reservation.Created, id);
        if (order < operationOrder)
        {
            operationOrder = order;
            operationCandidate = id;
        }
    }

    for (auto itr = _populationReservations.begin(); itr != _populationReservations.end();)
    {
        uint32 id = itr->first;
        auto& reservation = itr->second;
        Player* bot = GetPlayerBot(id);
        // Keep group members and prepared invitees out of relocation and retirement.
        if (retainedWorldBots.contains(id))
        {
            if (BotPopulationState const* state = GetBotPopulation(bot))
                reservation.Ready = state->Ready;
            ++itr;
            continue;
        }

        bool wanted = wantedWorldBots.contains(id) || invitedWorldBots.contains(id) ||
            (wantedArenaTeams.contains(reservation.Assignment.Owner) && reservation.Assignment.Rated);
        if (!reservation.Assignment.Rated && reservation.Assignment.Purpose != BotPopulationPurpose::World &&
            !IsActivityReplacementExcluded(reservation.Assignment, ObjectGuid::Create<HighGuid::Player>(id)))
        {
            auto request = std::find_if(requests.begin(), requests.end(), [&](Request const& value)
            {
                return value.Reserved < value.Count && SameRequest(value.Assignment, reservation.Assignment);
            });
            wanted = request != requests.end();
            if (wanted)
                ++request->Reserved;
        }
        if (!wanted && reservation.Assignment.Purpose == BotPopulationPurpose::World &&
            (!bot || !bot->IsBeingTeleported()) && !operationCandidate && operationDue(reservation.Assignment))
        {
            CharacterCacheEntry const* character = sCharacterCache->GetCharacterCacheByGuid(
                ObjectGuid::Create<HighGuid::Player>(id));
            if (character)
                for (Request& request : requests)
                {
                    if (request.Assignment.Purpose != BotPopulationPurpose::World || request.Reserved >= request.Count)
                        continue;
                    BotPopulationAssignment assignment = request.Assignment;
                    TeamId team = IsAlliance(character->Race) ? TEAM_ALLIANCE : TEAM_HORDE;
                    if ((assignment.Team != TEAM_NEUTRAL && assignment.Team != team) ||
                        !ResolveWorldAssignment(character->Class, team, assignment))
                        continue;
                    reservation.Assignment = assignment;
                    reservation.Ready = false;
                    finishOperation(reservation.Assignment);
                    if (bot)
                    {
                        bot->CombatStop(true);
                        bot->GetMotionMaster()->Clear();
                        bot->SetPhaseMask(STAGING_PHASE_MASK, true);
                        auto* state = new BotPopulationState();
                        state->Assignment = assignment;
                        bot->CustomData.Set(BotPopulationState::Key, state);
                    }
                    ++request.Reserved;
                    wanted = true;
                    break;
                }
        }
        lfg::LfgState lfgState = bot ? sLFGMgr->GetState(bot->GetGUID()) : lfg::LFG_STATE_NONE;
        bool queuedDungeonGroup = bot && bot->GetGroup() && bot->GetGroup()->isLFGGroup() &&
            (lfgState == lfg::LFG_STATE_QUEUED || lfgState == lfg::LFG_STATE_ROLECHECK);
        bool activeDungeon = lfgState == lfg::LFG_STATE_PROPOSAL || lfgState == lfg::LFG_STATE_BOOT ||
            lfgState == lfg::LFG_STATE_DUNGEON || queuedDungeonGroup;
        if (bot && bot->GetGroup() && reservation.Assignment.Purpose == BotPopulationPurpose::Dungeon &&
            reservation.Assignment.Dungeons.empty() && bot->GetGroup()->GetGUID() == reservation.Assignment.Owner)
            activeDungeon = true;
        // A proposal precedes group creation, so a solo bot has no client group to keep it alive yet.
        bool pendingDungeonProposal = bot && reservation.Assignment.Purpose == BotPopulationPurpose::Dungeon &&
            lfgState == lfg::LFG_STATE_PROPOSAL;
        bool inUse = bot && reservation.Assignment.Purpose != BotPopulationPurpose::World &&
            (pendingDungeonProposal || HasClientInInstance(bot) || (HasClientInGroup(bot) && activeDungeon));
        bool timedOut = reservation.LoginRequested && !bot && (now - reservation.Created > LOGIN_TIMEOUT ||
            (reservation.Ready && !botLoading.contains(ObjectGuid::Create<HighGuid::Player>(id))));
        if ((!wanted && !inUse) || timedOut)
        {
            bool retiringLogin = bot || reservation.LoginRequested;
            if (retiringLogin && !PopulationOperationDue(now, _populationNextRetirement))
            {
                ++itr;
                continue;
            }
            if (bot && (bot->IsBeingTeleported() ||
                bot->CustomData.Get<InstanceExitCleanupState>(InstanceExitCleanupState::Key)))
            {
                ++itr;
                continue;
            }
            ObjectGuid botGuid = ObjectGuid::Create<HighGuid::Player>(id);
            if (reservation.Assignment.Purpose == BotPopulationPurpose::Dungeon)
                LOG_INFO("playerbots", "Population: retiring dungeon bot {} wanted {} in use {} "
                    "LFG state {} connected group client {} timed out {}", botGuid.ToString(), wanted, inUse,
                    uint32(lfgState), bot && HasClientInGroup(bot), timedOut);
            if (bot)
                LogoutPlayerBot(botGuid);
            else if (reservation.LoginRequested)
                AbandonPendingLogin(botGuid);
            currentBots.erase(id);
            SetEventValue(id, "add", 0, 0);
            itr = _populationReservations.erase(itr);
            if (retiringLogin)
                _populationNextRetirement = NowSeconds() + sPlayerbotAIConfig.PopulationLogoutInterval;
            continue;
        }
        if (bot && reservation.Ready && reservation.Assignment.Purpose == BotPopulationPurpose::World &&
            bot->GetPhaseMask() == STAGING_PHASE_MASK && bot->IsInWorld() && !bot->IsBeingTeleported())
        {
            auto* state = bot->CustomData.Get<BotPopulationState>(BotPopulationState::Key);
            WorldLocation location(bot->GetMapId(), bot->GetPosition());
            Player* owner = reservation.Assignment.Nearby
                ? ObjectAccessor::FindConnectedPlayer(reservation.Assignment.Owner) : nullptr;
            float ownerDistance = owner ? owner->GetExactDist2d(bot) : 0.0f;
            bool spawnAllowed = !reservation.Assignment.Nearby ||
                (IsPopulationClient(owner) && owner->GetMap() == bot->GetMap() &&
                    ownerDistance >= NEARBY_SPAWN_MIN_DISTANCE && ownerDistance <= NEARBY_SPAWN_MAX_DISTANCE);
            if (location.GetMapId() == reservation.Assignment.Map &&
                IsPopulationPatrolZone(reservation.Assignment.Zone, bot->GetZoneId()) && spawnAllowed)
                bot->SetPhaseMask(PHASEMASK_NORMAL, true);
            else
            {
                reservation.Ready = false;
                if (state)
                    state->Ready = false;
            }
        }
        if (bot && !reservation.Ready && id == operationCandidate &&
            operationDue(reservation.Assignment) &&
            bot->IsInWorld() && !bot->IsBeingTeleported())
        {
            PreparePopulationBot(bot, reservation.Assignment);
            BotPopulationState const* state = GetBotPopulation(bot);
            reservation.Ready = state && state->Ready;
            reservation.Created = NowSeconds();
            finishOperation(reservation.Assignment);
        }
        else if (bot && reservation.Ready && reservation.Assignment.Purpose == BotPopulationPurpose::World &&
            !bot->IsBeingTeleported() && bot->IsInWorld() &&
            id == operationCandidate && operationDue(reservation.Assignment) &&
            (bot->GetMapId() != reservation.Assignment.Map ||
                !IsPopulationPatrolZone(reservation.Assignment.Zone, bot->GetZoneId())))
        {
            bot->CombatStop(true);
            bot->GetMotionMaster()->Clear();
            auto* state = bot->CustomData.Get<BotPopulationState>(BotPopulationState::Key);
            if (state)
                state->Ready = false;
            reservation.Ready = false;
            PreparePopulationBot(bot, reservation.Assignment);
            reservation.Ready = state && state->Ready;
            reservation.Created = NowSeconds();
            finishOperation(reservation.Assignment);
        }
        else if (bot && reservation.Ready && bot->isDead() && !bot->InBattleground() &&
            !bot->GetMap()->Instanceable())
            Revive(bot);
        if (bot && reservation.Ready && reservation.Assignment.Purpose == BotPopulationPurpose::Dungeon &&
            reservation.Assignment.Dungeons.empty())
            AttachDungeonReplacement(bot, reservation.Assignment);
        ++itr;
    }

    for (auto const& [id, reservation] : _populationReservations)
    {
        if (!reservation.Ready || !reservation.Assignment.Rated ||
            reservation.Assignment.Owner != ObjectGuid::Create<HighGuid::Player>(id))
            continue;
        Player* captain = GetPlayerBot(id);
        ArenaType type = ArenaType(BattlegroundMgr::BGArenaType(
            BattlegroundQueueTypeId(reservation.Assignment.Queue)));
        if (!captain || captain->GetGroup() || captain->IsBeingTeleported() ||
            captain->InBattlegroundQueue() || captain->InBattleground())
            continue;
        std::vector<Player*> members;
        for (auto const& [memberId, memberReservation] : _populationReservations)
        {
            if (!memberReservation.Ready || !memberReservation.Assignment.Rated ||
                memberReservation.Assignment.Owner != captain->GetGUID() ||
                memberReservation.Assignment.Queue != reservation.Assignment.Queue)
                continue;
            Player* member = GetPlayerBot(memberId);
            if (member && !member->GetGroup() && !member->IsBeingTeleported())
                members.push_back(member);
        }
        if (members.size() != static_cast<uint32>(type))
            continue;
        ArenaTeam* arena = sArenaTeamMgr->GetArenaTeamByCaptain(captain->GetGUID(), type);
        if (!arena)
        {
            std::string name = "Population " + std::to_string(id) + " " + std::to_string(type);
            if (sArenaTeamMgr->GetArenaTeamByName(name))
                continue;
            arena = new ArenaTeam();
            if (!arena->Create(captain->GetGUID(), type, name, 0, 0, 0, 0, 0))
            {
                delete arena;
                continue;
            }
            sArenaTeamMgr->AddArenaTeam(arena);
        }
        for (Player* member : members)
            if (!member->GetArenaTeamId(arena->GetSlot()))
                arena->AddMember(member->GetGUID());
        if (arena->GetMembersSize() < static_cast<uint32>(type))
            continue;
        auto* group = new Group();
        if (!group->Create(captain))
        {
            delete group;
            continue;
        }
        sGroupMgr->AddGroup(group);
        for (Player* member : members)
            if (member != captain && group->GetMembersCount() < static_cast<uint32>(type))
                group->AddMember(member);
    }

    uint32 activityCount = 0;
    uint32 worldCount = 0;
    for (auto const& [id, reservation] : _populationReservations)
        if (reservation.Assignment.Purpose != BotPopulationPurpose::World)
            ++activityCount;
        else
            ++worldCount;
    uint32 logins = 0;
    auto reserve = [&](PopulationCandidate const& candidate, BotPopulationAssignment assignment)
    {
        _populationReservations.emplace(candidate.Guid, PopulationReservation{assignment});
        currentBots.insert(candidate.Guid);
        SetEventValue(candidate.Guid, "add", 1, sPlayerbotAIConfig.PermanentlyInWorldTime);
        SetEventValue(candidate.Guid, "logout", 0, 0);
        // Reserving a complete arena team is cheap; each actual login is dispatched separately below.
        ++logins;
        if (assignment.Purpose != BotPopulationPurpose::World)
            ++activityCount;
        else
            ++worldCount;
    };
    for (Request& request : requests)
    {
        while (request.Reserved < request.Count && logins < LOGINS_PER_UPDATE && !_populationCandidates.empty())
        {
            auto assignment = request.Assignment;
            if ((assignment.Purpose != BotPopulationPurpose::World &&
                activityCount >= sPlayerbotAIConfig.PopulationActivityLimit) ||
                (assignment.Purpose == BotPopulationPurpose::World &&
                    worldCount >= sPlayerbotAIConfig.PopulationWorldCount))
                break;
            bool found = false;
            for (uint32 attempt = 0; attempt < _populationCandidates.size(); ++attempt)
            {
                PopulationCandidate candidate = _populationCandidates[
                    _populationCandidateCursor++ % _populationCandidates.size()];
                ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(candidate.Guid);
                if (_populationReservations.contains(candidate.Guid) || ObjectAccessor::FindConnectedPlayer(guid) ||
                    botLoading.contains(guid) || IsActivityReplacementExcluded(assignment, guid) ||
                    !SupportsRole(candidate.Class, assignment.Role) ||
                    (sPlayerbotAIConfig.DisableDeathKnightLogin && candidate.Class == CLASS_DEATH_KNIGHT))
                    continue;
                TeamId team = IsAlliance(candidate.Race) ? TEAM_ALLIANCE : TEAM_HORDE;
                if (assignment.Team != TEAM_NEUTRAL && assignment.Team != team)
                    continue;
                if (assignment.Purpose == BotPopulationPurpose::World)
                {
                    CharacterCacheEntry const* character = sCharacterCache->GetCharacterCacheByGuid(guid);
                    if (character && std::any_of(character->ArenaTeamId.begin(), character->ArenaTeamId.end(),
                        [](uint32 teamId) { return teamId != 0; }))
                        continue;
                    if (!ResolveWorldAssignment(candidate.Class, team, assignment))
                        continue;
                }
                else
                {
                    if (candidate.Class == CLASS_DEATH_KNIGHT && assignment.Level < DEATH_KNIGHT_MIN_LEVEL)
                        continue;
                    // Queue staging stays outside the world population in a faction capital.
                    assignment.Location = team == TEAM_ALLIANCE
                        ? WorldLocation(0, -8833.38f, 628.62f, 94.0f, 0.0f)
                        : WorldLocation(1, 1633.33f, -4439.11f, 16.0f, 0.0f);
                    if (assignment.Purpose == BotPopulationPurpose::Arena)
                    {
                        BattlegroundTypeId type = sBattlegroundMgr->BGTemplateId(
                            BattlegroundQueueTypeId(assignment.Queue));
                        bool staged = false;
                        for (TeamId faction : {team, TEAM_NEUTRAL})
                        {
                            for (uint32 entry : BattleMastersCache[faction][type])
                            {
                                CreatureData const* data = GetCreatureDataByEntry(entry);
                                if (!data)
                                    continue;
                                assignment.Location = WorldLocation(data->mapid, data->posX + 2.0f,
                                    data->posY, data->posZ, data->orientation);
                                staged = true;
                                break;
                            }
                            if (staged)
                                break;
                        }
                        if (!staged)
                            continue;
                    }
                }
                if (assignment.Rated)
                {
                    ArenaType type = ArenaType(BattlegroundMgr::BGArenaType(BattlegroundQueueTypeId(assignment.Queue)));
                    uint32 size = static_cast<uint32>(type);
                    if (activityCount + size > sPlayerbotAIConfig.PopulationActivityLimit ||
                        request.Reserved + size > request.Count)
                        continue;
                    ArenaTeam* arena = sArenaTeamMgr->GetArenaTeamByCaptain(guid, type);
                    std::vector<PopulationCandidate> members;
                    if (arena)
                    {
                        for (auto const& member : arena->GetMembers())
                        {
                            if (members.size() >= size)
                                break;
                            auto available = std::find_if(_populationCandidates.begin(), _populationCandidates.end(),
                                [&](PopulationCandidate const& value)
                                {
                                    return value.Guid == member.Guid.GetCounter();
                                });
                            if (available == _populationCandidates.end() ||
                                _populationReservations.contains(available->Guid) ||
                                ObjectAccessor::FindConnectedPlayer(member.Guid) || botLoading.contains(member.Guid))
                                continue;
                            members.push_back(*available);
                        }
                    }
                    else
                    {
                        uint8 slot = ArenaTeam::GetSlotByType(type);
                        if (sCharacterCache->GetCharacterArenaTeamIdByGuid(guid, slot))
                            continue;
                        members.push_back(candidate);
                        for (auto const& member : _populationCandidates)
                        {
                            if (members.size() >= size)
                                break;
                            ObjectGuid memberGuid = ObjectGuid::Create<HighGuid::Player>(member.Guid);
                            if (member.Guid == candidate.Guid ||
                                IsAlliance(member.Race) != IsAlliance(candidate.Race) ||
                                _populationReservations.contains(member.Guid) ||
                                ObjectAccessor::FindConnectedPlayer(memberGuid) || botLoading.contains(memberGuid) ||
                                sCharacterCache->GetCharacterArenaTeamIdByGuid(memberGuid, slot))
                                continue;
                            members.push_back(member);
                        }
                    }
                    if (members.size() != size || std::none_of(members.begin(), members.end(), [&](auto const& value)
                        { return value.Guid == candidate.Guid; }))
                        continue;
                    assignment.Owner = guid;
                    for (auto const& member : members)
                        reserve(member, assignment);
                    request.Reserved += size;
                }
                else
                {
                    reserve(candidate, assignment);
                    ++request.Reserved;
                }
                found = true;
                break;
            }
            if (!found || assignment.Purpose != BotPopulationPurpose::World)
                break;
        }
    }
    if (!operationUsed)
    {
        auto candidate = _populationReservations.end();
        for (auto itr = _populationReservations.begin(); itr != _populationReservations.end(); ++itr)
        {
            auto const& reservation = itr->second;
            if (reservation.LoginRequested || !hasRequest(itr->first, reservation) ||
                !operationDue(reservation.Assignment))
                continue;
            if (candidate == _populationReservations.end() ||
                std::make_pair(priority(reservation.Assignment), progress(reservation.Assignment)) <
                    std::make_pair(priority(candidate->second.Assignment), progress(candidate->second.Assignment)))
                candidate = itr;
        }
        if (candidate != _populationReservations.end())
        {
            auto& reservation = candidate->second;
            reservation.LoginRequested = true;
            reservation.Created = now;
            AddPlayerBot(ObjectGuid::Create<HighGuid::Player>(candidate->first), 0);
            finishOperation(reservation.Assignment);
        }
    }
    SetEventValue(0, "bot_count", sPlayerbotAIConfig.PopulationWorldCount + activityCount, DAY);
    uint32 readyWorld = 0;
    uint32 readyActivity = 0;
    for (auto const& [id, reservation] : _populationReservations)
        if (reservation.Ready && GetPlayerBot(id))
        {
            if (reservation.Assignment.Purpose == BotPopulationPurpose::World)
                ++readyWorld;
            else
                ++readyActivity;
        }
    static std::pair<uint32, uint32> previousCounts = {0, 0};
    if (previousCounts != std::make_pair(readyWorld, readyActivity))
    {
        LOG_INFO("playerbots", "Population: world {}/{}, activities {}/{}, occupied zones {}",
            readyWorld, regions.empty() ? 0u : sPlayerbotAIConfig.PopulationWorldCount,
            readyActivity, sPlayerbotAIConfig.PopulationActivityLimit, regions.size());
        previousCounts = {readyWorld, readyActivity};
    }
    static uint32 lastShortage = 0;
    if (!logins && now - lastShortage >= 30)
        for (auto const& request : requests)
            if (request.Reserved < request.Count &&
                (request.Assignment.Purpose == BotPopulationPurpose::World ||
                    activityCount < sPlayerbotAIConfig.PopulationActivityLimit))
            {
                LOG_WARN("playerbots", "Population: no eligible offline characters for purpose {}, missing {} bots",
                    static_cast<uint32>(request.Assignment.Purpose), request.Count - request.Reserved);
                lastShortage = now;
                break;
            }
}
