/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <cassert>

#include "GroupFollowerPhasePolicy.h"

int main()
{
    GroupFollowerPhaseState follower;
    follower.Adopt(107, 1, 2);
    assert(follower.OriginalFor(107, 2) == 1);
    assert(!follower.OriginalFor(107, 4));
    assert(!follower.OriginalFor(108, 2));

    follower.Adopt(107, 2, 3);
    assert(follower.OriginalFor(107, 3) == 1);
    follower.Adopt(107, 3, 3);
    assert(follower.OriginalFor(107, 3) == 1);

    follower.Adopt(108, 3, 8);
    assert(!follower.OriginalFor(107, 8));
    assert(follower.OriginalFor(108, 8) == 1);

    follower.Adopt(108, 4, 8);
    assert(follower.OriginalFor(108, 8) == 4);
    assert(!follower.OriginalFor(108, 16));
    assert(!follower.OriginalFor(108, 1u << 30));
}
