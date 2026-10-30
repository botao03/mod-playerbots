/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityReplacementPolicy.h"
#include <cassert>

int main()
{
    ActivityVacancies vacancies;
    assert(vacancies.Empty());
    vacancies.Added();
    assert(vacancies.Empty());

    vacancies.Removed(1000);
    assert(vacancies.Ready(30999, 4) == 0);
    assert(vacancies.Waiting(30999) == 1);
    assert(vacancies.Ready(31000, 4) == 1);
    assert(vacancies.Waiting(31000) == 0);
    assert(vacancies.Ready(31000, 0) == 0);

    // A real player can fill the vacancy during its waiting period.
    vacancies.Added();
    assert(vacancies.Empty());
    assert(vacancies.Ready(60000, 4) == 0);

    // Separate removals retain their own deadlines instead of resetting the first one.
    vacancies.Removed(100000);
    vacancies.Removed(110000);
    assert(vacancies.Waiting(129999) == 2);
    assert(vacancies.Ready(130000, 4) == 1);
    assert(vacancies.Waiting(130000) == 1);
    vacancies.Added();
    assert(vacancies.Ready(130001, 4) == 0);
    assert(vacancies.Ready(140000, 4) == 1);

    vacancies.Removed(110001);
    assert(vacancies.Ready(140001, 1) == 1);
    assert(vacancies.Ready(140001, 4) == 2);
    vacancies.Clear();
    assert(vacancies.Empty());
    assert(vacancies.Ready(200000, 4) == 0);
    assert(vacancies.Waiting(200000) == 0);
}
