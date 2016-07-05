/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2016 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "platform.h"
#include "jstats.h"
#include "thorstats.hpp"
#include "jdebug.hpp"


//MORE: When do cycles get converted into nanoseconds?
const StatisticsMapping nestedSectionStatistics(StCycleTotalExecuteCycles, StTimeTotalExecute, StNumExecutions);

ThorSectionTimer::ThorSectionTimer(const char * _name, CRuntimeStatistic & _occurences, CRuntimeStatistic & _elapsed)
: occurences(_occurences), elapsed(_elapsed), name(_name)
{

}

ThorSectionTimer * ThorSectionTimer::createTimer(CRuntimeStatisticCollection & stats, const char * name)
{
    StatsScopeId scope(SSTfunction, name);
    CRuntimeStatisticCollection & nested = stats.registerNested(scope, nestedSectionStatistics);
    CRuntimeStatistic & occurences = nested.queryStatistic(StNumExecutions);
    CRuntimeStatistic & elapsed = nested.queryStatistic(StCycleTotalExecuteCycles);
    return new ThorSectionTimer(name, occurences, elapsed);
}

void ThorSectionTimer::noteSectionTime(unsigned __int64 startCycles)
{
    cycle_t delay = get_cycles_now() - startCycles;
    elapsed.addAtomic(delay);
    occurences.addAtomic(1);
}
