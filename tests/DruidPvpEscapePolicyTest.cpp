/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <cassert>
#include <limits>

#include "DruidPvpEscapePolicy.h"

namespace
{
DruidPvpEscapeState PursuingFeral()
{
    DruidPvpEscapeState state;
    state.PlayerControl = true;
    state.Rooted = true;
    state.Melee = true;
    state.EnemyMoving = true;
    state.EnemyDistance = 20.0f;
    state.ControlRemainingMs = 6000;
    return state;
}
}  // namespace

int main()
{
    DruidPvpEscapeState state = PursuingFeral();
    assert(ShouldDruidPvpEscape(state));

    state.EnemyInMeleeRange = true;
    assert(!ShouldDruidPvpEscape(state));
    state = PursuingFeral();
    state.Casting = true;
    assert(!ShouldDruidPvpEscape(state));
    state = PursuingFeral();
    state.EnemyProtected = true;
    assert(!ShouldDruidPvpEscape(state));

    state = PursuingFeral();
    state.ControlRemainingMs = 2000;
    assert(!ShouldDruidPvpEscape(state));
    state.ControlRemainingMs = 2001;
    assert(ShouldDruidPvpEscape(state));
    state.ControlRemainingMs = -1;
    assert(ShouldDruidPvpEscape(state));

    state = PursuingFeral();
    state.SecondsSinceEscape = 7;
    assert(!ShouldDruidPvpEscape(state));
    state.SecondsSinceEscape = 8;
    assert(ShouldDruidPvpEscape(state));
    state.HealthPct = 20.0f;
    state.SecondsSinceEscape = 2;
    assert(!ShouldDruidPvpEscape(state));
    state.SecondsSinceEscape = 3;
    assert(ShouldDruidPvpEscape(state));

    state = PursuingFeral();
    state.Rooted = false;
    state.EnemyMoving = false;
    assert(!ShouldDruidPvpEscape(state));
    state.EnemyMoving = true;
    state.EnemyDistance = 9.0f;
    assert(!ShouldDruidPvpEscape(state));
    state.EnemyDistance = 10.0f;
    state.ControlRemainingMs = 4000;
    assert(!ShouldDruidPvpEscape(state));
    state.ControlRemainingMs = 4001;
    assert(ShouldDruidPvpEscape(state));

    state = PursuingFeral();
    state.Melee = false;
    assert(!ShouldDruidPvpEscape(state));
    state.EnemyDistance = 8.0f;
    state.EnemyPressuring = true;
    state.HealthPct = 45.0f;
    assert(ShouldDruidPvpEscape(state));
    state.Casting = true;
    assert(!ShouldDruidPvpEscape(state));
    state.HealthPct = 20.0f;
    assert(ShouldDruidPvpEscape(state));
    state.EnemyPressuring = false;
    assert(!ShouldDruidPvpEscape(state));
    state.EnemyPressuring = true;
    state.EnemyDistance = 11.0f;
    assert(!ShouldDruidPvpEscape(state));

    for (bool melee : {false, true})
        for (float health : {20.0f, 45.0f, 100.0f})
        {
            state = PursuingFeral();
            state.PlayerControl = false;
            state.Melee = melee;
            state.HealthPct = health;
            state.EnemyDistance = 8.0f;
            state.EnemyPressuring = true;
            assert(!ShouldDruidPvpEscape(state));
        }

    assert(CanAffordDruidPvpEscape(6500, 10000, 2500, 100.0f));
    assert(!CanAffordDruidPvpEscape(6499, 10000, 2500, 100.0f));
    assert(CanAffordDruidPvpEscape(5000, 10000, 2500, 20.0f));
    assert(!CanAffordDruidPvpEscape(4999, 10000, 2500, 20.0f));
    assert(!CanAffordDruidPvpEscape(10000, 10000, 6000, 100.0f));
    assert(CanAffordDruidPvpEscape(1, 10000, 0, 100.0f));
    assert(!CanAffordDruidPvpEscape(100, 10000, 200, 100.0f));
    assert(!CanAffordDruidPvpEscape(0, 0, 0, 100.0f));
    assert(
        CanAffordDruidPvpEscape(std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(), 1, 100.0f));

    // Reapplying a full-duration root every two seconds must not exhaust the mana pool.
    for (float health : {100.0f, 20.0f})
    {
        uint32_t mana = 10000;
        int64_t lastEscape = -100;
        uint32_t escapes = 0;
        for (int64_t elapsed = 0; elapsed <= 60; elapsed += 2)
        {
            state = PursuingFeral();
            state.HealthPct = health;
            state.SecondsSinceEscape = elapsed - lastEscape;
            if (ShouldDruidPvpEscape(state) && CanAffordDruidPvpEscape(mana, 10000, 2500, health))
            {
                mana -= 2500;
                lastEscape = elapsed;
                ++escapes;
            }
        }
        assert(escapes == (health > 25.0f ? 2u : 3u));
        assert(mana >= (health > 25.0f ? 4000u : 2000u));
    }
}
