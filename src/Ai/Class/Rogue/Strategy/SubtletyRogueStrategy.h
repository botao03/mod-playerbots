/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SUBTLETYROGUESTRATEGY_H
#define PLAYERBOTS_SUBTLETYROGUESTRATEGY_H

#include "AssassinationRogueStrategy.h"

class SubtletyRogueStrategy : public AssassinationRogueStrategy
{
public:
    SubtletyRogueStrategy(PlayerbotAI* botAI) : AssassinationRogueStrategy(botAI) {}
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "subtlety"; }
};

#endif
