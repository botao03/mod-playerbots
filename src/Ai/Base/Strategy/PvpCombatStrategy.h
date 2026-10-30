/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PVPCOMBATSTRATEGY_H
#define PLAYERBOTS_PVPCOMBATSTRATEGY_H

#include "PvpCombatTriggers.h"
#include "Strategy.h"

class PvpCombatStrategy : public Strategy
{
public:
    PvpCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "pvp combat"; }
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitReactionTriggers(std::vector<TriggerNode*>& triggers) override;

private:
    void AddSpell(std::vector<TriggerNode*>& triggers, PvpSituation situation, char const* spell, float relevance,
                  bool selfTarget = true);
};

#endif
