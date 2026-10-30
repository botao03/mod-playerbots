/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcceptInvitationAction.h"
#include "BotPopulation.h"
#include "Event.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSecurity.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "WorldPacket.h"

namespace
{
constexpr uint32 GROUP_INVITATION_PRIORITY = 50;

class AcceptInvitationOperation final : public PlayerbotOperation
{
public:
    AcceptInvitationOperation(ObjectGuid botGuid, ObjectGuid inviterGuid)
        : _botGuid(botGuid), _inviterGuid(inviterGuid) {}

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(_botGuid);
        Player* inviter = ObjectAccessor::FindConnectedPlayer(_inviterGuid);
        if (!bot || !inviter || !bot->GetSession() || !inviter->GetSession() ||
            !bot->IsInWorld() || !inviter->IsInWorld() ||
            bot->IsBeingTeleported() || inviter->IsBeingTeleported() ||
            bot->GetSession()->IsLoggingOut() || inviter->GetSession()->IsSocketClosed() ||
            inviter->GetSession()->IsLoggingOut())
            return false;

        Group* group = bot->GetGroupInvite();
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!group || group->GetLeaderGUID() != _inviterGuid || !botAI)
            return false;

        auto* population = bot->CustomData.Get<BotPopulationState>(BotPopulationState::Key);
        if (population && population->Assignment.Purpose == BotPopulationPurpose::World && !population->Prepared)
            return false;

        if (!botAI->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, false, inviter))
        {
            LOG_INFO("playerbots", "Group invitation declined: bot {} inviter {} (permission check)",
                bot->GetName(), inviter->GetName());
            WorldPacket data(SMSG_GROUP_DECLINE, 10);
            data << bot->GetName();
            inviter->SendDirectMessage(&data);
            bot->UninviteFromGroup();
            return false;
        }

        if (bot->isAFK() && !IsSelfBot(bot))
            bot->ToggleAFK();

        bool const full = group->IsFull();
        WorldPacket packet;
        uint32 rolesMask = 0;
        packet << rolesMask;
        bot->GetSession()->HandleGroupAcceptOpcode(packet);

        if (!bot->GetGroup() || !bot->GetGroup()->IsMember(_inviterGuid))
        {
            LOG_INFO("playerbots", "Group invitation failed: bot {} inviter {} (group full: {})",
                bot->GetName(), inviter->GetName(), full);
            return false;
        }

        if (population && population->Assignment.Purpose == BotPopulationPurpose::World)
        {
            population->Ready = true;
            bot->SetPhaseMask(PHASEMASK_NORMAL, true);
        }
        if (sRandomPlayerbotMgr.IsRandomBot(bot))
            botAI->SetMaster(inviter);

        botAI->ResetStrategies();
        botAI->ChangeStrategy("+follow,-lfg,-bg", BOT_STATE_NON_COMBAT);
        botAI->Reset();
        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));

        LOG_INFO("playerbots", "Group invitation accepted: bot {} inviter {} members {}",
            bot->GetName(), inviter->GetName(), bot->GetGroup()->GetMembersCount());
        if (sPlayerbotAIConfig.SummonWhenGroup && bot->GetDistance(inviter) > sPlayerbotAIConfig.SightDistance)
        {
            SummonAction summon(botAI);
            summon.Teleport(inviter, bot, true);
        }
        return true;
    }

    ObjectGuid GetBotGuid() const override { return _botGuid; }
    uint32 GetPriority() const override { return GROUP_INVITATION_PRIORITY; }
    std::string GetName() const override { return "AcceptInvitation"; }

private:
    ObjectGuid _botGuid;
    ObjectGuid _inviterGuid;
};
}

bool AcceptInvitationAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroupInvite();
    if (!group)
        return false;

    return PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<AcceptInvitationOperation>(bot->GetGUID(), group->GetLeaderGUID()));
}
