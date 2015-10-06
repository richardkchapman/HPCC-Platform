/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2015 HPCC Systems.

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

#ifndef TRACESLAVE_HPP
#define TRACESLAVE_HPP

#include "slave.hpp"
#include "jqueue.tpp"
#include "jthread.hpp"
#include "limits.h"


class CTracingThorDataLink :  implements CInterfaceOf<IThorDataLink>
{
private:
    class TraceQueue
    {
    public:
        OwnedConstThorRow *rowBuffer;
        unsigned rowHeadp;
        TraceQueue()
        {
            rowBuffer = NULL;
            rowHeadp = 0;
        }
        ~TraceQueue()
        {
            delete [] rowBuffer;
        }
        void init()
        {
            if (!rowBuffer)
            {
                rowBuffer = new OwnedConstThorRow[traceRowBufferSize];
                rowHeadp = 0;
            }
        }
        void enqueue(const void *row)
        {
            // NOTE - updates then sets rowHeadp (atomically) so that the code
            // below (ignoring oldest record) is safe
            rowBuffer[rowHeadp].set(row);
            unsigned nextHead = rowHeadp + 1;
            if (nextHead==traceRowBufferSize)
                nextHead = 0;
            rowHeadp = nextHead;
        }
        void dump(MemoryBuffer &mb, IHThorArg *helper)
        {
            unsigned rowPos = rowHeadp;
            // NOTE - we don't output every item in the list as one
            // COULD have been updated after we swapped the buffers
            for (unsigned n=1; n<traceRowBufferSize;++n)
            {
                OwnedConstThorRow row = rowBuffer[rowPos].getClear(); // This will need changing to move to a third buffer or something...

                if (++rowPos==traceRowBufferSize) rowPos = 0;
                if (!row) continue;

                CommonXmlWriter xmlwrite(XWFnoindent);
                helper->queryOutputMeta()->toXML((const byte *) row.get(), xmlwrite);
                VStringBuffer trace("<Row>%s</Row>", xmlwrite.str());
                mb.append(trace.str());
            }
        }
    } buffers[2];
    atomic_t rowBufInUse;

    static const unsigned traceRowBufferSize=11;  // we only output 10
    Owned<IThorDataLink> thorDataLink;

    IHThorArg *helper;
    activity_id activityId;

    inline void enqueueRowForTrace(const void *row)
    {
        buffers[atomic_read(&rowBufInUse)].enqueue(row);
    }

public:
    virtual void start()
    {
        thorDataLink->start();
        activityId = queryFromActivity()->queryActivityId();
    }
    virtual bool isGrouped() { return thorDataLink->isGrouped();}
    virtual const void *nextRowGE(const void * seek, unsigned numFields, bool &wasCompleteMatch, const SmartStepExtra &stepExtra)
    {
        const void *row = thorDataLink->nextRowGE(seek, numFields, wasCompleteMatch, stepExtra);
        enqueueRowForTrace(row);
        return row;
    }    // can only be called on stepping fields.
    virtual IInputSteppingMeta *querySteppingMeta() { return thorDataLink->querySteppingMeta(); }
    virtual bool gatherConjunctions(ISteppedConjunctionCollector & collector) { return thorDataLink->gatherConjunctions(collector); }
    virtual void resetEOF() { thorDataLink->resetEOF();}

    virtual void getMetaInfo(ThorDataLinkMetaInfo &info) { thorDataLink->getMetaInfo(info);}
    virtual CActivityBase *queryFromActivity() {return thorDataLink->queryFromActivity();} // activity that has this as an output
    virtual void dataLinkSerialize(MemoryBuffer &mb) {thorDataLink->dataLinkSerialize(mb);}
    virtual unsigned __int64 queryTotalCycles() const { return thorDataLink->queryTotalCycles();}
    virtual void debugRequest(MemoryBuffer &mb)
    {
        // NOTE - cannot be called by more than one thread
        buffers[1].init();
        int bufToDump = atomic_read(&rowBufInUse);
        atomic_set(&rowBufInUse, 1-bufToDump);
        buffers[bufToDump].dump(mb, helper);
        // TBD - may need a third buffer to ensure that a second call to debugRequest gets
        // any old records that have not been evicted.
    }

    virtual const void *nextRow()
    {
        const void *row = thorDataLink->nextRow();
        enqueueRowForTrace(row);
        return row;
    }
    virtual void stop() {
        thorDataLink->stop();
    }                              // after stop called NULL is returned

    inline const void *ungroupedNextRow() { return thorDataLink->ungroupedNextRow();}

    CTracingThorDataLink(IThorDataLink *_input, IHThorArg *_helper)
    : thorDataLink(_input), helper(_helper), activityId(0)
    {
        atomic_set(&rowBufInUse, 0);
        buffers[0].init();
    }
};

#endif
