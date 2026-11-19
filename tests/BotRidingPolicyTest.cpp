/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <cassert>

#include "BotRidingPolicy.h"

int main()
{
    assert(BotRidingSkillForLevel(1) == 0);
    assert(BotRidingSkillForLevel(19) == 0);
    assert(BotRidingSkillForLevel(20) == 75);
    assert(BotRidingSkillForLevel(39) == 75);
    assert(BotRidingSkillForLevel(40) == 150);
    assert(BotRidingSkillForLevel(58) == 150);
    assert(BotRidingSkillForLevel(59) == 150);
    assert(BotRidingSkillForLevel(60) == 225);
    assert(BotRidingSkillForLevel(69) == 225);
    assert(BotRidingSkillForLevel(70) == 300);
    assert(BotRidingSkillForLevel(80) == 300);

    using Action = BotFlightFollowAction;
    BotFlightFollowState follower;
    assert(follower.Update(107, 2, false, false, false) == Action::Follow);
    assert(follower.Update(107, 2, false, true, false) == Action::Follow);
    assert(follower.Update(107, 2, true, true, false) == Action::Wait);
    assert(follower.Update(107, 2, true, true, false) == Action::Wait);
    assert(follower.Update(107, 2, false, true, false) == Action::Wait);
    assert(follower.Update(107, 2, false, false, false) == Action::Return);
    assert(follower.Update(107, 2, true, true, false) == Action::Wait);
    follower.Clear();
    assert(follower.Update(107, 2, false, false, false) == Action::Follow);

    assert(follower.Update(107, 2, true, true, true) == Action::Follow);
    assert(follower.Update(107, 2, true, true, false) == Action::Wait);
    assert(follower.Update(107, 2, true, true, true) == Action::Follow);
    assert(!follower.Master);

    assert(follower.Update(107, 2, true, true, false) == Action::Wait);
    assert(follower.Update(108, 2, false, false, false) == Action::Follow);
    assert(follower.Update(107, 2, true, true, false) == Action::Wait);
    assert(follower.Update(107, 3, false, false, false) == Action::Follow);
    assert(follower.Update(107, 2, true, true, false) == Action::Wait);
    assert(follower.Update(0, 0, false, false, false) == Action::Follow);
    assert(!follower.Master && !follower.Group);
}
