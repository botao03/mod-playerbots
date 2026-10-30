/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PVPCOMBATACTIONS_H
#define PLAYERBOTS_PVPCOMBATACTIONS_H

#include "GenericSpellActions.h"
#include "PvpCombatTriggers.h"

bool ShouldDelayPvpDefensiveSpell(PlayerbotAI* botAI, std::string const& spell);

class PvpSpellAction : public CastSpellAction, public Qualified
{
public:
    PvpSpellAction(PlayerbotAI* botAI, bool selfTarget)
        : CastSpellAction(botAI, selfTarget ? "pvp self spell" : "pvp target spell"), _selfTarget(selfTarget)
    {
    }

    void Qualify(std::string const value) override;
    std::string const getName() override { return name + "::" + qualifier; }
    Unit* GetTarget() override;
    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;
    bool isUsefulWithoutControl() override { return _selfTarget; }
    bool ShouldReactionInterruptCast() const override { return _interruptCast; }
    bool ShouldReactionInterruptMovement() const override { return _interruptMovement; }

private:
    bool _selfTarget;
    bool _interruptCast = true;
    bool _interruptMovement = false;
    std::string _triggerName;
};

class PvpUseItemAction : public Action
{
public:
    PvpUseItemAction(PlayerbotAI* botAI, bool grenade)
        : Action(botAI, grenade ? "pvp grenade" : "pvp trinket"), _grenade(grenade)
    {
    }

    bool isUseful() override;
    bool isPossible() override;
    bool Execute(Event event) override;
    bool isUsefulWithoutControl() override { return !_grenade; }
    bool ShouldReactionInterruptCast() const override { return true; }

private:
    bool SelectItem(Item* item);
    bool _grenade;
    ObjectGuid _itemGuid;
    uint32 _spellId = 0;
};

#endif
