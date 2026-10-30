/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpValues.h"
#include "BattlegroundEY.h"
#include "BattlegroundMgr.h"
#include "BattlegroundWS.h"
#include "BattlegroundSA.h"
#include "BattleGroundTactics.h"
#include "PositionValue.h"
#include "Spell.h"
#include <map>
#include "Playerbots.h"
#include "ServerFacade.h"

namespace
{
// a game from the random queue records BATTLEGROUND_RB as the player's type: use the rolled map
BattlegroundTypeId RealBgType(Player* bot)
{
    BattlegroundTypeId bgType = bot->GetBattlegroundTypeId();
    if (bgType == BATTLEGROUND_RB && bot->GetBattleground())
        bgType = bot->GetBattleground()->GetBgTypeID(true);
    return bgType;
}
}  // namespace

Unit* FlagCarrierValue::Calculate()
{
    Unit* carrier = nullptr;

    if (botAI->GetBot()->InBattleground())
    {
        if (RealBgType(botAI->GetBot()) == BattlegroundTypeId::BATTLEGROUND_WS)
        {
            BattlegroundWS* bg = (BattlegroundWS*)botAI->GetBot()->GetBattleground();

            if (!bg)
                return nullptr;

            if (((!sameTeam && bot->GetTeamId() == TEAM_HORDE) || (sameTeam && bot->GetTeamId() == TEAM_ALLIANCE)) &&
                !bg->GetFlagPickerGUID(TEAM_HORDE).IsEmpty())
                carrier = ObjectAccessor::GetPlayer(bg->GetBgMap(), bg->GetFlagPickerGUID(TEAM_HORDE));

            if (((!sameTeam && bot->GetTeamId() == TEAM_ALLIANCE) || (sameTeam && bot->GetTeamId() == TEAM_HORDE)) &&
                !bg->GetFlagPickerGUID(TEAM_ALLIANCE).IsEmpty())
                carrier = ObjectAccessor::GetPlayer(bg->GetBgMap(), bg->GetFlagPickerGUID(TEAM_ALLIANCE));

            if (carrier)
            {
                if (ignoreRange || bot->IsWithinDistInMap(carrier, sPlayerbotAIConfig.SightDistance))
                {
                    return carrier;
                }
                else
                    return nullptr;
            }
        }

        if (RealBgType(botAI->GetBot()) == BATTLEGROUND_EY)
        {
            BattlegroundEY* bg = (BattlegroundEY*)botAI->GetBot()->GetBattleground();

            if (!bg)
                return nullptr;

            if (bg->GetFlagPickerGUID().IsEmpty())
                return nullptr;

            Player* fc = ObjectAccessor::GetPlayer(bg->GetBgMap(), bg->GetFlagPickerGUID());
            if (!fc)
                return nullptr;

            if (!sameTeam && (fc->GetTeamId() != bot->GetTeamId()))
                carrier = fc;

            if (sameTeam && (fc->GetTeamId() == bot->GetTeamId()))
                carrier = fc;

            if (carrier)
            {
                if (ignoreRange || bot->IsWithinDistInMap(carrier, sPlayerbotAIConfig.SightDistance))
                {
                    return carrier;
                }
                else
                    return nullptr;
            }
        }
    }

    return carrier;
}

GuidVector BattlegroundProtectedControlValue::Calculate()
{
    GuidVector protectedEnemies;
    if (!sPlayerbotAIConfig.CoordinatedBattlegrounds || !bot->InBattleground() || bot->InArena())
        return protectedEnemies;
    for (ObjectGuid guid : AI_VALUE(GuidVector, "nearest enemy players"))
        if (Unit* enemy = botAI->GetUnit(guid))
            if (enemy->IsAlive() && bot->IsWithinDistInMap(enemy, 60.0f) &&
                enemy->HasBreakableByDamageCrowdControlAura())
                protectedEnemies.push_back(guid);
    return protectedEnemies;
}

ObjectGuid BattlegroundTargetValue::Calculate()
{
    Battleground* bg = bot->GetBattleground();
    if (!sPlayerbotAIConfig.CoordinatedBattlegrounds || !bg || bg->isArena() ||
        bg->GetStatus() != STATUS_IN_PROGRESS || !bot->IsAlive() || bot->GetVehicle())
        return ObjectGuid::Empty;

    std::map<ObjectGuid, uint32> assists;
    for (ObjectGuid guid : AI_VALUE(GuidVector, "closest friendly players"))
        if (Unit* friendly = botAI->GetUnit(guid))
            if (friendly != bot && friendly->IsAlive() && bot->IsWithinDistInMap(friendly, 60.0f))
                if (Unit* victim = friendly->GetVictim())
                    ++assists[victim->GetGUID()];

    Unit* enemyCarrier = AI_VALUE(Unit*, "enemy flag carrier");
    Unit* teamCarrier = AI_VALUE(Unit*, "team flag carrier");
    PositionInfo objective = AI_VALUE(PositionMap&, "position")["bg objective"];
    ObjectGuid best;
    float bestScore = 0.0f;
    for (ObjectGuid guid : AI_VALUE(GuidVector, "nearest enemy players"))
    {
        Unit* enemy = botAI->GetUnit(guid);
        if (!enemy || !enemy->IsAlive() || !bot->IsValidAttackTarget(enemy) ||
            !bot->CanSeeOrDetect(enemy) || !bot->IsWithinLOSInMap(enemy) ||
            !bot->IsWithinDistInMap(enemy, _control ? 30.0f : 55.0f) ||
            enemy->HasBreakableByDamageCrowdControlAura())
            continue;
        bool capturing = false;
        for (CurrentSpellTypes type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
            if (Spell* spell = enemy->GetCurrentSpell(type))
                capturing = capturing || spell->m_spellInfo->Id == SPELL_CAPTURE_BANNER;
        bool pursuer = teamCarrier && enemy->GetVictim() == teamCarrier;
        bool healer = enemy->IsPlayer() && PlayerbotAI::IsHeal(enemy->ToPlayer(), true);
        bool nearObjective = objective.isSet() && enemy->GetDistance2d(objective.x, objective.y) < 35.0f;
        if (_control && !capturing && !pursuer && enemy != enemyCarrier && !healer && !nearObjective)
            continue;
        float score = capturing ? 3000.0f : enemy == enemyCarrier ? 2000.0f : pursuer ? 1500.0f : 100.0f;
        score += std::min(assists[guid], 5u) * 120.0f + (100.0f - enemy->GetHealthPct()) * 2.0f;
        if (healer)
            score += _control ? 800.0f : 80.0f;
        if (nearObjective)
            score += 150.0f;
        score -= bot->GetDistance2d(enemy);
        if (score > bestScore || (score == bestScore && guid < best))
        {
            best = guid;
            bestScore = score;
        }
    }
    if (!_control && RealBgType(bot) == BATTLEGROUND_SA)
    {
        auto* strand = static_cast<BattlegroundSA*>(bg);
        if (strand->GetAttackerTeam() != bot->GetTeamId())
            for (uint32 index = BG_SA_DEMOLISHER_1; index <= BG_SA_DEMOLISHER_8; ++index)
                if (Creature* vehicle = bg->GetBgMap()->GetCreature(bg->BgCreatures[index]))
                    if (vehicle->IsAlive() && bot->IsValidAttackTarget(vehicle) &&
                        bot->IsWithinDistInMap(vehicle, 45.0f) && bot->IsWithinLOSInMap(vehicle))
                    {
                        float score = 2500.0f - vehicle->GetHealthPct() - bot->GetDistance2d(vehicle);
                        if (score > bestScore)
                        {
                            best = vehicle->GetGUID();
                            bestScore = score;
                        }
                    }
    }
    return best;
}

std::vector<CreatureData const*> BgMastersValue::Calculate()
{
    BattlegroundTypeId bgTypeId = (BattlegroundTypeId)stoi(qualifier);

    std::vector<uint32> entries;
    std::map<TeamId, std::map<BattlegroundTypeId, std::vector<uint32>>> battleMastersCache =
        sRandomPlayerbotMgr.getBattleMastersCache();
    entries.insert(entries.end(), battleMastersCache[TEAM_NEUTRAL][bgTypeId].begin(),
                   battleMastersCache[TEAM_NEUTRAL][bgTypeId].end());
    entries.insert(entries.end(), battleMastersCache[TEAM_ALLIANCE][bgTypeId].begin(),
                   battleMastersCache[TEAM_ALLIANCE][bgTypeId].end());
    entries.insert(entries.end(), battleMastersCache[TEAM_HORDE][bgTypeId].begin(),
                   battleMastersCache[TEAM_HORDE][bgTypeId].end());

    std::vector<CreatureData const*> bmGuids;

    for (auto entry : entries)
    {
        for (auto creaturePair : WorldPosition().getCreaturesNear(0, entry))
        {
            bmGuids.push_back(creaturePair);
        }
    }

    return bmGuids;
}

CreatureData const* BgMasterValue::Calculate()
{
    CreatureData const* bmPair = NearestBm(false);
    if (!bmPair)
        bmPair = NearestBm(true);

    return bmPair;
}

CreatureData const* BgMasterValue::NearestBm(bool allowDead)
{
    WorldPosition botPos(bot);

    std::vector<CreatureData const*> bmPairs = AI_VALUE2(std::vector<CreatureData const*>, "bg masters", qualifier);

    float rDist = 0.0f;
    CreatureData const* rbmPair = nullptr;

    for (auto& bmPair : bmPairs)
    {
        if (!bmPair)
            continue;

        WorldPosition bmPos(bmPair->mapid, bmPair->posX, bmPair->posY, bmPair->posZ, bmPair->orientation);

        float dist = botPos.distance(bmPos);  // This is the aproximate travel distance.

        // Did we already find a closer unit that is not dead?
        if (rbmPair && rDist <= dist)
            continue;

        CreatureTemplate const* bmTemplate = sObjectMgr->GetCreatureTemplate(bmPair->id);
        if (!bmTemplate)
            continue;

        FactionTemplateEntry const* bmFactionEntry = sFactionTemplateStore.LookupEntry(bmTemplate->faction);

        // Is the unit hostile?
        if (Unit::GetFactionReactionTo(bot->GetFactionTemplateEntry(), bmFactionEntry) < REP_NEUTRAL)
            continue;

        AreaTableEntry const* area = bmPos.getArea();

        if (!area)
            continue;

        // Is the area hostile?
        if (area->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
            continue;
        if (area->team == 2 && bot->GetTeamId() == TEAM_HORDE)
            continue;

        if (!allowDead)
        {
            Unit* unit = botAI->GetUnit(bmPair);

            if (!unit)
                continue;

            // Is the unit dead?
            if (unit->getDeathState() == DeathState::Dead)
                continue;
        }

        rbmPair = bmPair;
        rDist = dist;
    }

    return rbmPair;
}

BattlegroundTypeId RpgBgTypeValue::Calculate()
{
    GuidPosition guidPosition = AI_VALUE(GuidPosition, "rpg target");

    if (guidPosition)
        for (uint32 i = 1; i < MAX_BATTLEGROUND_QUEUE_TYPES; i++)
        {
            BattlegroundQueueTypeId queueTypeId = (BattlegroundQueueTypeId)i;

            BattlegroundTypeId bgTypeId = sBattlegroundMgr->BGTemplateId(queueTypeId);

            Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
            if (!bg)
                continue;

            if (bot->GetLevel() < bg->GetMinLevel())
                continue;

            // check if already in queue
            if (bot->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
                continue;

            std::map<TeamId, std::map<BattlegroundTypeId, std::vector<uint32>>> battleMastersCache =
                sRandomPlayerbotMgr.getBattleMastersCache();

            for (auto& entry : battleMastersCache[TEAM_NEUTRAL][bgTypeId])
                if (entry == guidPosition.GetEntry())
                    return bgTypeId;

            for (auto& entry : battleMastersCache[bot->GetTeamId()][bgTypeId])
                if (entry == guidPosition.GetEntry())
                    return bgTypeId;
        }

    return BATTLEGROUND_TYPE_NONE;
}
