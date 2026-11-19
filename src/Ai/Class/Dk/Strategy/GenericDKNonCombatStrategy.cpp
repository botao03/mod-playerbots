/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericDKNonCombatStrategy.h"
#include "Playerbots.h"

GenericDKNonCombatStrategy::GenericDKNonCombatStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

void GenericDKNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    constexpr uint32 SPELL_MASTER_OF_GHOULS = 52143;
    if (botAI->GetBot()->HasAura(SPELL_MASTER_OF_GHOULS))
        triggers.push_back(new TriggerNode("no pet", { NextAction("raise dead", 22.0f) }));

    triggers.push_back(new TriggerNode("often", { NextAction("apply stone", 1.0f) }));
    triggers.push_back(
        new TriggerNode("horn of winter", { NextAction("horn of winter", 21.0f) }));
    triggers.push_back(
        new TriggerNode("bone shield", { NextAction("bone shield", 21.0f) }));
}
