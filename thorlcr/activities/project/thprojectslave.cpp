/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

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


#include "thprojectslave.ipp"
#include "eclrtl_imp.hpp"
#include "thorstrand.hpp"

//  IThorDataLink needs only be implemented once, since there is only one output,
//  therefore may as well implement it here.

class CProjectProcessor : public StrandProcessor
{
    IHThorProjectArg * helper;
    bool anyThisGroup;
    IRowStream  *input;
    Owned<IEngineRowAllocator> allocator;

public:
    CProjectProcessor(CSlaveActivity & _activity) : StrandProcessor(_activity), helper(nullptr)
    {
        input = NULL;
        anyThisGroup = false;
    }
    IMPLEMENT_IINTERFACE_USING(CSimpleInterface);

    void init(IHThorProjectArg * _helper, IEngineRowAllocator * _allocator)
    {
        helper = _helper;
        anyThisGroup = false;
        allocator.set(_allocator);
    }
    IEngineRowStream * queryOutput() { return this; }
    void setInput(IRowStream * _rowStream)
    {
        input = _rowStream;
    }
    void startProcessor()
    {
        dataLinkStart();
    }
    void stopProcessor()
    {
        dataLinkStop();
    }
    CATCH_NEXTROW()
    {
        ActivityTimer t(totalCycles, owner->queryTimeActivities());
        loop {
            OwnedConstThorRow row = input->nextRow();
            if (!row && !anyThisGroup)
                row.setown(input->nextRow());

            if (!row||isAborting())
                break;
            RtlDynamicRowBuilder ret(allocator);
            size32_t sz;
            try {
                sz = helper->transform(ret, row);
            }
            catch (IException *e)
            {
                owner->ActPrintLog(e, "In helper->transform()");
                throw;
            }
            catch (CATCHALL)
            {
                owner->ActPrintLog("PROJECT: Unknown exception in helper->transform()");
                throw;
            }
            if (sz) {
                dataLinkIncrement();
                anyThisGroup = true;
                return ret.finalizeRowClear(sz);
            }
        }
        anyThisGroup = false;
        return NULL;
    }
    virtual void start()
    {
        //Not sure what to do.... junction may need to hold a count + pass on when done
    }
    virtual void stop()
    {
        //Not sure what to do.... junction may need to hold a count + pass on when done
    }
    void getMetaInfo(ThorDataLinkMetaInfo &info)
    {
        throwUnexpected();
    }

    virtual bool isGrouped() { throwUnexpected(); return false; }
};


class CProjectSlaveActivity : public CSlaveActivity, public CThorDataLink
{
    IHThorProjectArg * helper;
    CProjectProcessor * * processors;
    unsigned numStrands;
    unsigned rowsPerBlock;
    Owned<IStrandBranch> branch;
    IStrandJunction * inputJunction;
    IStrandJunction * outputJunction;
    IRowStream * strandOutput;
    bool orderedStrands;
public:
    IMPLEMENT_IINTERFACE_USING(CSimpleInterface);

    CProjectSlaveActivity(CGraphElementBase *_container) : CSlaveActivity(_container), CThorDataLink(this)
    {
        numStrands = getOptInt(THOROPT_NUM_STRANDS, 1);
        rowsPerBlock = getOptInt(THOROPT_STRAND_BLOCK_SIZE, 2000);
        orderedStrands = getOptBool(THOROPT_STRAND_ORDERED, true);
        if (numStrands < 1)
            numStrands = 1;
        ActPrintLog("PROJECT: Strands(%u) rowsPerBlock(%u) ordered(%u)", numStrands, rowsPerBlock, orderedStrands);
        processors = new CProjectProcessor * [numStrands];
        for (unsigned i=0; i < numStrands; i++)
            processors[i] = new CProjectProcessor(*this);
        inputJunction = nullptr;
        outputJunction = nullptr;
    }
    ~CProjectSlaveActivity()
    {
        delete [] processors;
    }

    void init(MemoryBuffer &data, MemoryBuffer &slaveData)
    {
        appendOutputLinked(this);
        helper = static_cast <IHThorProjectArg *> (queryHelper());
        for (unsigned i=0; i < numStrands; i++)
        {
            Owned<IEngineRowAllocator> allocator = queryJobChannel().getRowAllocator(queryRowMetaData(),queryActivityId(),roxiemem::RHFunique);
            processors[i]->init(helper, allocator);
        }

        //MORE: The following logic should be outside of the activity code, to allow chains
        //of activities to be connected.  Hopefully the following code is the correct structure for that.
        bool isGrouped = helper->queryOutputMeta()->isGrouped();
        branch.setown(createStrandBranch(queryRowManager(), numStrands, rowsPerBlock, orderedStrands, isGrouped));
        inputJunction = branch->queryInputJunction();
        outputJunction = branch->queryOutputJunction();
    }

    void start()
    {
        ActivityTimer s(totalCycles, timeActivities);

        //Connect inputs
        inputJunction->setInput(0, inputs.item(0));
        for (unsigned i1=0; i1 < numStrands; i1++)
            processors[i1]->setInput(inputJunction->queryOutput(i1));

        //Connect outputs
        for (unsigned i2=0; i2 < numStrands; i2++)
            outputJunction->setInput(i2, processors[i2]->queryOutput());
        strandOutput = outputJunction->queryOutput(0);

        //MORE: The processors will not be started... - an issue if multiple activities.
        startInput(inputs.item(0));
        for (unsigned i3=0; i3 < numStrands; i3++)
            processors[i3]->startProcessor();

        inputJunction->ready();
        outputJunction->ready();
    }
    void stop()
    {
        for (unsigned i3=0; i3 < numStrands; i3++)
            processors[i3]->stopProcessor();
        stopInput(inputs.item(0));
    }
    CATCH_NEXTROW()
    {
        return strandOutput->nextRow();
    }
    void getMetaInfo(ThorDataLinkMetaInfo &info)
    {
        initMetaInfo(info);
        info.fastThrough = true; // ish
        if (helper->canFilter())
            info.canReduceNumRows = true;
        calcMetaInfoSize(info,inputs.item(0));
    }

    virtual bool isGrouped() { return inputs.item(0)->isGrouped(); }
};


class CPrefetchProjectSlaveActivity : public CSlaveActivity, public CThorDataLink
{
    IHThorPrefetchProjectArg *helper;
    rowcount_t numProcessedLastGroup;
    bool eof;
    IThorDataLink *input;
    Owned<IEngineRowAllocator> allocator;
    IThorChildGraph *child;
    bool parallel;
    unsigned preload;

    class PrefetchInfo : public CSimpleInterface
    {
    public:
        inline PrefetchInfo(IHThorPrefetchProjectArg &helper, const void *_in, unsigned __int64 _recordCount)
        {
            if (helper.preTransform(extract, _in, _recordCount))
            {
                in.setown(_in);
                recordCount = _recordCount;
            }
            else
                ReleaseThorRow(_in);
        }
        OwnedConstThorRow in;
        unsigned __int64 recordCount;
        rtlRowBuilder extract;
    };
    class CPrefetcher : public CSimpleInterface, implements IThreaded
    {
        CPrefetchProjectSlaveActivity &parent;
        CThreadedPersistent threaded;
        rowcount_t recordCount;
        bool full, blocked, stopped, eoi, eog, eoq;
        QueueOf<PrefetchInfo, true> prefetchQueue;
        CriticalSection crit;
        Semaphore blockedSem, fullSem;

    public:
        CPrefetcher(CPrefetchProjectSlaveActivity &_parent) : threaded("CPrefetcher", this), parent(_parent)
        {
        }
        ~CPrefetcher() { stop(); }
        PrefetchInfo *pullRecord()
        {
            OwnedConstThorRow row = parent.input->nextRow();
            if (row)
            {
                eog = false;
                return new PrefetchInfo(*parent.helper, row.getClear(), ++recordCount);
            }
            else if (!eog)
            {
                eog = true;
                return NULL;
            }
            eoi = true;
            return NULL;
        }
        void start() { recordCount = 0; full = blocked = eoq = eoi = stopped = false; eog = true; threaded.start(); }
        void stop()
        {
            stopped = true;
            fullSem.signal();
            threaded.join();
            while (prefetchQueue.ordinality())
                ::Release(prefetchQueue.dequeue());
        }
        void abort()
        {
            blockedSem.signal(); // reader might be stuck
        }
        void main()
        {
            loop
            {
                Owned<PrefetchInfo> fetchRow = pullRecord();
                CriticalBlock b(crit);
                if (!eoi)
                    prefetchQueue.enqueue(fetchRow.getClear());
                if (blocked)
                {
                    blocked = false;
                    blockedSem.signal();
                }
                if (eoi)
                    break;
                if (prefetchQueue.ordinality() >= parent.preload)
                {
                    full = true;
                    CriticalUnblock b(crit);
                    fullSem.wait();
                    if (stopped)
                        break;
                }
            }
        }
        PrefetchInfo *getPrefetchRow()
        {
            if (eoq)
                return NULL;
            CriticalBlock b(crit);
            loop
            {
                if (prefetchQueue.ordinality())
                {
                    if (full)
                    {
                        full = false;
                        fullSem.signal();
                    }
                    return prefetchQueue.dequeue();
                }
                else
                {
                    if (eoi)
                    {
                        eoq = true;
                        return NULL;
                    }
                    blocked = true;
                    CriticalUnblock b(crit);
                    blockedSem.wait();
                }
            }
        }
    } prefetcher;

    PrefetchInfo *readNextRecord()
    {
        if (!parallel)
            return prefetcher.pullRecord();
        else
            return prefetcher.getPrefetchRow();
    }

public:
    IMPLEMENT_IINTERFACE_USING(CSimpleInterface);

    CPrefetchProjectSlaveActivity(CGraphElementBase *_container) : CSlaveActivity(_container), CThorDataLink(this), prefetcher(*this)
    {
        helper = (IHThorPrefetchProjectArg *) queryHelper();
        parallel = 0 != (helper->getFlags() & PPFparallel);
        preload = helper->getLookahead();
        if (!preload)
            preload = 10; // default
        child = helper->queryChild();
    }
    void init(MemoryBuffer &data, MemoryBuffer &slaveData)
    {
        appendOutputLinked(this);
        allocator.set(queryRowAllocator());
    }
    void start()
    {
        ActivityTimer s(totalCycles, timeActivities);
        input = inputs.item(0);
        startInput(input);

        numProcessedLastGroup = getDataLinkGlobalCount();
        eof = !helper->canMatchAny();
        if (parallel)
            prefetcher.start();
        dataLinkStart();
    }
    void stop()
    {
        if (parallel)
            prefetcher.stop();
        stopInput(input);
        dataLinkStop();
    }
    CATCH_NEXTROW()
    {
        ActivityTimer t(totalCycles, timeActivities);
        if (eof)
            return NULL;
        loop
        {
            Owned<PrefetchInfo> prefetchRow = readNextRecord();
            if (!prefetchRow)
            {
                if (numProcessedLastGroup == getDataLinkGlobalCount())
                    prefetchRow.setown(readNextRecord());
                if (!prefetchRow)
                {
                    numProcessedLastGroup = getDataLinkGlobalCount();
                    eof = true;
                    return NULL;
                }
            }
            if (prefetchRow->in)
            {
                RtlDynamicRowBuilder out(allocator);
                Owned<IEclGraphResults> results;
                if (child)
                    results.setown(child->evaluate(prefetchRow->extract.size(), prefetchRow->extract.getbytes()));
                size32_t outSize = helper->transform(out, prefetchRow->in, results, prefetchRow->recordCount);
                if (outSize)
                {
                    dataLinkIncrement();
                    return out.finalizeRowClear(outSize);
                }
            }
        }
    }
    void abort()
    {
        CSlaveActivity::abort();
        prefetcher.abort();
    }
    void getMetaInfo(ThorDataLinkMetaInfo &info)
    {
        initMetaInfo(info);
        if (helper->canFilter())
            info.canReduceNumRows = true;
        calcMetaInfoSize(info,inputs.item(0));
    }
    virtual bool isGrouped() { return inputs.item(0)->isGrouped(); }
};


CActivityBase *createPrefetchProjectSlave(CGraphElementBase *container)
{
    return new CPrefetchProjectSlaveActivity(container);
}


CActivityBase *createProjectSlave(CGraphElementBase *container)
{
    return new CProjectSlaveActivity(container);
}


