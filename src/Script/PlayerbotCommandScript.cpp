/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BattleGroundTactics.h"
#include "Chat.h"
#include "GuildTaskMgr.h"
#include "Log.h"
#include "PerfMonitor.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include <sstream>

using namespace Acore::ChatCommands;

class playerbots_commandscript : public CommandScript
{
public:
    playerbots_commandscript() : CommandScript("playerbots_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable playerbotsDebugCommandTable = {
            {"bg", HandleDebugBGCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsAccountCommandTable = {
            {"setKey", HandleSetSecurityKeyCommand, SEC_MODERATOR, Console::No},
            {"link", HandleLinkAccountCommand, SEC_MODERATOR, Console::No},
            {"linkedAccounts", HandleViewLinkedAccountsCommand, SEC_MODERATOR, Console::No},
            {"unlink", HandleUnlinkAccountCommand, SEC_MODERATOR, Console::No},
        };

        static ChatCommandTable playerbotsCommandTable = {
            {"bot", HandlePlayerbotCommand, SEC_MODERATOR, Console::No},
            {"gtask", HandleGuildTaskCommand, SEC_GAMEMASTER, Console::Yes},
            {"pmon", HandlePerfMonCommand, SEC_GAMEMASTER, Console::Yes},
            {"rndbot", HandleRandomPlayerbotCommand, SEC_GAMEMASTER, Console::Yes},
            {"debug", playerbotsDebugCommandTable},
            {"account", playerbotsAccountCommandTable},
        };

        static ChatCommandTable commandTable = {
            {"playerbots", playerbotsCommandTable},
            {"pvpequip", HandlePvpEquipCommand, SEC_GAMEMASTER, Console::No},
            {"pveequip", HandlePveEquipCommand, SEC_GAMEMASTER, Console::No},
        };

        return commandTable;
    }

    static bool HandleEquipCommand(ChatHandler* handler, char const* args, bool pvp)
    {
        int32 talentTree = 0;
        if (args && *args)
        {
            std::istringstream arguments(args);
            std::string extra;
            if (!(arguments >> talentTree) || talentTree < 1 || talentTree > 3 || (arguments >> extra))
            {
                handler->SendSysMessage("用法：.pvpequip [1-3] 或 .pveequip [1-3]，数字表示天赋树序号。");
                return false;
            }
        }
        Player* target = handler->GetPlayer()->GetSelectedPlayer();
        if (!target || !target->IsInWorld() || !target->GetSession())
        {
            handler->SendSysMessage("请先选中一个在线玩家角色。");
            return false;
        }
        if (handler->HasLowerSecurity(target))
            return false;
        if (!target->IsAlive() || target->IsInCombat() || target->GetTradeData() || target->IsInFlight() ||
            target->IsBeingTeleported() || target->GetSession()->IsLoggingOut() || target->InBattleground() ||
            target->GetVehicle())
        {
            handler->SendSysMessage("目标必须存活、脱离战斗，且不能正在交易、飞行、传送、退出，"
                                    "或处于战场和载具中。");
            return false;
        }

        std::string result;
        PlayerbotFactory factory(target, target->GetLevel());
        if (!factory.ResetLoadout(pvp, result, talentTree))
        {
            handler->SendSysMessage(result);
            return false;
        }
        LOG_INFO("playerbots", "GM {} reset loadout of {} ({}) using {}",
            handler->GetPlayer()->GetName(), target->GetName(), target->GetGUID().ToString(), result);
        handler->PSendSysMessage("{}：已应用 {}，旧装备、背包、银行和邮箱已清空，"
                                 "金币设为 10000 金，武器、骑术、急救、钓鱼和烹饪已补至可学上限，"
                                 "已补齐职业任务技能、训练师技能、种族陆地坐骑、阵营飞行坐骑和天赋雕文。",
            target->GetName(), result);
        return true;
    }

    static bool HandlePvpEquipCommand(ChatHandler* handler, char const* args)
    {
        return HandleEquipCommand(handler, args, true);
    }

    static bool HandlePveEquipCommand(ChatHandler* handler, char const* args)
    {
        return HandleEquipCommand(handler, args, false);
    }

    static bool HandlePlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return PlayerbotMgr::HandlePlayerbotMgrCommand(handler, args);
    }

    static bool HandleRandomPlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(handler, args);
    }

    static bool HandleGuildTaskCommand(ChatHandler* handler, char const* args)
    {
        return GuildTaskMgr::HandleConsoleCommand(handler, args);
    }

    static bool HandlePerfMonCommand(ChatHandler* /*handler*/, char const* args)
    {
        if (!strcmp(args, "reset"))
        {
            sPerfMonitor.Reset();
            return true;
        }

        if (!strcmp(args, "tick"))
        {
            sPerfMonitor.PrintStats(true, false);
            sPerfMonitor.DumpJson(true);
            return true;
        }

        if (!strcmp(args, "stack"))
        {
            sPerfMonitor.PrintStats(false, true);
            sPerfMonitor.DumpJson(false);
            return true;
        }

        if (!strcmp(args, "toggle"))
        {
            sPlayerbotAIConfig.PerfMonEnabled = !sPlayerbotAIConfig.PerfMonEnabled;
            if (sPlayerbotAIConfig.PerfMonEnabled)
                LOG_INFO("playerbots", "Performance monitor enabled");
            else
                LOG_INFO("playerbots", "Performance monitor disabled");
            return true;
        }

        sPerfMonitor.PrintStats();
        sPerfMonitor.DumpJson(false);
        return true;
    }

    static bool HandleDebugBGCommand(ChatHandler* handler, char const* args)
    {
        return BGTactics::HandleConsoleCommand(handler, args);
    }

    static bool HandleSetSecurityKeyCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->PSendSysMessage("Usage: .playerbots account setKey <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();
        std::string key = args;

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleSetSecurityKeyCommand(player, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleLinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        char* key = strtok(nullptr, " ");

        if (!accountName || !key)
        {
            handler->PSendSysMessage("Usage: .playerbots account link <accountName> <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleLinkAccountCommand(player, accountName, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleViewLinkedAccountsCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleViewLinkedAccountsCommand(player);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleUnlinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        if (!accountName)
        {
            handler->PSendSysMessage("Usage: .playerbots account unlink <accountName>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleUnlinkAccountCommand(player, accountName);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }
};

void AddPlayerbotsCommandscripts() { new playerbots_commandscript(); }
