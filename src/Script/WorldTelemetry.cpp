/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldTelemetry.h"
#include "BotPopulation.h"
#include "Config.h"
#include "DataMap.h"
#include "Engine.h"
#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "Playerbots.h"
#include "WorldSession.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace
{
constexpr uint32 SampleInterval = 5000;
constexpr uint32 SampleExpiry = 30;
constexpr char const* SampleTimerKey = "Playerbots.WorldTelemetry.Timer";

struct SampleTimer final : DataMap::Base
{
    uint32 Remaining = 0;
};

struct Sample
{
    std::time_t Timestamp;
    std::string Row;
};

std::string snapshotPath;
std::mutex sampleMutex;
std::unordered_map<ObjectGuid, Sample> samples;
uint32 writeRemaining = SampleInterval;
bool writeErrorReported = false;

std::string Activity(Player* player, PlayerbotAI* ai)
{
    if (!player->IsAlive())
        return "dead";
    if (player->IsInCombat())
        return "combat";
    if (player->IsInFlight())
        return "flight";
    if (player->InBattleground())
        return "battleground";
    if (ai)
    {
        switch (ai->rpgInfo.GetStatus())
        {
            case RPG_DO_QUEST:
                return "quest";
            case RPG_GO_GRIND:
                return "grind";
            case RPG_GO_CAMP:
            case RPG_TRAVEL_FLIGHT:
                return "travel";
            case RPG_WANDER_NPC:
            case RPG_WANDER_RANDOM:
                return "patrol";
            case RPG_REST:
                return "rest";
            case RPG_OUTDOOR_PVP:
                return "outdoor_pvp";
            case RPG_DO_GATHER:
                return "gather";
            default:
                break;
        }
    }
    return player->isMoving() ? "travel" : "idle";
}
}

void InitializeWorldTelemetry()
{
    snapshotPath = sConfigMgr->GetOption<std::string>("Playerbots.WorldSnapshotPath", "");
    if (!snapshotPath.empty() && !std::filesystem::path(snapshotPath).is_absolute())
    {
        LOG_ERROR("playerbots", "Playerbots.WorldSnapshotPath must be absolute; telemetry disabled");
        snapshotPath.clear();
    }
}

void SampleWorldTelemetry(Player* player, uint32 diff)
{
    if (snapshotPath.empty() || !player || !player->IsInWorld() || !player->GetSession() ||
        player->GetSession()->IsLoggingOut())
        return;

    auto* timer = player->CustomData.Get<SampleTimer>(SampleTimerKey);
    if (!timer)
    {
        timer = new SampleTimer();
        player->CustomData.Set(SampleTimerKey, timer);
    }
    if (timer->Remaining > diff)
    {
        timer->Remaining -= diff;
        return;
    }
    timer->Remaining = SampleInterval;

    if (BotPopulationState const* population = GetBotPopulation(player))
        if (!population->Ready || player->GetPhaseMask() != PHASEMASK_NORMAL)
        {
            RemoveWorldTelemetry(player);
            return;
        }

    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    Group* group = player->GetGroup();
    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::ostringstream row;
    row.imbue(std::locale::classic());
    row << now << ',' << player->GetGUID().GetCounter() << ',' << std::quoted(player->GetName(), '"', '"')
        << ',' << uint32(player->getRace()) << ',' << uint32(player->getClass()) << ',' << uint32(player->GetLevel())
        << ',' << player->GetMapId() << ',' << player->GetZoneId() << ',' << player->GetAreaId()
        << ',' << std::fixed << std::setprecision(2) << player->GetPositionX() << ',' << player->GetPositionY()
        << ',' << player->GetPositionZ() << ',' << (ai ? 1 : 0)
        << ',' << (player->GetSession()->IsHeadless() ? 0 : 1)
        << ',' << (group ? group->GetGUID().GetCounter() : 0) << ',' << Activity(player, ai) << ',';
    std::string action = ai && ai->GetCurrentEngine() ? ai->GetCurrentEngine()->GetLastAction() : "";
    row << std::quoted(action, '"', '"') << '\n';

    // Only copied data crosses from map threads to the world-thread writer.
    std::lock_guard<std::mutex> lock(sampleMutex);
    samples.insert_or_assign(player->GetGUID(), Sample{now, row.str()});
}

void RemoveWorldTelemetry(Player* player)
{
    if (snapshotPath.empty() || !player)
        return;

    std::lock_guard<std::mutex> lock(sampleMutex);
    samples.erase(player->GetGUID());
}

void UpdateWorldTelemetry(uint32 diff)
{
    if (snapshotPath.empty())
        return;
    if (writeRemaining > diff)
    {
        writeRemaining -= diff;
        return;
    }
    writeRemaining = SampleInterval;

    std::ostringstream content;
    content << "sampled_at,guid,name,race,class,level,map,zone,area,x,y,z,is_bot,is_player,group_id,activity,action\n";
    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    {
        std::lock_guard<std::mutex> lock(sampleMutex);
        for (auto it = samples.begin(); it != samples.end();)
        {
            if (now - it->second.Timestamp > SampleExpiry)
                it = samples.erase(it);
            else
            {
                content << it->second.Row;
                ++it;
            }
        }
    }

    std::string temporaryPath = snapshotPath + ".tmp";
    std::ofstream file(temporaryPath, std::ios::trunc);
    file << content.str();
    file.close();
    std::error_code error;
    if (file)
        std::filesystem::rename(temporaryPath, snapshotPath, error);
    if (!file || error)
    {
        if (!writeErrorReported)
            LOG_ERROR("playerbots", "Unable to write world telemetry snapshot to {}", snapshotPath);
        writeErrorReported = true;
        return;
    }
    writeErrorReported = false;
}
