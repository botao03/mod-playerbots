/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityReplacement.h"
#include "ActivityReplacementPolicy.h"
#include "AllBattlegroundScript.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "EmptyInstanceCleanup.h"
#include "Group.h"
#include "GroupMgr.h"
#include "GroupScript.h"
#include "LFGMgr.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Playerbots.h"
#include "TaskScheduler.h"
#include "WorldPacket.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace
{
int64 NowMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Enabled() { return sPlayerbotAIConfig.PopulationEnabled; }

enum class ReplacementEventType
{
    GroupKick, GroupAdd, GroupDisband, BattlegroundLeave, BattlegroundAdd, BootVote
};

struct ReplacementEvent
{
    ReplacementEventType Type;
    ObjectGuid Group;
    ObjectGuid Player;
    ObjectGuid Victim;
    uint32 Instance = 0;
    BattlegroundTypeId BattlegroundType = BATTLEGROUND_TYPE_NONE;
    TeamId Team = TEAM_NEUTRAL;
    bool InProgress = false;
    bool HumanApproval = false;
    bool BotPending = false;
    int64 Time = NowMilliseconds();
};

struct DungeonReplacement
{
    uint32 Map = 0;
    uint32 Instance = 0;
    uint32 Level = 1;
    TeamId Team = TEAM_NEUTRAL;
    ActivityVacancies Vacancies;
    std::set<ObjectGuid> Excluded;
    std::set<ObjectGuid> Attached;
};

struct BattlegroundReplacement
{
    BattlegroundTypeId Type = BATTLEGROUND_TYPE_NONE;
    uint32 Queue = 0;
    uint32 Bracket = 0;
    ActivityVacancies Vacancies;
    std::set<ObjectGuid> Excluded;
};

struct BootVotes
{
    ObjectGuid Victim;
    bool HumanApproved = false;
    std::set<ObjectGuid> Pending;
    std::set<ObjectGuid> Sent;
};

using BattlegroundKey = std::pair<uint32, TeamId>;

Player* FindDungeonClient(Group* group, DungeonReplacement const* target = nullptr)
{
    if (!group || group->isRaidGroup() || group->isBGGroup())
        return nullptr;
    for (auto const& slot : group->GetMemberSlots())
        if (Player* player = ObjectAccessor::FindConnectedPlayer(slot.guid))
            if (IsClientControlledInstancePlayer(player) && player->IsInWorld() &&
                !player->IsBeingTeleported() && player->GetMap()->IsDungeon() && !player->GetMap()->IsRaid() &&
                (!target || (player->GetMapId() == target->Map && player->GetInstanceId() == target->Instance)))
                return player;
    return nullptr;
}

bool HasTransferringClient(Group* group)
{
    for (auto const& slot : group->GetMemberSlots())
        if (Player* player = ObjectAccessor::FindConnectedPlayer(slot.guid))
            if (IsClientControlledInstancePlayer(player) && player->IsBeingTeleported())
                return true;
    return false;
}

bool HasBattlegroundClient(Battleground* battleground)
{
    if (!battleground || battleground->isArena() || battleground->GetStatus() >= STATUS_WAIT_LEAVE)
        return false;
    for (auto const& [guid, slot] : battleground->GetPlayers())
        if (IsClientControlledInstancePlayer(ObjectAccessor::FindConnectedPlayer(guid)))
            return true;
    return false;
}

class ActivityReplacementMgr
{
public:
    ActivityReplacementMgr()
    {
        _scheduler.Schedule(std::chrono::seconds(5), [this](TaskContext context)
        {
            Prune();
            context.Repeat();
        });
    }

    void Queue(ReplacementEvent event)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _events.push_back(std::move(event));
    }

    void Update(uint32 diff)
    {
        std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
        std::vector<ReplacementEvent> events;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            events.swap(_events);
        }
        if (!Enabled())
        {
            _dungeons.clear();
            _battlegrounds.clear();
            _votes.clear();
            _battlegroundTeams.clear();
            return;
        }
        for (ReplacementEvent const& event : events)
            Process(event);
        for (auto itr = _votes.begin(); itr != _votes.end();)
        {
            Group* group = sGroupMgr->GetGroupByGUID(itr->first.GetCounter());
            if (!group || sLFGMgr->GetState(itr->first) != lfg::LFG_STATE_BOOT)
            {
                itr = _votes.erase(itr);
                continue;
            }
            BootVotes& votes = itr->second;
            if (votes.HumanApproved)
                for (ObjectGuid guid : votes.Pending)
                {
                    if (votes.Sent.contains(guid) || !group->IsMember(guid) || guid == votes.Victim)
                        continue;
                    Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
                    if (!bot || !bot->GetSession() || !bot->GetSession()->IsHeadless() || !GET_PLAYERBOT_AI(bot))
                        continue;
                    auto* packet = new WorldPacket(CMSG_LFG_SET_BOOT_VOTE, 1);
                    *packet << bool(true);
                    bot->GetSession()->QueuePacket(packet);
                    votes.Sent.insert(guid);
                    LOG_INFO("playerbots", "Activity vote: {} votes YES to remove {}",
                        bot->GetName(), votes.Victim.ToString());
                }
            ++itr;
        }
        _scheduler.Update(diff);
    }

    bool Pending(ObjectGuid group) const
    {
        std::lock_guard<std::recursive_mutex> lock(_stateMutex);
        auto entry = _dungeons.find(group);
        return entry != _dungeons.end() && !entry->second.Vacancies.Empty();
    }

    std::vector<ActivityReplacementRequest> Requests()
    {
        std::lock_guard<std::recursive_mutex> lock(_stateMutex);
        std::vector<ActivityReplacementRequest> requests;
        if (!Enabled() || !sPlayerbotAIConfig.RandomBotJoinLfg)
            return requests;
        for (auto& [guid, replacement] : _dungeons)
        {
            Group* group = sGroupMgr->GetGroupByGUID(guid.GetCounter());
            if (!group || group->GetMembersCount() >= MAXGROUPSIZE ||
                (group->isLFGGroup() && sLFGMgr->GetState(guid) == lfg::LFG_STATE_FINISHED_DUNGEON) ||
                (!FindDungeonClient(group, &replacement) && !HasTransferringClient(group)))
                continue;
            uint32 remaining = replacement.Vacancies.Ready(NowMilliseconds(),
                MAXGROUPSIZE - group->GetMembersCount());
            if (!remaining)
                continue;
            std::array<uint32, 3> missing = {1, 1, 3};
            constexpr std::array<uint8, 3> roles =
                {lfg::PLAYER_ROLE_TANK, lfg::PLAYER_ROLE_HEALER, lfg::PLAYER_ROLE_DAMAGE};
            for (auto const& slot : group->GetMemberSlots())
            {
                uint8 role = slot.roles;
                Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
                if (!(role & (lfg::PLAYER_ROLE_TANK | lfg::PLAYER_ROLE_HEALER | lfg::PLAYER_ROLE_DAMAGE)))
                    role = member && PlayerbotAI::IsTank(member) ? lfg::PLAYER_ROLE_TANK
                        : member && PlayerbotAI::IsHeal(member) ? lfg::PLAYER_ROLE_HEALER : lfg::PLAYER_ROLE_DAMAGE;
                for (uint32 index = 0; index < roles.size(); ++index)
                    if ((role & roles[index]) && missing[index])
                    {
                        --missing[index];
                        break;
                    }
            }
            for (uint32 index = 0; index < missing.size() && remaining; ++index)
            {
                uint32 count = std::min(remaining, missing[index]);
                if (!count)
                    continue;
                BotPopulationAssignment assignment;
                assignment.Purpose = BotPopulationPurpose::Dungeon;
                assignment.Owner = guid;
                assignment.Map = replacement.Map;
                assignment.Level = replacement.Level;
                assignment.Role = roles[index];
                assignment.Team = replacement.Team;
                requests.push_back({assignment, count});
                remaining -= count;
            }
        }
        return requests;
    }

    uint32 Hold(uint32 queue, uint32 bracket, TeamId team) const
    {
        std::lock_guard<std::recursive_mutex> lock(_stateMutex);
        uint32 count = 0;
        if (Enabled())
            for (auto const& [key, entry] : _battlegrounds)
                if (key.second == team && entry.Queue == queue && entry.Bracket == bracket)
                    count += entry.Vacancies.Waiting(NowMilliseconds());
        return count;
    }

    bool Excluded(BotPopulationAssignment const& assignment, ObjectGuid bot) const
    {
        if (!Enabled() || (assignment.Purpose != BotPopulationPurpose::Dungeon &&
            assignment.Purpose != BotPopulationPurpose::Battleground))
            return false;
        std::lock_guard<std::recursive_mutex> lock(_stateMutex);
        if (assignment.Purpose == BotPopulationPurpose::Dungeon)
            return std::any_of(_dungeons.begin(), _dungeons.end(), [&](auto const& entry)
            { return entry.second.Excluded.contains(bot); });
        if (assignment.Purpose == BotPopulationPurpose::Battleground)
            return std::any_of(_battlegrounds.begin(), _battlegrounds.end(), [&](auto const& entry)
            {
                return entry.first.second == assignment.Team && entry.second.Queue == assignment.Queue &&
                    entry.second.Bracket == assignment.Bracket && entry.second.Excluded.contains(bot);
            });
        return false;
    }

    void Attach(Player* bot, BotPopulationAssignment const& assignment)
    {
        std::lock_guard<std::recursive_mutex> lock(_stateMutex);
        if (!Enabled() || !bot || !bot->IsAlive() || bot->IsInCombat() ||
            assignment.Purpose != BotPopulationPurpose::Dungeon || !assignment.Dungeons.empty() ||
            !assignment.Owner.IsGroup() || bot->GetGroup() || !bot->IsInWorld() || bot->IsBeingTeleported())
            return;
        auto entry = _dungeons.find(assignment.Owner);
        Group* group = sGroupMgr->GetGroupByGUID(assignment.Owner.GetCounter());
        if (entry == _dungeons.end() || !group || group->IsFull() || Excluded(assignment, bot->GetGUID()) ||
            !entry->second.Vacancies.Ready(NowMilliseconds(), MAXGROUPSIZE - group->GetMembersCount()))
            return;
        Player* client = FindDungeonClient(group, &entry->second);
        if (!client || client->IsInCombat())
            return;
        if (group->isLFGGroup())
        {
            lfg::LfgState state = sLFGMgr->GetState(assignment.Owner);
            if (state != lfg::LFG_STATE_DUNGEON && state != lfg::LFG_STATE_QUEUED)
                return;
            sLFGMgr->InitializeLockedDungeons(bot, group);
            uint32 dungeon = sLFGMgr->GetDungeon(assignment.Owner, false);
            auto const& locks = sLFGMgr->GetLockedDungeons(bot->GetGUID());
            if (locks.contains(dungeon))
            {
                entry->second.Excluded.insert(bot->GetGUID());
                LOG_WARN("playerbots", "Activity refill: {} blocked dungeon {} lock {}",
                    bot->GetName(), dungeon, locks.at(dungeon));
                return;
            }
        }
        if (!group->AddMember(bot, assignment.Role))
            return;
        bool teleported = false;
        if (group->isLFGGroup())
        {
            sLFGMgr->SetupGroupMember(bot->GetGUID(), assignment.Owner);
            sLFGMgr->SetRoles(bot->GetGUID(), assignment.Role);
            WorldLocation location(client->GetMapId(), client->GetPosition());
            sLFGMgr->TeleportPlayer(bot, false, &location);
            teleported = bot->IsBeingTeleported();
        }
        else
        {
            bot->SetEntryPoint();
            teleported = bot->TeleportTo(client->GetMapId(), client->GetPositionX(), client->GetPositionY(),
                client->GetPositionZ(), client->GetOrientation(), 0, nullptr, true);
        }
        if (!teleported)
        {
            entry->second.Excluded.insert(bot->GetGUID());
            group->RemoveMember(bot->GetGUID());
            LOG_WARN("playerbots", "Activity refill: {} could not enter map {} instance {}",
                bot->GetName(), entry->second.Map, entry->second.Instance);
            return;
        }
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
        {
            ai->SetMaster(client);
            ai->ResetStrategies();
        }
        // OnAddMember is delivered next update; consume now so another bot cannot claim this vacancy.
        entry->second.Vacancies.Added();
        entry->second.Attached.insert(bot->GetGUID());
        LOG_INFO("playerbots", "Activity refill: {} joins group {} map {} instance {} as role {}",
            bot->GetName(), assignment.Owner.ToString(), entry->second.Map, entry->second.Instance,
            uint32(assignment.Role));
    }

private:
    void Process(ReplacementEvent const& event)
    {
        if (event.Type == ReplacementEventType::BootVote)
        {
            if (!event.InProgress)
            {
                _votes.erase(event.Group);
                return;
            }
            BootVotes& vote = _votes[event.Group];
            if (vote.Victim != event.Victim)
            {
                vote = {};
                vote.Victim = event.Victim;
            }
            vote.HumanApproved |= event.HumanApproval;
            if (event.BotPending)
                vote.Pending.insert(event.Player);
            return;
        }
        if (event.Type == ReplacementEventType::GroupDisband)
        {
            _dungeons.erase(event.Group);
            _votes.erase(event.Group);
            return;
        }
        if (event.Type == ReplacementEventType::GroupAdd)
        {
            auto entry = _dungeons.find(event.Group);
            Group* group = sGroupMgr->GetGroupByGUID(event.Group.GetCounter());
            if (entry != _dungeons.end() && group && group->IsMember(event.Player) &&
                !entry->second.Attached.erase(event.Player))
                entry->second.Vacancies.Added();
            return;
        }
        if (event.Type == ReplacementEventType::GroupKick)
        {
            Group* group = sGroupMgr->GetGroupByGUID(event.Group.GetCounter());
            Player* client = FindDungeonClient(group);
            if (!client)
                return;
            DungeonReplacement& entry = _dungeons[event.Group];
            if (entry.Map != client->GetMapId() || entry.Instance != client->GetInstanceId())
                entry = {};
            entry.Map = client->GetMapId();
            entry.Instance = client->GetInstanceId();
            entry.Level = client->GetLevel();
            entry.Team = client->GetTeamId();
            entry.Excluded.insert(event.Player);
            entry.Vacancies.Removed(event.Time);
            LOG_INFO("playerbots", "Activity refill: group {} removed {}, replacement waits 30 seconds",
                event.Group.ToString(), event.Player.ToString());
            return;
        }
        auto playerKey = std::make_pair(event.Instance, event.Player);
        TeamId team = event.Team;
        if (event.Type == ReplacementEventType::BattlegroundLeave)
            if (auto entry = _battlegroundTeams.find(playerKey); entry != _battlegroundTeams.end())
            {
                team = entry->second;
                _battlegroundTeams.erase(entry);
            }
        BattlegroundKey key{event.Instance, team};
        if (event.Type == ReplacementEventType::BattlegroundAdd)
        {
            _battlegroundTeams[playerKey] = team;
            if (auto entry = _battlegrounds.find(key); entry != _battlegrounds.end())
                entry->second.Vacancies.Added();
            return;
        }
        Battleground* battleground = sBattlegroundMgr->GetBattleground(event.Instance, event.BattlegroundType);
        if (!HasBattlegroundClient(battleground))
            return;
        BattlegroundReplacement& entry = _battlegrounds[key];
        entry.Type = event.BattlegroundType;
        entry.Queue = BattlegroundMgr::BGQueueTypeId(event.BattlegroundType, 0);
        entry.Bracket = battleground->GetBracketId();
        entry.Excluded.insert(event.Player);
        entry.Vacancies.Removed(event.Time);
        LOG_INFO("playerbots", "Activity refill: battleground {} team {} lost {}, replacement waits 30 seconds",
            event.Instance, uint32(team), event.Player.ToString());
    }

    void Prune()
    {
        for (auto itr = _dungeons.begin(); itr != _dungeons.end();)
        {
            Group* group = sGroupMgr->GetGroupByGUID(itr->first.GetCounter());
            if (!group || (group->isLFGGroup() &&
                sLFGMgr->GetState(itr->first) == lfg::LFG_STATE_FINISHED_DUNGEON) ||
                (!FindDungeonClient(group, &itr->second) && !HasTransferringClient(group)))
                itr = _dungeons.erase(itr);
            else
            {
                if (group->IsFull())
                    itr->second.Vacancies.Clear();
                ++itr;
            }
        }
        for (auto itr = _battlegrounds.begin(); itr != _battlegrounds.end();)
        {
            Battleground* battleground = sBattlegroundMgr->GetBattleground(itr->first.first, itr->second.Type);
            if (!HasBattlegroundClient(battleground))
                itr = _battlegrounds.erase(itr);
            else
            {
                if (battleground->GetPlayersCountByTeam(itr->first.second) >= battleground->GetMaxPlayersPerTeam())
                    itr->second.Vacancies.Clear();
                ++itr;
            }
        }
    }

    std::mutex _mutex;
    mutable std::recursive_mutex _stateMutex;
    std::vector<ReplacementEvent> _events;
    std::map<ObjectGuid, DungeonReplacement> _dungeons;
    std::map<BattlegroundKey, BattlegroundReplacement> _battlegrounds;
    std::map<ObjectGuid, BootVotes> _votes;
    std::map<std::pair<uint32, ObjectGuid>, TeamId> _battlegroundTeams;
    TaskScheduler _scheduler;
};

ActivityReplacementMgr& Replacements()
{
    static ActivityReplacementMgr manager;
    return manager;
}

class ActivityReplacementGroupScript : public GroupScript
{
public:
    ActivityReplacementGroupScript() : GroupScript("ActivityReplacementGroupScript",
        {GROUPHOOK_ON_ADD_MEMBER, GROUPHOOK_ON_REMOVE_MEMBER, GROUPHOOK_ON_DISBAND}) {}

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (Enabled())
            Replacements().Queue({ReplacementEventType::GroupAdd, group->GetGUID(), guid});
    }

    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method, ObjectGuid, char const*) override
    {
        if (Enabled() && !group->isBGGroup() &&
            (method == GROUP_REMOVEMETHOD_KICK || method == GROUP_REMOVEMETHOD_KICK_LFG))
            Replacements().Queue({ReplacementEventType::GroupKick, group->GetGUID(), guid});
    }

    void OnDisband(Group* group) override
    {
        if (Enabled())
            Replacements().Queue({ReplacementEventType::GroupDisband, group->GetGUID()});
    }
};

class ActivityReplacementBattlegroundScript : public BGScript
{
public:
    ActivityReplacementBattlegroundScript() : BGScript("ActivityReplacementBattlegroundScript",
        {ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_ADD_PLAYER,
            ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_REMOVE_PLAYER_AT_LEAVE}) {}

    void OnBattlegroundAddPlayer(Battleground* battleground, Player* player) override
    {
        Record(ReplacementEventType::BattlegroundAdd, battleground, player);
    }

    void OnBattlegroundRemovePlayerAtLeave(Battleground* battleground, Player* player) override
    {
        Record(ReplacementEventType::BattlegroundLeave, battleground, player);
    }

private:
    void Record(ReplacementEventType type, Battleground* battleground, Player* player)
    {
        if (!Enabled() || battleground->isArena())
            return;
        ReplacementEvent event{type};
        event.Player = player->GetGUID();
        event.Instance = battleground->GetInstanceID();
        event.BattlegroundType = battleground->GetBgTypeID();
        event.Team = player->GetBgTeamId();
        Replacements().Queue(std::move(event));
    }
};
}

void ObserveActivityKickVote(WorldSession* session, WorldPacket const& packet)
{
    if (!Enabled() || packet.GetOpcode() != SMSG_LFG_BOOT_PROPOSAL_UPDATE || packet.size() < 3 + sizeof(uint64))
        return;
    Player* player = session ? session->GetPlayer() : nullptr;
    Group* group = player ? player->GetGroup() : nullptr;
    if (!group || !group->isLFGGroup())
        return;
    WorldPacket update(packet);
    update.rpos(0);
    uint8 inProgress = 0;
    uint8 didVote = 0;
    uint8 agree = 0;
    ReplacementEvent event{ReplacementEventType::BootVote, group->GetGUID(), player->GetGUID()};
    update >> inProgress >> didVote >> agree >> event.Victim;
    event.InProgress = inProgress;
    event.HumanApproval = IsClientControlledInstancePlayer(player) && didVote && agree;
    event.BotPending = session->IsHeadless() && GET_PLAYERBOT_AI(player) && !didVote &&
        event.Victim != event.Player;
    Replacements().Queue(std::move(event));
}

void UpdateActivityReplacements(uint32 diff) { Replacements().Update(diff); }
std::vector<ActivityReplacementRequest> GetDungeonReplacementRequests() { return Replacements().Requests(); }
bool HasPendingDungeonReplacement(ObjectGuid group) { return Replacements().Pending(group); }
uint32 GetBattlegroundReplacementHold(uint32 queue, uint32 bracket, TeamId team)
{
    return Replacements().Hold(queue, bracket, team);
}
bool IsActivityReplacementExcluded(BotPopulationAssignment const& assignment, ObjectGuid bot)
{
    return Replacements().Excluded(assignment, bot);
}
void AttachDungeonReplacement(Player* bot, BotPopulationAssignment const& assignment)
{
    Replacements().Attach(bot, assignment);
}
void AddActivityReplacementScripts()
{
    new ActivityReplacementGroupScript();
    new ActivityReplacementBattlegroundScript();
}
