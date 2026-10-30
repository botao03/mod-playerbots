/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PVPCOMBATTRIGGERS_H
#define PLAYERBOTS_PVPCOMBATTRIGGERS_H

#include "Trigger.h"

class Spell;

enum class PvpSituation
{
    Controlled,
    Feared,
    FearOrSap,
    Rooted,
    Slowed,
    Stunned,
    Hurt,
    Critical,
    IncomingMagic,
    EnemyCasting,
    EnemyClose,
    Chase,
    Burst,
    LowPower,
    Afflicted,
    MagicDebuff
};

char const* PvpTriggerName(PvpSituation situation);
Unit* GetPvpOpponent(PlayerbotAI* botAI);
Spell* GetIncomingMagicSpell(Unit* caster, Player* bot);
bool IsPvpSituationActive(PlayerbotAI* botAI, PvpSituation situation);
void RegisterPvpCombatTriggers(NamedObjectFactory<Trigger>& factory);

class PvpCombatTrigger : public Trigger
{
public:
    PvpCombatTrigger(PlayerbotAI* botAI, PvpSituation situation)
        : Trigger(botAI, PvpTriggerName(situation)), _situation(situation)
    {
    }
    bool IsActive() override;

private:
    PvpSituation _situation;
};

#endif
