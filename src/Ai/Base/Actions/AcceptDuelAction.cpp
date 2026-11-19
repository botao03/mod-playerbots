/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcceptDuelAction.h"

#include "Event.h"
#include "LastMovementValue.h"
#include "MotionMaster.h"
#include "Playerbots.h"

bool AcceptDuelAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());

    ObjectGuid flagGuid;
    p >> flagGuid;
    ObjectGuid playerGuid;
    p >> playerGuid;

    // A rejected request may still be queued after its duel has been cleared.
    if (!bot->duel || bot->GetGuidValue(PLAYER_DUEL_ARBITER) != flagGuid ||
        bot->duel->Initiator->GetGUID() != playerGuid)
        return false;

    // Do not auto duel with low health
    if ((!botAI->HasGameClientMaster() ||
        (botAI->GetMaster() && botAI->GetMaster()->GetGUID() != playerGuid)) &&
        AI_VALUE2(uint8, "health", "self target") < 90)
    {
        WorldPacket packet(CMSG_DUEL_CANCELLED, 8);
        packet << flagGuid;
        bot->GetSession()->HandleDuelCancelledOpcode(packet);
        return false;
    }

    WorldPacket packet(CMSG_DUEL_ACCEPTED, 8);
    packet << flagGuid;
    bot->GetSession()->HandleDuelAcceptedOpcode(packet);

    if (!IsAcceptedDuel(bot))
        return false;

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    AI_VALUE(LastMovement&, "last movement").clear();
    if (bot->IsMounted() && !bot->IsFlying())
    {
        WorldPacket cancelMount;
        bot->GetSession()->HandleCancelMountAuraOpcode(cancelMount);
    }
    botAI->ResetStrategies();
    return true;
}
