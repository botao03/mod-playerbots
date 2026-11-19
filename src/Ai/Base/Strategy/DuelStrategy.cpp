/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DuelStrategy.h"

#include "AiFactory.h"
#include "Playerbots.h"
#include "PvpCombatActions.h"

namespace
{
class DuelPreparationMultiplier : public Multiplier
{
public:
    DuelPreparationMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "duel preparation") {}
    float GetValue(Action* action) override
    {
        if (!action || !GetPvpDuelOpponent(botAI) ||
            (bot->duel->State == DUEL_STATE_IN_PROGRESS &&
             (botAI->GetState() == BOT_STATE_COMBAT || action->IsReaction())))
            return 1.0f;

        std::string const name = action->getName();
        return name == "prepare pvp duel" || name == "attack duel opponent" ||
                       name.starts_with("pvp self spell::pvp duel preparation|")
                   ? 1.0f
                   : 0.0f;
    }
};
}  // namespace

void DuelStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    if (sPlayerbotAIConfig.PvpCombatTactics)
        multipliers.push_back(new DuelPreparationMultiplier(botAI));
}

void DuelStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    PassThroughStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode("duel requested", { NextAction("accept duel", relevance) }));
    if (!sPlayerbotAIConfig.PvpCombatTactics)
    {
        triggers.push_back(new TriggerNode("no attackers", {NextAction("attack duel opponent", 70.0f)}));
        return;
    }

    triggers.push_back(new TriggerNode(PvpTriggerName(PvpSituation::DuelStart),
                                       {NextAction("attack duel opponent", ACTION_HIGH + 5)}));
    triggers.push_back(new TriggerNode(PvpTriggerName(PvpSituation::DuelPreparation),
                                       {NextAction("prepare pvp duel", ACTION_EMERGENCY)}));
    auto prepare = [&](char const* spell, float priority = ACTION_HIGH)
    {
        if (botAI->HasSpell(spell))
            triggers.push_back(
                new TriggerNode(PvpTriggerName(PvpSituation::DuelPreparation),
                                {NextAction(std::string("pvp self spell::pvp duel preparation|") + spell, priority)}));
    };

    Player* bot = botAI->GetBot();
    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            prepare("battle shout");
            break;
        case CLASS_PALADIN:
            prepare("sacred shield");
            break;
        case CLASS_HUNTER:
            prepare("trueshot aura");
            break;
        case CLASS_ROGUE:
            prepare("stealth", ACTION_MOVE + 5);
            break;
        case CLASS_PRIEST:
            prepare("power word: shield");
            break;
        case CLASS_DEATH_KNIGHT:
            prepare("horn of winter");
            break;
        case CLASS_SHAMAN:
            prepare(AiFactory::GetPlayerSpecTab(bot) == SHAMAN_TAB_RESTORATION ? "water shield" : "lightning shield");
            break;
        case CLASS_MAGE:
            prepare("ice barrier");
            break;
        case CLASS_WARLOCK:
            prepare(botAI->HasSpell("fel armor") ? "fel armor" : "demon armor");
            break;
        case CLASS_DRUID:
            if (AiFactory::GetPlayerSpecTab(bot) == DRUID_TAB_FERAL && PlayerbotAI::IsDps(bot))
            {
                prepare("cat form", ACTION_MOVE + 5);
                prepare("prowl", ACTION_MOVE + 6);
            }
            else
                prepare("mark of the wild");
            break;
        default:
            break;
    }
}

DuelStrategy::DuelStrategy(PlayerbotAI* botAI) : PassThroughStrategy(botAI) {}

void StartDuelStrategy::InitTriggers(std::vector<TriggerNode*>& /*triggers*/) {}

StartDuelStrategy::StartDuelStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
