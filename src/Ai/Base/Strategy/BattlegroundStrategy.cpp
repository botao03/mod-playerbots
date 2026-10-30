/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BattlegroundStrategy.h"
#include "Playerbots.h"
#include "BattleGroundTactics.h"
#include "AttackAction.h"
#include "Spell.h"

void BGStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("often", { NextAction("bg join", relevance)}));
    if (sPlayerbotAIConfig.PopulationEnabled)
        triggers.push_back(new TriggerNode("population queue ready", {NextAction("bg join", relevance)}));
    triggers.push_back(new TriggerNode("bg invite active", { NextAction("bg status check", relevance)}));
    triggers.push_back(new TriggerNode("timer", { NextAction("bg strategy check", relevance)}));
}

BGStrategy::BGStrategy(PlayerbotAI* botAI) : PassThroughStrategy(botAI) {}

void BattlegroundStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("bg waiting", { NextAction("bg move to start", ACTION_BG)}));
    triggers.push_back(new TriggerNode("bg active", { NextAction("bg move to objective", ACTION_BG)}));
    triggers.push_back(new TriggerNode("often", { NextAction("bg check objective", ACTION_BG + 1)}));
    triggers.push_back(new TriggerNode("dead", { NextAction("bg reset objective force", ACTION_EMERGENCY)}));
}

void WarsongStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("bg active", { NextAction("bg check flag", ACTION_EMERGENCY )}));
    triggers.push_back(new TriggerNode("enemy flagcarrier near", { NextAction("attack enemy flag carrier", ACTION_RAID + 1.0f)}));
    triggers.push_back(new TriggerNode("team flagcarrier near", { NextAction("bg protect fc", ACTION_RAID)}));
    triggers.push_back(new TriggerNode("often", { NextAction("bg use buff", ACTION_BG)}));
    triggers.push_back(new TriggerNode("low health", { NextAction("bg use buff", ACTION_MOVE)}));
    triggers.push_back(new TriggerNode("low mana", { NextAction("bg use buff", ACTION_MOVE)}));
    triggers.push_back(new TriggerNode("player has flag", { NextAction("bg move to objective", ACTION_EMERGENCY)}));
    triggers.push_back(new TriggerNode("timer bg", { NextAction("bg reset objective force", ACTION_EMERGENCY)}));
}

void AlteracStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("timer bg", { NextAction("bg reset objective force", ACTION_EMERGENCY)}));
}

void ArathiStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("bg active", { NextAction("bg check flag", ACTION_EMERGENCY)}));
    triggers.push_back(new TriggerNode("often", { NextAction("bg use buff", ACTION_BG)}));
    triggers.push_back(new TriggerNode("low health", { NextAction("bg use buff", ACTION_MOVE)}));
    triggers.push_back(new TriggerNode("low mana", { NextAction("bg use buff", ACTION_MOVE)}));
}

void EyeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("bg active", { NextAction("bg check flag", ACTION_EMERGENCY)}));
    triggers.push_back(new TriggerNode("often", { NextAction("bg use buff", ACTION_BG)}));
    triggers.push_back(new TriggerNode("low health", { NextAction("bg use buff", ACTION_MOVE)}));
    triggers.push_back(new TriggerNode("low mana", { NextAction("bg use buff", ACTION_MOVE)}));
    triggers.push_back(new TriggerNode("enemy flagcarrier near", { NextAction("attack enemy flag carrier", ACTION_RAID)}));
    triggers.push_back(new TriggerNode("player has flag",{ NextAction("bg move to objective", ACTION_EMERGENCY)}));
}

//TODO: Do Priorities
void IsleStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("bg active", { NextAction("bg check flag", ACTION_MOVE)}));
    triggers.push_back(new TriggerNode("timer", { NextAction("enter vehicle", ACTION_MOVE + 8.0f)}));
    triggers.push_back(new TriggerNode("random", { NextAction("leave vehicle", ACTION_MOVE + 7.0f)}));
    triggers.push_back(new TriggerNode("in vehicle", { NextAction("hurl boulder", ACTION_MOVE + 9.0f)}));
    triggers.push_back(new TriggerNode("in vehicle", { NextAction("fire cannon", ACTION_MOVE + 9.0f)}));
    triggers.push_back(new TriggerNode("in vehicle", { NextAction("napalm", ACTION_MOVE + 9.0f)}));
    triggers.push_back(new TriggerNode("enemy is close", { NextAction("steam blast", ACTION_MOVE + 9.0f)}));
    triggers.push_back(new TriggerNode("in vehicle", { NextAction("ram", ACTION_MOVE + 9.0f)}));
    triggers.push_back(new TriggerNode("enemy is close", { NextAction("ram", ACTION_MOVE + 9.1f)}));
    triggers.push_back(new TriggerNode("enemy out of melee", { NextAction("steam rush", ACTION_MOVE + 9.2f)}));
    triggers.push_back(new TriggerNode("in vehicle", { NextAction("incendiary rocket", ACTION_MOVE + 9.0f)}));
    triggers.push_back(new TriggerNode("in vehicle", { NextAction("rocket blast", ACTION_MOVE + 9.0f)}));
    // this is bugged: it doesn't work, and stops glaive throw working (which is needed to take down gate)
    // triggers.push_back(new TriggerNode("in vehicle", { NextAction("blade salvo", ACTION_MOVE + 9.0f)}));
    triggers.push_back(new TriggerNode("in vehicle", { NextAction("glaive throw", ACTION_MOVE + 9.0f)}));
}

void ArenaStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("no possible targets", { NextAction("arena tactics", ACTION_BG)}));
}

void CoordinatedBattlegroundStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("bg active", {NextAction("bg focus fire", ACTION_RAID + 2)}));
    triggers.push_back(new TriggerNode("bg active", {NextAction("bg crowd control", ACTION_INTERRUPT + 1)}));
    triggers.push_back(new TriggerNode("bg active", {NextAction("bg check flag", ACTION_INTERRUPT + 2)}));
}

void StrandStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("timer", {NextAction("enter vehicle", ACTION_BG + 1)}));
    triggers.push_back(new TriggerNode("bg active", {NextAction("ram", ACTION_NORMAL + 1)}));
    triggers.push_back(new TriggerNode("bg active", {NextAction("hurl boulder", ACTION_NORMAL)}));
}

void CoordinatedBattlegroundStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new BattlegroundCoordinationMultiplier(botAI));
}

float BattlegroundCoordinationMultiplier::GetValue(Action* action)
{
    if (!action || !sPlayerbotAIConfig.CoordinatedBattlegrounds || !bot->InBattleground() || bot->InArena())
        return 1.0f;
    if (bot->GetHealthPct() >= 25.0f)
        for (CurrentSpellTypes type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
            if (Spell* spell = bot->GetCurrentSpell(type))
                if (spell->m_spellInfo->Id == SPELL_CAPTURE_BANNER)
                    return dynamic_cast<BGTactics*>(action) ? 1.0f : 0.0f;
    if (action->getThreatType() == Action::ActionThreatType::Aoe &&
        !AI_VALUE(GuidVector, "bg protected crowd control").empty())
        return 0.0f;
    if (action->getThreatType() != Action::ActionThreatType::None || dynamic_cast<AttackAction*>(action))
        if (Unit* target = action->GetTarget())
            if (bot->IsHostileTo(target) && target->HasBreakableByDamageCrowdControlAura())
                return 0.0f;
    return 1.0f;
}

void CoordinatedBattlegroundStrategy::AppendTargetExclusions(GuidSet& exclusions,
    TargetValueExclusionType /*type*/)
{
    GuidVector protectedEnemies = botAI->GetAiObjectContext()
        ->GetValue<GuidVector>("bg protected crowd control")->Get();
    exclusions.insert(protectedEnemies.begin(), protectedEnemies.end());
}
