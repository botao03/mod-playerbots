/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYREPLACEMENTPOLICY_H
#define PLAYERBOTS_ACTIVITYREPLACEMENTPOLICY_H

#include <algorithm>
#include <cstdint>
#include <deque>

class ActivityVacancies
{
public:
    static constexpr int64_t DELAY_MILLISECONDS = 30000;

    void Removed(int64_t now) { _deadlines.push_back(now + DELAY_MILLISECONDS); }

    void Added()
    {
        if (!_deadlines.empty())
            _deadlines.pop_front();
    }

    uint32_t Ready(int64_t now, uint32_t freeSlots) const
    {
        return std::min(freeSlots, uint32_t(std::count_if(_deadlines.begin(), _deadlines.end(),
            [now](int64_t deadline) { return deadline <= now; })));
    }

    uint32_t Waiting(int64_t now) const
    {
        return std::count_if(_deadlines.begin(), _deadlines.end(),
            [now](int64_t deadline) { return deadline > now; });
    }

    void Clear() { _deadlines.clear(); }
    bool Empty() const { return _deadlines.empty(); }

private:
    std::deque<int64_t> _deadlines;
};

#endif
