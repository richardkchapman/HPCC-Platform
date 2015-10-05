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

#include "platform.h"
#include "jprop.hpp"
#include "thormisc.hpp"
#include "thtmptableslave.ipp"
#include "thorport.hpp"
#include "thactivityutil.ipp"
#include <atomic>
#include "thorstrand.hpp"

/*
 * This class can be used for creating massive temp tables and will also be used
 * when optimising NORMALISE(ds, count) to DATASET(count, transform), so could end
 * up consuming a lot of rows.
 *
 */
class CInlineTableSlaveActivity : public CSlaveActivity, public CThorDataLink
{
private:
    class InlineTableProcessor : public StrandProcessor
    {
    public:
        InlineTableProcessor(CSlaveActivity & _activity) : StrandProcessor(_activity)
        {
            helper = NULL;
            startRow = 0;
            currentRow = 0;
            maxRow = 0;
        }

        void init(IHThorInlineTableArg * _helper, IEngineRowAllocator * _rowAllocator)
        {
            helper = _helper;
            rowAllocator.setown(_rowAllocator);
        }

        void onstart(__uint64 _startRow, __uint64 _maxRow)
        {
            setAborting(false);
            startRow = _startRow;
            currentRow = _startRow;
            maxRow = _maxRow;
        }

        void start()
        {
            dataLinkStart();
        }

        virtual void stop()
        {
            setAborting(true);
        }

        virtual bool isGrouped()
        {
            return false;
        }

        void reset()
        {
            setAborting(false);
        }

        virtual void getMetaInfo(ThorDataLinkMetaInfo &info) {}
        virtual void dataLinkSerialize(MemoryBuffer &mb) {}

        CATCH_NEXTROW()
        {
            ActivityTimer t(totalCycles, owner->queryTimeActivities());
            if (isAborting())
                return NULL;
            while (currentRow < maxRow) {
                RtlDynamicRowBuilder row(rowAllocator);
                size32_t sizeGot = helper->getRow(row, currentRow++);
                if (sizeGot)
                {
                    dataLinkIncrement();
                    return row.finalizeRowClear(sizeGot);
                }
            }
            return NULL;
        }
    protected:
        IHThorInlineTableArg * helper;
        Owned<IEngineRowAllocator> rowAllocator;

        rowcount_t startRow;
        rowcount_t currentRow;
        rowcount_t maxRow;
    };

    unsigned numStrands;
    InlineTableProcessor * * processors;
    Owned<IStrandJunction> junction;
    IRowStream * output;

public:
    IMPLEMENT_IINTERFACE_USING(CSimpleInterface);

    CInlineTableSlaveActivity(CGraphElementBase *_container)
    : CSlaveActivity(_container), CThorDataLink(this)
    {
        numStrands = getOptInt(THOROPT_NUM_STRANDS, 1);
        unsigned blockSize = getOptInt(THOROPT_STRAND_BLOCK_SIZE, 32);
        bool isOrdered = getOptBool(THOROPT_STRAND_ORDERED, false);
        ActPrintLog("InlineSLAVE: strands = %u blockSize = %u ordered(%d)", numStrands, blockSize, (int)isOrdered);
        processors = new InlineTableProcessor * [numStrands];

        IHThorInlineTableArg * helper = static_cast <IHThorInlineTableArg *> (queryHelper());
        for (unsigned strand=0; strand < numStrands; strand++)
        {
            processors[strand] = new InlineTableProcessor(*this);
            processors[strand]->init(helper, queryJobChannel().getRowAllocator(queryRowMetaData(),queryActivityId(),roxiemem::RHFunique));
        }
        junction.setown(createStrandJunction(queryRowManager(), numStrands, 1, blockSize, isOrdered));
        for (unsigned i=0; i < numStrands; i++)
            junction->setInput(i, processors[i]);
        output = junction->queryOutput(0);
        startRow = 0;
        maxRow = 0;
    }
    virtual bool isGrouped() { return false; }
    void init(MemoryBuffer &data, MemoryBuffer &slaveData)
    {
        appendOutputLinked(this);
    }
    void start()
    {
        ActivityTimer s(totalCycles, timeActivities);
        dataLinkStart();

        IHThorInlineTableArg * helper = static_cast <IHThorInlineTableArg *> (queryHelper());
        __uint64 numRows = helper->numRows();
        // local when generated from a child query (the range is per node, don't split)
        bool isLocal = container.queryLocalData() || container.queryOwner().isLocalChild();
        if (!isLocal && ((helper->getFlags() & TTFdistributed) != 0))
        {
            __uint64 nodes = queryCodeContext()->getNodes();
            __uint64 nodeid = queryCodeContext()->getNodeNum();
            startRow = (nodeid * numRows) / nodes;
            maxRow = ((nodeid + 1) * numRows) / nodes;
            ActPrintLog("InlineSLAVE: numRows = %" I64F "d, nodes = %" I64F
                        "d, nodeid = %" I64F "d, start = %" I64F "d, max = %" I64F "d",
                        numRows, nodes, nodeid, startRow, maxRow);
        }
        else
        {
            startRow = 0;
            // when not distributed, only first node compute, unless local
            if (firstNode() || isLocal)
                maxRow = numRows;
            else
                maxRow = 0;
        }

        __uint64 num = maxRow - startRow;
        ActPrintLog("InlineSLAVE: numRows = %" I64F "d, strands = %u", num, numStrands);

        for (unsigned strand=0; strand < numStrands; strand++)
        {
            __uint64 from = startRow + (strand * num) / numStrands;
            __uint64 to = startRow + ((strand + 1) * num) / numStrands;
            processors[strand]->onstart(from, to);
            processors[strand]->start();
        }
        junction->ready();
    }
    void stop()
    {
        dataLinkStop();
        junction->stop();
    }
    void abort()
    {
        CSlaveActivity::abort();
        junction->abort();
    }
    void reset()
    {
        junction->reset();
        for (unsigned strand=0; strand < numStrands; strand++)
            processors[strand]->reset();
    }
    const void *nextRow()
    {
        return output->nextRow();
    }
    void getMetaInfo(ThorDataLinkMetaInfo &info)
    {
        initMetaInfo(info);
        info.isSource = true;
        info.unknownRowsOutput = false;
        info.totalRowsMin = info.totalRowsMax = maxRow - startRow;
    }

protected:
    rowcount_t startRow;
    rowcount_t maxRow;
};

CActivityBase *createInlineTableSlave(CGraphElementBase *container)
{
    return new CInlineTableSlaveActivity(container);
}
