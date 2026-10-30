/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpCombatStrategy.h"

#include <string_view>

#include "Playerbots.h"
#include "PvpCombatActions.h"

namespace
{
class PvpCombatMultiplier : public Multiplier
{
public:
    PvpCombatMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "pvp cooldown timing") {}
    float GetValue(Action* action) override
    {
        if (!action || !GetPvpOpponent(botAI))
            return 1.0f;

        if (auto* spellAction = dynamic_cast<CastSpellAction*>(action))
            if (spellAction->GetTarget() == bot && ShouldDelayPvpDefensiveSpell(botAI, spellAction->getSpell()))
                return 0.0f;

        for (std::string_view racial :
             {"arcane torrent", "war stomp", "blood fury", "berserking", "every man for himself",
              "will of the forsaken", "escape artist", "stoneform", "gift of the naaru", "lifeblood"})
            if (action->getName() == racial)
                return 0.0f;
        return 1.0f;
    }
};
}  // namespace

void PvpCombatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    if (sPlayerbotAIConfig.PvpCombatTactics)
        multipliers.push_back(new PvpCombatMultiplier(botAI));
}

void PvpCombatStrategy::AddSpell(std::vector<TriggerNode*>& triggers, PvpSituation situation, char const* spell,
                                 float relevance, bool selfTarget)
{
    if (!botAI->HasSpell(spell) && std::string_view(spell) != "spell lock")
        return;

    std::string trigger = PvpTriggerName(situation);
    std::string action = selfTarget ? "pvp self spell::" : "pvp target spell::";
    triggers.push_back(new TriggerNode(trigger, {NextAction(action + trigger + "|" + spell, relevance)}));
}

void PvpCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    if (!sPlayerbotAIConfig.PvpCombatTactics)
        return;

    AddSpell(triggers, PvpSituation::Burst, "blood fury", ACTION_HIGH + 3);
    AddSpell(triggers, PvpSituation::Burst, "berserking", ACTION_HIGH + 3);
    AddSpell(triggers, PvpSituation::Hurt, "berserking", ACTION_HIGH + 4);
    AddSpell(triggers, PvpSituation::LowPower, "arcane torrent", ACTION_HIGH + 2);

    switch (botAI->GetBot()->getClass())
    {
        case CLASS_WARRIOR:
            AddSpell(triggers, PvpSituation::Chase, "hamstring", ACTION_MOVE + 2, false);
            AddSpell(triggers, PvpSituation::Chase, "intercept", ACTION_MOVE + 1, false);
            break;
        case CLASS_PALADIN:
            AddSpell(triggers, PvpSituation::Chase, "hammer of justice", ACTION_MOVE + 1, false);
            break;
        case CLASS_HUNTER:
            AddSpell(triggers, PvpSituation::EnemyClose, "concussive shot", ACTION_MOVE + 1, false);
            break;
        case CLASS_ROGUE:
            AddSpell(triggers, PvpSituation::Chase, "sprint", ACTION_MOVE + 1);
            break;
        case CLASS_DEATH_KNIGHT:
            AddSpell(triggers, PvpSituation::Chase, "chains of ice", ACTION_MOVE + 2, false);
            AddSpell(triggers, PvpSituation::Chase, "death grip", ACTION_MOVE + 1, false);
            break;
        default:
            break;
    }

    if (sPlayerbotAIConfig.PvpUseEngineeringItems && botAI->GetBot()->HasSkill(SKILL_ENGINEERING))
    {
        triggers.push_back(new TriggerNode("pvp enemy casting", {NextAction("pvp grenade", ACTION_INTERRUPT)}));
        triggers.push_back(new TriggerNode("pvp chase", {NextAction("pvp grenade", ACTION_MOVE)}));
    }
}

void PvpCombatStrategy::InitReactionTriggers(std::vector<TriggerNode*>& triggers)
{
    if (!sPlayerbotAIConfig.PvpCombatTactics)
        return;

    AddSpell(triggers, PvpSituation::Controlled, "every man for himself", ACTION_EMERGENCY + 4);
    AddSpell(triggers, PvpSituation::Feared, "will of the forsaken", ACTION_EMERGENCY + 5);
    AddSpell(triggers, PvpSituation::Slowed, "escape artist", ACTION_EMERGENCY + 5);
    AddSpell(triggers, PvpSituation::Afflicted, "stoneform", ACTION_EMERGENCY + 2);
    AddSpell(triggers, PvpSituation::IncomingMagic, "shadowmeld", ACTION_EMERGENCY + 1);
    AddSpell(triggers, PvpSituation::EnemyCasting, "arcane torrent", ACTION_INTERRUPT + 1);
    AddSpell(triggers, PvpSituation::EnemyCasting, "war stomp", ACTION_INTERRUPT + 1);
    AddSpell(triggers, PvpSituation::EnemyClose, "war stomp", ACTION_INTERRUPT);
    AddSpell(triggers, PvpSituation::Hurt, "gift of the naaru", ACTION_EMERGENCY);
    AddSpell(triggers, PvpSituation::Hurt, "lifeblood", ACTION_EMERGENCY);
    triggers.push_back(new TriggerNode("pvp controlled", {NextAction("pvp trinket", ACTION_EMERGENCY + 1)}));

    switch (botAI->GetBot()->getClass())
    {
        case CLASS_WARRIOR:
            AddSpell(triggers, PvpSituation::FearOrSap, "berserker rage", ACTION_EMERGENCY + 6);
            AddSpell(triggers, PvpSituation::EnemyCasting, "pummel", ACTION_INTERRUPT + 3, false);
            AddSpell(triggers, PvpSituation::EnemyCasting, "shield bash", ACTION_INTERRUPT + 3, false);
            AddSpell(triggers, PvpSituation::IncomingMagic, "spell reflection", ACTION_EMERGENCY + 3);
            AddSpell(triggers, PvpSituation::Hurt, "enraged regeneration", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::Critical, "shield wall", ACTION_EMERGENCY + 3);
            AddSpell(triggers, PvpSituation::Critical, "last stand", ACTION_EMERGENCY + 2);
            break;
        case CLASS_PALADIN:
            AddSpell(triggers, PvpSituation::Slowed, "hand of freedom", ACTION_EMERGENCY + 6);
            AddSpell(triggers, PvpSituation::EnemyCasting, "hammer of justice", ACTION_INTERRUPT + 2, false);
            AddSpell(triggers, PvpSituation::Hurt, "divine protection", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::Critical, "divine shield", ACTION_EMERGENCY + 3);
            break;
        case CLASS_HUNTER:
            AddSpell(triggers, PvpSituation::Slowed, "master's call", ACTION_EMERGENCY + 6);
            AddSpell(triggers, PvpSituation::EnemyCasting, "silencing shot", ACTION_INTERRUPT + 3, false);
            AddSpell(triggers, PvpSituation::Hurt, "deterrence", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::EnemyClose, "disengage", ACTION_MOVE + 3);
            break;
        case CLASS_ROGUE:
            AddSpell(triggers, PvpSituation::EnemyCasting, "kick", ACTION_INTERRUPT + 3, false);
            AddSpell(triggers, PvpSituation::IncomingMagic, "cloak of shadows", ACTION_EMERGENCY + 3);
            AddSpell(triggers, PvpSituation::MagicDebuff, "cloak of shadows", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::EnemyClose, "evasion", ACTION_EMERGENCY);
            break;
        case CLASS_PRIEST:
            AddSpell(triggers, PvpSituation::EnemyCasting, "silence", ACTION_INTERRUPT + 3, false);
            AddSpell(triggers, PvpSituation::Hurt, "pain suppression", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::Hurt, "desperate prayer", ACTION_EMERGENCY + 1);
            AddSpell(triggers, PvpSituation::Critical, "dispersion", ACTION_EMERGENCY + 3);
            break;
        case CLASS_DEATH_KNIGHT:
            AddSpell(triggers, PvpSituation::EnemyCasting, "mind freeze", ACTION_INTERRUPT + 3, false);
            AddSpell(triggers, PvpSituation::EnemyCasting, "strangulate", ACTION_INTERRUPT + 2, false);
            AddSpell(triggers, PvpSituation::IncomingMagic, "anti-magic shell", ACTION_EMERGENCY + 3);
            AddSpell(triggers, PvpSituation::MagicDebuff, "anti-magic shell", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::Stunned, "icebound fortitude", ACTION_EMERGENCY + 6);
            AddSpell(triggers, PvpSituation::Hurt, "icebound fortitude", ACTION_EMERGENCY + 3);
            AddSpell(triggers, PvpSituation::Hurt, "rune tap", ACTION_EMERGENCY + 1);
            AddSpell(triggers, PvpSituation::Critical, "vampiric blood", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::Critical, "death pact", ACTION_EMERGENCY + 1);
            break;
        case CLASS_SHAMAN:
            AddSpell(triggers, PvpSituation::EnemyCasting, "wind shear", ACTION_INTERRUPT + 3, false);
            AddSpell(triggers, PvpSituation::IncomingMagic, "grounding totem", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::Hurt, "shamanistic rage", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::EnemyClose, "thunderstorm", ACTION_MOVE + 3);
            break;
        case CLASS_MAGE:
            AddSpell(triggers, PvpSituation::EnemyCasting, "counterspell", ACTION_INTERRUPT + 3, false);
            AddSpell(triggers, PvpSituation::Rooted, "blink", ACTION_EMERGENCY + 6);
            AddSpell(triggers, PvpSituation::Stunned, "blink", ACTION_EMERGENCY + 6);
            AddSpell(triggers, PvpSituation::Critical, "ice block", ACTION_EMERGENCY + 3);
            AddSpell(triggers, PvpSituation::EnemyClose, "frost nova", ACTION_MOVE + 2);
            break;
        case CLASS_WARLOCK:
            AddSpell(triggers, PvpSituation::EnemyCasting, "spell lock", ACTION_INTERRUPT + 3, false);
            AddSpell(triggers, PvpSituation::EnemyCasting, "shadowfury", ACTION_INTERRUPT + 2, false);
            AddSpell(triggers, PvpSituation::Hurt, "death coil", ACTION_EMERGENCY + 2, false);
            break;
        case CLASS_DRUID:
            triggers.push_back(new TriggerNode("pvp slowed", {NextAction("pvp break root", ACTION_EMERGENCY + 6)}));
            AddSpell(triggers, PvpSituation::Stunned, "barkskin", ACTION_EMERGENCY + 3);
            AddSpell(triggers, PvpSituation::Hurt, "barkskin", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::Critical, "survival instincts", ACTION_EMERGENCY + 3);
            AddSpell(triggers, PvpSituation::Critical, "frenzied regeneration", ACTION_EMERGENCY + 2);
            AddSpell(triggers, PvpSituation::EnemyCasting, "feral charge - bear", ACTION_INTERRUPT + 3, false);
            AddSpell(triggers, PvpSituation::EnemyCasting, "bash", ACTION_INTERRUPT + 2, false);
            AddSpell(triggers, PvpSituation::EnemyCasting, "maim", ACTION_INTERRUPT + 2, false);
            break;
        default:
            break;
    }
}
