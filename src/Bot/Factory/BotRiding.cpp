/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotRiding.h"
#include "BotRidingPolicy.h"
#include "DataMap.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::chrono::seconds RIDING_CHECK_INTERVAL{1};
constexpr uint32 SPELL_COLD_WEATHER_FLYING = 54197;
constexpr uint32 SPELL_GOLDEN_GRYPHON = 32235;
constexpr uint32 SPELL_BLUE_WIND_RIDER = 32244;
constexpr uint32 SPELL_SWIFT_BLUE_GRYPHON = 32242;
constexpr uint32 SPELL_SWIFT_GREEN_WIND_RIDER = 32295;

struct RidingTraining final : DataMap::Base
{
    static constexpr char const* Key = "Playerbots.RidingTraining";
    uint8 Level = 0;
    Clock::time_point NextCheck;
};

struct RacialMounts
{
    uint8 Race;
    uint32 Slow;
    uint32 Fast;
};

constexpr std::array<RacialMounts, 10> GroundMounts = {{{RACE_HUMAN, 458, 23228}, {RACE_ORC, 6654, 23250},
    {RACE_DWARF, 6899, 23238}, {RACE_NIGHTELF, 10789, 23219}, {RACE_UNDEAD_PLAYER, 17463, 17465},
    {RACE_TAUREN, 18990, 23249}, {RACE_GNOME, 10969, 23225}, {RACE_TROLL, 10796, 23241},
    {RACE_DRAENEI, 34406, 35713}, {RACE_BLOODELF, 35022, 35025}}};

std::array<uint32, 4> GetRidingMountSpells(Player const& player, uint16 skill)
{
    bool const alliance = player.GetTeamId(true) == TEAM_ALLIANCE;
    uint8 const race = player.getRace(true);
    RacialMounts const* ground = &GroundMounts[alliance ? 0 : 1];
    for (auto const& mount : GroundMounts)
        if (mount.Race == race)
        {
            ground = &mount;
            break;
        }

    return {{skill >= BOT_RIDING_APPRENTICE ? ground->Slow : 0, skill >= BOT_RIDING_JOURNEYMAN ? ground->Fast : 0,
             skill >= BOT_RIDING_EXPERT ? (alliance ? SPELL_GOLDEN_GRYPHON : SPELL_BLUE_WIND_RIDER) : 0,
             skill >= BOT_RIDING_ARTISAN ? (alliance ? SPELL_SWIFT_BLUE_GRYPHON : SPELL_SWIFT_GREEN_WIND_RIDER) : 0}};
}

bool HasRequiredRiding(Player const* bot)
{
    uint8 const level = bot->GetLevel();
    uint16 const skill = BotRidingSkillForLevel(level);
    if (bot->GetPureSkillValue(SKILL_RIDING) != skill || bot->GetPureMaxSkillValue(SKILL_RIDING) != skill)
        return false;

    for (auto const& rank : BotRidingRanks)
        if ((level >= rank.Level) != bot->HasSpell(rank.Spell))
            return false;

    if ((level >= BotColdWeatherFlyingLevel) != bot->HasSpell(SPELL_COLD_WEATHER_FLYING))
        return false;

    for (uint32 spell : GetRidingMountSpells(*bot, skill))
        if (spell && !bot->HasActiveSpell(spell))
            return false;

    return true;
}

void LearnRidingSpell(Player& player, uint32 spell)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spell);
    if (!spellInfo)
        return;

    // addSpell leaves existing temporary spells and inactive unranked spells unchanged.
    auto const known = player.GetSpellMap().find(spell);
    if (known != player.GetSpellMap().end() &&
        (known->second->State == PLAYERSPELL_TEMPORARY || (!known->second->Active && !spellInfo->IsRanked())))
        player.removeSpell(spell, SPEC_MASK_ALL, false);

    // Existing riding spells can be restricted to a single talent spec.
    if (player.GetSession()->IsHeadless() || player.HasActiveSpell(spell))
        player.addSpell(spell, SPEC_MASK_ALL, true);
    else
        player.learnSpell(spell);
}

void SetRidingSkill(Player& player, uint16 skill, bool matchLevel)
{
    if (!skill && !matchLevel)
        return;

    uint16 const value = matchLevel ? skill : std::max(skill, player.GetPureSkillValue(SKILL_RIDING));
    uint16 const cap = matchLevel ? skill : std::max(value, player.GetPureMaxSkillValue(SKILL_RIDING));
    player.SetSkill(SKILL_RIDING, cap / BOT_RIDING_APPRENTICE, value, cap);
}

uint16 TrainRidingForLevel(Player& player, bool removeUnavailableRanks)
{
    uint8 const level = player.GetLevel();
    uint16 const skill = BotRidingSkillForLevel(level);
    for (auto const& rank : BotRidingRanks)
        if (level >= rank.Level)
            LearnRidingSpell(player, rank.Spell);
        else if (removeUnavailableRanks)
            player.removeSpell(rank.Spell, SPEC_MASK_ALL, false);

    if (level >= BotColdWeatherFlyingLevel)
        LearnRidingSpell(player, SPELL_COLD_WEATHER_FLYING);
    else if (removeUnavailableRanks)
        player.removeSpell(SPELL_COLD_WEATHER_FLYING, SPEC_MASK_ALL, false);

    SetRidingSkill(player, skill, removeUnavailableRanks);
    return skill;
}

void LearnRidingMounts(Player& player, uint16 skill)
{
    for (uint32 spell : GetRidingMountSpells(player, skill))
        if (spell)
            LearnRidingSpell(player, spell);
}
}  // namespace

void InitializeLoadoutRidingSkills(Player& player) { TrainRidingForLevel(player, false); }

void InitializeLoadoutMounts(Player& player)
{
    uint16 const skill = BotRidingSkillForLevel(player.GetLevel());
    // Class trainers can lower the riding cap when teaching spells such as Flight Form.
    SetRidingSkill(player, skill, false);
    LearnRidingMounts(player, skill);
}

void InitializeBotRiding(Player* bot)
{
    if (!sPlayerbotAIConfig.AutoTrainRiding || !bot || !bot->GetSession() || !bot->GetSession()->IsHeadless())
        return;

    LearnRidingMounts(*bot, TrainRidingForLevel(*bot, true));

    auto* training = bot->CustomData.Get<RidingTraining>(RidingTraining::Key);
    if (!training)
    {
        training = new RidingTraining();
        bot->CustomData.Set(RidingTraining::Key, training);
    }
    training->Level = bot->GetLevel();
    training->NextCheck = Clock::now() + RIDING_CHECK_INTERVAL;
    LOG_DEBUG("playerbots", "Riding training: bot {} level {} skill {}/{} cold weather flying {}", bot->GetName(),
              training->Level, bot->GetPureSkillValue(SKILL_RIDING), bot->GetPureMaxSkillValue(SKILL_RIDING),
              bot->HasSpell(SPELL_COLD_WEATHER_FLYING));
}

void UpdateBotRiding(Player* bot)
{
    if (!sPlayerbotAIConfig.AutoTrainRiding || !bot || !bot->GetSession() || !bot->GetSession()->IsHeadless())
        return;
    auto* training = bot->CustomData.Get<RidingTraining>(RidingTraining::Key);
    if (!training || training->Level != bot->GetLevel())
    {
        InitializeBotRiding(bot);
        return;
    }

    Clock::time_point const now = Clock::now();
    if (now < training->NextCheck)
        return;
    training->NextCheck = now + RIDING_CHECK_INTERVAL;
    // Factory resets and spell changes can invalidate training without changing the level.
    if (!HasRequiredRiding(bot))
        InitializeBotRiding(bot);
}

bool HasBotFlightRiding(Player const* bot)
{
    return bot && BotRidingSkillForLevel(bot->GetLevel()) >= BOT_RIDING_EXPERT &&
           bot->GetPureSkillValue(SKILL_RIDING) >= BOT_RIDING_EXPERT;
}

bool IsUsingFlyingMount(Player const* player)
{
    ShapeshiftForm const form = player->GetShapeshiftForm();
    return (player->IsMounted() && player->HasIncreaseMountedFlightSpeedAura()) ||
           form == FORM_FLIGHT || form == FORM_FLIGHT_EPIC;
}

bool IsUsingMountOrTravelForm(Player const* player)
{
    return player->IsMounted() || IsUsingFlyingMount(player) || player->GetShapeshiftForm() == FORM_TRAVEL;
}
