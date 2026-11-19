/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STRANDOFTHEANCIENTSACTIONS_H
#define PLAYERBOTS_STRANDOFTHEANCIENTSACTIONS_H

#include "VehicleActions.h"

class BattlegroundSA;
class GameObject;

uint32 GetStrandGate(BattlegroundSA const* battleground, bool right);
Position GetStrandGateApproach(Player* player, GameObject const* gate, float distance, float lateral = 0.0f);
bool IsStrandDockTarget(Player& bot, Unit const& target);

class StrandCaptureAction : public MovementAction
{
public:
    StrandCaptureAction(PlayerbotAI* botAI) : MovementAction(botAI, "strand capture") {}
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    bool Capture(GameObject* object, uint32 spellId, bool canApproach);
};

class StrandPrepareAction : public MovementAction
{
public:
    StrandPrepareAction(PlayerbotAI* botAI) : MovementAction(botAI, "strand prepare") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class StrandVehicleAction : public EnterVehicleAction
{
public:
    StrandVehicleAction(PlayerbotAI* botAI) : EnterVehicleAction(botAI, "strand vehicle") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class StrandDefenseAction : public MovementAction
{
public:
    StrandDefenseAction(PlayerbotAI* botAI) : MovementAction(botAI, "strand regroup") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class StrandBombAction : public MovementAction
{
public:
    StrandBombAction(PlayerbotAI* botAI) : MovementAction(botAI, "strand bomb") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

#endif
