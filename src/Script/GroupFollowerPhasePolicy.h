/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GROUPFOLLOWERPHASEPOLICY_H
#define PLAYERBOTS_GROUPFOLLOWERPHASEPOLICY_H

#include <cstdint>
#include <optional>

struct GroupFollowerPhaseState
{
    uint64_t Leader = 0;
    uint32_t Original = 0;
    uint32_t Applied = 0;

    void Adopt(uint64_t leader, uint32_t current, uint32_t desired)
    {
        if (!Leader || current != Applied)
            Original = current;
        Leader = leader;
        Applied = desired;
    }

    std::optional<uint32_t> OriginalFor(uint64_t leader, uint32_t current) const
    {
        if (Leader == leader && current == Applied)
            return Original;
        return std::nullopt;
    }
};

#endif
