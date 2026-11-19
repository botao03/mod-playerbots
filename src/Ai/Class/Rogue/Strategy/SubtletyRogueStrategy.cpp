/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SubtletyRogueStrategy.h"

#include "Playerbots.h"
#include "PvpCombatTriggers.h"

void SubtletyRogueStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    AssassinationRogueStrategy::InitTriggers(triggers);

    triggers.push_back(
        new TriggerNode(PvpTriggerName(PvpSituation::MeleePressure),
                        {NextAction("pvp target spell::pvp melee pressure|eviscerate", ACTION_HIGH + 6),
                         NextAction("backstab", ACTION_HIGH + 2), NextAction("hemorrhage", ACTION_HIGH + 1),
                         NextAction("sinister strike", ACTION_HIGH)}));
}
