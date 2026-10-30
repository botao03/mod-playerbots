/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EmptyInstanceCleanup.h"
#include "AllMapScript.h"
#include "BattleGroundTactics.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "DBCStores.h"
#include "Group.h"
#include "InstanceExitCleanup.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "SpellAuras.h"
#include "TaskScheduler.h"
#include "UnitScript.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <vector>

bool IsClientControlledInstancePlayer(Player* player)
{
    return player && player->GetSession() && !player->GetSession()->IsSocketClosed() &&
        !player->GetSession()->IsLoggingOut() && !player->IsSpectator() && !player->IsGMSpectator();
}

namespace
{
constexpr uint32 SPELL_BATTLEGROUND_DESERTER = 26013;
constexpr std::chrono::seconds SCAN_INTERVAL{5};
using InstanceKey = std::pair<uint32, uint32>;

bool IsCleanupBot(Player* player)
{
    return player && player->GetSession() && GET_PLAYERBOT_AI(player) &&
        player->GetSession()->IsSocketClosed();
}

bool IsInInstance(Player* player, InstanceKey const& key)
{
    return player->FindMap() && player->GetMapId() == key.first && player->GetInstanceId() == key.second;
}

bool HasMembersInAnotherInstance(Group const* group, InstanceKey const& key)
{
    for (auto const& member : group->GetMemberSlots())
        if (Player* player = ObjectAccessor::FindConnectedPlayer(member.guid))
            if (!IsInInstance(player, key) && (player->IsBeingTeleported() || (player->FindMap() &&
                (player->GetMap()->IsDungeon() || player->GetMap()->IsBattlegroundOrArena()))))
                return true;
    return false;
}

void ClearExitPenalties(Player* player)
{
    player->RemoveAurasDueToSpell(SPELL_BATTLEGROUND_DESERTER);
    player->RemoveAurasDueToSpell(lfg::LFG_SPELL_DUNGEON_DESERTER);
    player->RemoveAurasDueToSpell(lfg::LFG_SPELL_DUNGEON_COOLDOWN);
}

struct InstanceSnapshot
{
    InstanceKey Key;
    bool Battleground = false;
    bool HasPlayer = false;
    std::vector<ObjectGuid> Bots;
};

struct InstanceDebuffs final : DataMap::Base
{
    static constexpr char const* Key = "Playerbots.InstanceDebuffs";
    std::map<std::pair<uint32, ObjectGuid>, InstanceKey> Auras;
};

struct PendingExit
{
    InstanceKey Key;
    std::set<std::pair<uint32, ObjectGuid>> NegativeAuras;
    bool LoggedFailure = false;
};

class EmptyInstanceCleanup
{
public:
    EmptyInstanceCleanup()
    {
        _scheduler.Schedule(SCAN_INTERVAL, [this](TaskContext context)
        {
            FinishExits();
            if (sPlayerbotAIConfig.Enabled && sPlayerbotAIConfig.CleanupEmptyInstances)
                ScanInstances();
            else
                _emptySince.clear();
            context.Repeat();
        });
    }

    void Update(uint32 diff)
    {
        if (!sPlayerbotAIConfig.Enabled || !sPlayerbotAIConfig.CleanupEmptyInstances)
        {
            _emptySince.clear();
            if (_pending.empty())
                return;
        }
        _scheduler.Update(diff);
    }

    void ResetGrace(InstanceKey const& key)
    {
        std::lock_guard<std::mutex> lock(_entryMutex);
        _entries.insert(key);
    }

private:
    bool HasIncomingClient(InstanceSnapshot const& snapshot)
    {
        for (auto const& [account, session] : sWorldSessionMgr->GetAllSessions())
        {
            Player* player = session->GetPlayer();
            if (!IsClientControlledInstancePlayer(player))
                continue;
            if (snapshot.Battleground && player->IsInvitedForBattlegroundInstance(snapshot.Key.second))
                return true;
            // The teleport destination has no public instance id; defer conservatively for this map id.
            if (player->IsBeingTeleportedFar() && player->GetTeleportDest().GetMapId() == snapshot.Key.first)
                return true;
        }
        return false;
    }

    void ScanInstances()
    {
        {
            std::lock_guard<std::mutex> lock(_entryMutex);
            for (InstanceKey const& key : _entries)
                _emptySince.erase(key);
            _entries.clear();
        }
        std::vector<InstanceSnapshot> snapshots;
        sMapMgr->DoForAllMaps([&](Map* map)
        {
            if (!map->GetInstanceId() || (!map->IsDungeon() && !map->IsBattlegroundOrArena()))
                return;
            InstanceSnapshot snapshot;
            snapshot.Key = {map->GetId(), map->GetInstanceId()};
            snapshot.Battleground = map->IsBattlegroundOrArena();
            for (auto const& reference : map->GetPlayers())
            {
                Player* player = reference.GetSource();
                if (IsClientControlledInstancePlayer(player))
                    snapshot.HasPlayer = true;
                else if (IsCleanupBot(player))
                    snapshot.Bots.push_back(player->GetGUID());
            }
            snapshots.push_back(std::move(snapshot));
        });

        auto now = std::chrono::steady_clock::now();
        std::set<InstanceKey> present;
        for (InstanceSnapshot const& snapshot : snapshots)
        {
            present.insert(snapshot.Key);
            if (snapshot.HasPlayer || snapshot.Bots.empty() || HasIncomingClient(snapshot))
            {
                _emptySince.erase(snapshot.Key);
                continue;
            }
            auto [itr, inserted] = _emptySince.try_emplace(snapshot.Key, now);
            if (inserted || now - itr->second < std::chrono::seconds(sPlayerbotAIConfig.EmptyInstanceGraceSeconds))
                continue;
            bool newBots = false;
            for (ObjectGuid guid : snapshot.Bots)
                if (!_pending.count(guid))
                    newBots = true;
            if (newBots)
                Evacuate(snapshot);
        }
        for (auto itr = _emptySince.begin(); itr != _emptySince.end();)
            if (!present.count(itr->first))
                itr = _emptySince.erase(itr);
            else
                ++itr;
    }

    void CaptureDebuffs(Player* player, PendingExit& pending)
    {
        if (!IsInInstance(player, pending.Key))
            return;
        if (InstanceDebuffs* tracked = player->CustomData.Get<InstanceDebuffs>(InstanceDebuffs::Key))
            for (auto const& [aura, key] : tracked->Auras)
            {
                AuraApplication const* application = player->GetAuraApplication(aura.first, aura.second);
                if (key == pending.Key && application && !application->IsPositive())
                    pending.NegativeAuras.insert(aura);
            }
        for (auto const& [spell, application] : player->GetAppliedAuras())
        {
            if (application->IsPositive())
                continue;
            Aura* aura = application->GetBase();
            Unit* caster = aura->GetCaster();
            if (caster && caster != player && caster->FindMap() == player->FindMap())
                pending.NegativeAuras.emplace(spell, aura->GetCasterGUID());
        }
        for (auto const& [spell, caster] : pending.NegativeAuras)
            player->RemoveAurasDueToSpell(spell, caster);
    }

    void Prepare(Player* player, InstanceKey const& key)
    {
        auto [itr, inserted] = _pending.try_emplace(player->GetGUID());
        if (!inserted)
            return;
        itr->second.Key = key;
        player->CustomData.Set(InstanceExitCleanupState::Key, new InstanceExitCleanupState());
        ClearExitPenalties(player);
        CaptureDebuffs(player, itr->second);
    }

    void CancelInvitations(InstanceSnapshot const& snapshot)
    {
        std::vector<ObjectGuid> invited;
        {
            std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
            for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                if (IsCleanupBot(player) && !IsInInstance(player, snapshot.Key) &&
                    player->IsInvitedForBattlegroundInstance(snapshot.Key.second))
                    invited.push_back(guid);
        }
        for (ObjectGuid guid : invited)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!IsCleanupBot(player))
                continue;
            for (uint8 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
            {
                BattlegroundQueueTypeId type = player->GetBattlegroundQueueTypeId(slot);
                if (type == BATTLEGROUND_QUEUE_NONE)
                    continue;
                GroupQueueInfo info;
                BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(type);
                if (queue.GetPlayerGroupInfoData(guid, &info) &&
                    info.IsInvitedToBGInstanceGUID == snapshot.Key.second)
                {
                    queue.RemovePlayer(guid, true);
                    player->RemoveBattlegroundQueueId(type);
                }
            }
        }
    }

    void Evacuate(InstanceSnapshot const& snapshot)
    {
        Map* map = sMapMgr->FindMap(snapshot.Key.first, snapshot.Key.second);
        if (!map)
            return;
        // Recheck immediately before changing the match, not just when the timer started.
        for (auto const& reference : map->GetPlayers())
            if (IsClientControlledInstancePlayer(reference.GetSource()))
            {
                _emptySince.erase(snapshot.Key);
                return;
            }

        std::set<ObjectGuid> bots(snapshot.Bots.begin(), snapshot.Bots.end());
        if (!snapshot.Battleground)
            for (ObjectGuid guid : snapshot.Bots)
                if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                    if (Group* group = player->GetGroup())
                        for (auto const& member : group->GetMemberSlots())
                            if (Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid))
                                if (IsCleanupBot(bot) && bot->FindMap() &&
                                    (IsInInstance(bot, snapshot.Key) || (!bot->IsBeingTeleported() &&
                                        !bot->GetMap()->IsDungeon() && !bot->GetMap()->IsBattlegroundOrArena())))
                                    bots.insert(member.guid);

        for (ObjectGuid guid : bots)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                Prepare(player, snapshot.Key);

        if (snapshot.Battleground)
        {
            if (Battleground* bg = map->ToBattlegroundMap()->GetBG())
            {
                // Cancellation must not award victory, honor or arena rating.
                bg->SetStatus(STATUS_WAIT_LEAVE);
                bg->SetEndTime(0);
                bg->RemoveFromBGFreeSlotQueue();
                bgStrategies.erase(snapshot.Key.second);
            }
            CancelInvitations(snapshot);
        }
        for (ObjectGuid guid : bots)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!player)
                continue;
            if (!snapshot.Battleground)
            {
                sLFGMgr->LeaveLfg(guid);
                sLFGMgr->LeaveAllLfgQueues(guid, false);
                // Removing one member can disband the whole group, including members in another instance.
                if (Group* group = player->GetGroup())
                    if (!HasMembersInAnotherInstance(group, snapshot.Key))
                        group->RemoveMember(guid);
            }
            Exit(player, _pending.at(guid));
        }
        LOG_INFO("playerbots", "Evacuating empty {} map {} instance {}: {} bots, grace {} seconds",
            snapshot.Battleground ? "battleground" : "dungeon", snapshot.Key.first, snapshot.Key.second,
            bots.size(), sPlayerbotAIConfig.EmptyInstanceGraceSeconds);
    }

    void Exit(Player* player, PendingExit& pending)
    {
        if (player->IsBeingTeleported() || !player->IsInWorld() || !IsInInstance(player, pending.Key))
            return;
        CaptureDebuffs(player, pending);
        player->CombatStopWithPets(true);
        player->InterruptNonMeleeSpells(true);
        player->ExitVehicle();
        player->GetMotionMaster()->Clear();
        player->StopMoving();
        if (!player->IsAlive())
        {
            player->ResurrectPlayer(1.0f);
            player->SpawnCorpseBones();
        }
        CaptureDebuffs(player, pending);
        MapEntry const* entry = sMapStore.LookupEntry(player->GetEntryPoint().GetMapId());
        if (entry && !entry->IsDungeon() && !entry->IsBattlegroundOrArena() && player->TeleportToEntryPoint())
            return;
        entry = sMapStore.LookupEntry(player->m_homebindMapId);
        if (entry && !entry->IsDungeon() && !entry->IsBattlegroundOrArena() &&
            player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY,
                player->m_homebindZ, 0.0f))
            return;
        WorldLocation start = player->GetStartPosition();
        entry = sMapStore.LookupEntry(start.GetMapId());
        if (entry && !entry->IsDungeon() && !entry->IsBattlegroundOrArena() &&
            player->TeleportTo(start.GetMapId(), start.GetPositionX(), start.GetPositionY(),
                start.GetPositionZ(), start.GetOrientation()))
            return;
        if (!pending.LoggedFailure)
        {
            pending.LoggedFailure = true;
            LOG_ERROR("playerbots", "Empty-instance evacuation could not teleport bot {}; will retry",
                player->GetGUID().ToString());
        }
    }

    void FinishExits()
    {
        for (auto itr = _pending.begin(); itr != _pending.end();)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(itr->first);
            if (!player || !player->CustomData.Get<InstanceExitCleanupState>(InstanceExitCleanupState::Key))
            {
                itr = _pending.erase(itr);
                continue;
            }
            // SelfBots normally wait for a client ACK, but a disconnected client cannot send one.
            if (IsCleanupBot(player) && IsSelfBot(player) && player->IsBeingTeleportedFar())
            {
                player->GetSession()->HandleMoveWorldportAck();
                player = ObjectAccessor::FindConnectedPlayer(itr->first);
                if (!player)
                {
                    itr = _pending.erase(itr);
                    continue;
                }
            }
            if (!IsCleanupBot(player) && !player->IsBeingTeleported())
            {
                ClearExitPenalties(player);
                player->CustomData.Erase(InstanceExitCleanupState::Key);
                _emptySince.erase(itr->second.Key);
                itr = _pending.erase(itr);
                continue;
            }
            if (IsInInstance(player, itr->second.Key))
            {
                Exit(player, itr->second);
                ++itr;
                continue;
            }
            if (player->IsBeingTeleported() || !player->IsInWorld())
            {
                ++itr;
                continue;
            }
            ClearExitPenalties(player);
            for (auto const& [spell, caster] : itr->second.NegativeAuras)
                player->RemoveAurasDueToSpell(spell, caster);
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
            {
                if (sRandomPlayerbotMgr.IsRandomBot(player))
                    ai->SetMaster(nullptr);
                ai->Reset();
                AiObjectContext* context = ai->GetAiObjectContext();
                context->GetValue<uint32>("bg type")->Set(0);
                context->GetValue<uint32>("bg role")->Set(0);
                context->GetValue<uint32>("arena type")->Set(0);
                context->GetValue<PositionMap&>("position")->Get()["bg objective"].Reset();
                ai->ResetStrategies();
            }
            player->CustomData.Erase(InstanceExitCleanupState::Key);
            player->SaveToDB(false, false);
            LOG_INFO("playerbots", "Empty-instance evacuation finished for bot {}", player->GetGUID().ToString());
            itr = _pending.erase(itr);
        }
    }

    TaskScheduler _scheduler;
    std::mutex _entryMutex;
    std::set<InstanceKey> _entries;
    std::map<InstanceKey, std::chrono::steady_clock::time_point> _emptySince;
    std::map<ObjectGuid, PendingExit> _pending;
};

EmptyInstanceCleanup& GetCleanup()
{
    static EmptyInstanceCleanup cleanup;
    return cleanup;
}

class EmptyInstanceDebuffScript : public UnitScript
{
public:
    EmptyInstanceDebuffScript() : UnitScript("EmptyInstanceDebuffScript", true,
        {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE}) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!sPlayerbotAIConfig.Enabled || !sPlayerbotAIConfig.CleanupEmptyInstances)
            return;
        Player* player = unit->ToPlayer();
        if (!player || !GET_PLAYERBOT_AI(player) || !player->FindMap() ||
            (!player->GetMap()->IsDungeon() && !player->GetMap()->IsBattlegroundOrArena()))
            return;
        AuraApplication const* application = aura->GetApplicationOfTarget(player->GetGUID());
        if (!application || application->IsPositive() || aura->GetCasterGUID().IsEmpty())
            return;
        Unit* caster = aura->GetCaster();
        if (caster && caster->FindMap() != player->FindMap())
            return;
        InstanceDebuffs* tracked = player->CustomData.GetDefault<InstanceDebuffs>(InstanceDebuffs::Key);
        tracked->Auras[{aura->GetId(), aura->GetCasterGUID()}] = {player->GetMapId(), player->GetInstanceId()};
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode /*mode*/) override
    {
        if (Player* player = unit->ToPlayer())
            if (InstanceDebuffs* tracked = player->CustomData.Get<InstanceDebuffs>(InstanceDebuffs::Key))
            {
                Aura* aura = application->GetBase();
                tracked->Auras.erase({aura->GetId(), aura->GetCasterGUID()});
            }
    }
};

class EmptyInstanceCleanupMapScript : public AllMapScript
{
public:
    EmptyInstanceCleanupMapScript() : AllMapScript("EmptyInstanceCleanupMapScript",
        {ALLMAPHOOK_ON_PLAYER_ENTER_ALL, ALLMAPHOOK_ON_DESTROY_INSTANCE}) { }

    void OnPlayerEnterAll(Map* map, Player* player) override
    {
        if (sPlayerbotAIConfig.Enabled && sPlayerbotAIConfig.CleanupEmptyInstances &&
            map->GetInstanceId() && IsClientControlledInstancePlayer(player))
            GetCleanup().ResetGrace({map->GetId(), map->GetInstanceId()});
    }

    void OnDestroyInstance(MapInstanced* /*base*/, Map* map) override
    {
        if (sPlayerbotAIConfig.Enabled && sPlayerbotAIConfig.CleanupEmptyInstances)
            GetCleanup().ResetGrace({map->GetId(), map->GetInstanceId()});
    }
};
}

void UpdateEmptyInstanceCleanup(uint32 diff)
{
    GetCleanup().Update(diff);
}

void AddEmptyInstanceCleanupScripts()
{
    new EmptyInstanceCleanupMapScript();
    new EmptyInstanceDebuffScript();
}
