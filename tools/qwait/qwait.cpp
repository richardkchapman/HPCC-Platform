/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2020 HPCC Systems®.

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
#include "portlist.h"
#include "jlib.hpp"
#include "daclient.hpp"
#include "wujobq.hpp"

void usage(const char * action = nullptr)
{
    printf("Usage: qwait daliserver=<dali> queue=<qname>\n");
}

int main(int argc, const char *argv[])
{
    InitModuleObjects();
    Owned<IProperties> globals = createProperties(false);
    for (int i = 1; i < argc; i++)
    {
        const char * arg = argv[i];
        if (strchr(arg,'='))
            globals->loadProp(arg);
        else
        {
            usage();
            _exit(4);
        }
    }
    const char *dali = globals->queryProp("dali");
    const char *queue = globals->queryProp("queue");
    if (!dali || !queue)
    {
        usage();
        _exit(4);
    }

    SocketEndpoint ep(dali,DALI_SERVER_PORT);
    SocketEndpointArray epa;
    epa.append(ep);
    Owned<IGroup> group = createIGroup(epa);
    initClientProcess(group, DCR_Monitoring);
    Owned<IJobQueue> jobq= createJobQueue(queue);
    jobq->connect(false);

    while (true)
    {
        Owned<IJobQueueItem> item = jobq->dequeue();
        if (item.get())
        {
            StringAttr wuid;
            wuid.set(item->queryWUID());
            printf("Dequeued workunit request '%s'\n", wuid.get());
        }
    }
    ExitModuleObjects();
}
