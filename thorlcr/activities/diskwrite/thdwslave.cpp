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

#include "jio.hpp"
#include "jlzw.hpp"
#include "jtime.hpp"
#include "jfile.ipp"
#include "jqueue.hpp"

#include "dafdesc.hpp"

#include "thbuf.hpp"
#include "thexception.hpp"
#include "thbufdef.hpp"
#include "csvsplitter.hpp"
#include "thactivityutil.ipp"
#include "thdiskbaseslave.ipp"
#include "thdwslave.ipp"

class CDiskWriteSlaveActivity : public CDiskWriteSlaveActivityBase
{
protected:
    virtual void write()
    {
        ActPrintLog("%s",grouped?"Grouped":"Ungrouped");

        while(!abortSoon)
        {
            OwnedConstThorRow r = input->nextRow();
            if (!r.get()) {
                if (grouped) {
                    if ((processed & THORDATALINK_COUNT_MASK)!=0)
                        out->putRow(NULL);
                }
                r.setown(input->nextRow());
                if (!r.get())
                    break;
            }
            out->putRow(r.getClear());
            processed++;
        }
    }

public:
    CDiskWriteSlaveActivity(CGraphElementBase *container) : CDiskWriteSlaveActivityBase(container)
    {
    }
};


//---------------------------------------------------------------------------------------------------------------------

//very similar to IThreaded - quite possibly a replacement
interface ITask : public IInterface
{
    virtual ITask * execute() = 0;
    //Called when task is finished with - a derived implementation could release
    virtual void noteComplete()
    {
    }
};

typedef ReaderWriterQueue<ITask *, unsigned, 8, 1, 11, 0> TaskQueue;
class CTaskProcessor : public Thread
{
    TaskQueue & queue;
public:
    inline CTaskProcessor(TaskQueue & _queue) : Thread("TaskProcessor"), queue(_queue) { }
    virtual int run()
    {
        loop
        {
            ITask * task;
            if (!queue.dequeue(task, false))
                break;
            do
            {
                ITask * next = task->execute();
                task->noteComplete();
                task = next;
            } while (task);
        }
        return 1;
    }
};

class TaskPool : public CInterface
{
public:
    TaskPool(unsigned _maxTasks, TaskQueue & _queue) : maxTasks(_maxTasks)
    {
        processors = new CTaskProcessor * [maxTasks];
        for (unsigned i = 0; i < maxTasks; i++)
            processors[i] = new CTaskProcessor(_queue);
    }
    ~TaskPool()
    {
        for (unsigned i = 0; i < maxTasks; i++)
            ::Release(processors[i]);
        delete [] processors;
    }

    void start()
    {
        for (unsigned i = 0; i < maxTasks; i++)
            processors[i]->start();
    }

    void wait()
    {
        for (unsigned i = 0; i < maxTasks; i++)
            processors[i]->join();
    }

private:
    CTaskProcessor * * processors;
    unsigned maxTasks;
};

//---------------------------------------------------------------------------------------------------------------------

class CSequencer
{
    const static unsigned PendingFlag = 0x80000000;
public:
    CSequencer() : maxUsers(0)
    {
        sems = NULL;
        curUser.store(0, std::memory_order_relaxed);
    }
    CSequencer(unsigned _maxUsers) : maxUsers(_maxUsers)
    {
        sems = new Semaphore[maxUsers];
        curUser.store(0, std::memory_order_relaxed);
    }
    ~CSequencer()
    {
        delete [] sems;
    }

    void nextInput()
    {
        unsigned next = curUser.load(std::memory_order_relaxed) + 1;
        if (next == maxUsers)
            next = 0;
        curUser.store(next | PendingFlag, std::memory_order_release);
        sems[next].signal();
    }

    void reset()
    {
        curUser = 0;
        for (unsigned i=0; i < maxUsers; i++)
            sems[i].reinit(0);
    }

    void setMax(unsigned _maxUsers)
    {
        assertex(maxUsers == 0);
        maxUsers = _maxUsers;
        sems = new Semaphore[maxUsers];
    }

    void waitTurn(unsigned id)
    {
        if (curUser.load(std::memory_order_relaxed) == id)
            return;
        sems[id].wait();
        unsigned activeUser = curUser.load(std::memory_order_relaxed);
        assertex(id == (activeUser &~ PendingFlag));
        curUser.store(activeUser &~PendingFlag, std::memory_order_relaxed);
    }

private:
    unsigned maxUsers;
    std::atomic<unsigned> curUser;  // Only updated by a single thread at a time
    Semaphore * sems;
};

//---------------------------------------------------------------------------------------------------------------------

class CSequentialIOStream : public CInterfaceOf<IFileIOStream>
{
public:
    CSequentialIOStream(unsigned _id, IFileIOStream * _stream, CSequencer & _sequencer)
        : id(_id), stream(_stream), sequencer(_sequencer)
    {
    }
    virtual size32_t read(size32_t max_len, void * data)
    {
        sequencer.waitTurn(id);
        return stream->read(max_len, data);
    }
    virtual void flush()
    {
        sequencer.waitTurn(id);
        stream->flush();
    }
    virtual size32_t write(size32_t len, const void * data)
    {
        sequencer.waitTurn(id);
        return stream->write(len, data);
    }
    virtual void seek(offset_t pos, IFSmode origin)
    {
        sequencer.waitTurn(id);
        stream->seek(pos, origin);
    }
    virtual offset_t size()
    {
        sequencer.waitTurn(id);
        return stream->size();
    }
    virtual offset_t tell()
    {
        sequencer.waitTurn(id);
        return stream->tell();
    }

private:
    CSequencer & sequencer;
    Linked<IFileIOStream> stream;
    unsigned id;
};

//More similar to some classes in thorstrand.cpp
class CDiskWriteProcessor : public CInterfaceOf<ITask>
{
public:
    CDiskWriteProcessor(unsigned _id, size32_t _maxRows, CSequencer & _sequencer) : id(_id), ready(1), maxRows(_maxRows), sequencer(_sequencer)
    {
        rows = new const void * [maxRows];
        curRow = 0;
    }
    ~CDiskWriteProcessor()
    {
        delete [] rows;
    }

    void close()
    {
        if (out) {
            //output will already have been flushed
            out.clear();
        }
    }

    inline bool empty() const { return curRow == 0; }

    virtual void prepareForInput()
    {
        ready.wait();
        curRow = 0;
    }

    inline bool putRowNowFull(const void * row)
    {
        rows[curRow] = row;
        return (++curRow == maxRows);
    }

    void noteComplete()
    {
        ready.signal();
    }

    virtual ITask * execute()
    {
        const unsigned maxRow = curRow;
        for (unsigned i=0; i < maxRow; i++)
            out->putRow(rows[i]);
        curRow = 0;
        out->flush();
        sequencer.nextInput();
        return NULL;
    }

    void reset()
    {
        //ReleaseRows;
        curRow = 0;
        ready.reinit(1);
    }

    void setOutput(IExtRowWriter * _out)
    {
        out.set(_out);
    }

protected:
    unsigned id;
    Semaphore ready;
    size32_t curRow;
    size32_t maxRows;
    const void * * rows;
    Owned<IExtRowWriter> out;
    CSequencer & sequencer;
};

//---------------------------------------------------------------------------------------------------------------------

class CParallelDiskWriteSlaveActivity : public CDiskWriteSlaveActivityBase
{
protected:
    virtual void write()
    {
        Owned<TaskPool> taskPool = new TaskPool(numThreads, tasks);
        taskPool->start();

        ActPrintLog("%s",grouped?"Grouped":"Ungrouped");

        unsigned curStrand = 0;
        CDiskWriteProcessor * curProcessor = processors[curStrand];
        curProcessor->prepareForInput();
        while(!abortSoon)
        {
            OwnedConstThorRow r = input->nextRow();
            if (!r.get()) {
                if (grouped) {
                    if ((processed & THORDATALINK_COUNT_MASK)!=0)
                        out->putRow(NULL);
                }
                r.setown(input->nextRow());
                if (!r.get())
                {
                    if (!curProcessor->empty())
                        tasks.enqueue(curProcessor);
                    break;
                }
            }
            processed++;
            if (curProcessor->putRowNowFull(r.getClear()))
            {
                tasks.enqueue(curProcessor);
                curStrand++;
                if (curStrand == numStrands)
                    curStrand = 0;
                curProcessor = processors[curStrand];
                curProcessor->prepareForInput();
            }
        }
        tasks.noteWriterStopped();
        taskPool->wait();
    }

    virtual void open()
    {
        Owned<IFileIO> iFileIO = createOutputIO();
        calcFileCrc = true;
        if (compress)
        {
            ActPrintLog("Performing row compression on output file: %s", fName.get());
            // NB: block compressed output has implicit crc of 0, no need to calculate in row  writer.
            calcFileCrc = false;
        }
        assertex(!wantRaw());

        unsigned rwFlags = 0;
        if (grouped)
            rwFlags |= rw_grouped;
        if (calcFileCrc)
            rwFlags |= rw_crc;
        Owned<IFileIOStream> stream = createIOStream(iFileIO);
        for (unsigned i=0; i < numStrands; i++)
        {
            Owned<IFileIOStream> strandStream = new CSequentialIOStream(i, stream, sequencer);
            //MORE: May want to create buffer if want raw
            Owned<IExtRowWriter> strandOut = createRowWriter(strandStream, ::queryRowInterfaces(input), rwFlags);
            processors[i]->setOutput(strandOut);
        }

        bool extend = 0 != (diskHelperBase->getFlags() & TDWextend);
        bool external = dlfn.isExternal();
        bool query = dlfn.isQuery();
        if (extend || (external && !query))
            stream->seek(0,IFSend);

        ActPrintLog("Created output streams for %s", fName.get());
    }

    void close()
    {
        for (unsigned i=0; i < numStrands; i++)
            processors[i]->close();
        CDiskWriteSlaveActivityBase::close();
    }

    const unsigned maxTaskItems = 63; // should be configurable....
public:
    CParallelDiskWriteSlaveActivity(CGraphElementBase *container) : CDiskWriteSlaveActivityBase(container), tasks(1, maxTaskItems)
    {
        numStrands = getOptInt(THOROPT_NUM_STRANDS, 32);
        numThreads = getAffinityCpus();
        rowsPerBlock = getOptInt(THOROPT_STRAND_BLOCK_SIZE, 2000);
        if (numThreads > numStrands)
            numThreads = numStrands;
        processors = NULL;
        ActPrintLog("Performing parallel (%u, %u)", numStrands, rowsPerBlock);
    }
    ~CParallelDiskWriteSlaveActivity()
    {
        if (processors)
        {
            for (unsigned i = 0; i < numStrands; i++)
                ::Release(processors[i]);
            delete [] processors;
        }
    }
    virtual void init(MemoryBuffer &data, MemoryBuffer &slaveData)
    {
        CDiskWriteSlaveActivityBase::init(data, slaveData);
        sequencer.setMax(numStrands);
        processors = new CDiskWriteProcessor * [numStrands];
        for (unsigned i=0; i < numStrands; i++)
            processors[i] = createProcessor(i);
    }

    virtual CDiskWriteProcessor * createProcessor(unsigned id)
    {
        //MORE: Override depending on the output type
        return new CDiskWriteProcessor(id, rowsPerBlock, sequencer);
    }

protected:
    unsigned numStrands;
    unsigned numThreads;
    unsigned rowsPerBlock;
    CDiskWriteProcessor * * processors;
    TaskQueue tasks;
    CSequencer sequencer;
};

CActivityBase *createDiskWriteSlave(CGraphElementBase *container)
{
    if (container->getOptInt(THOROPT_NUM_STRANDS, 0))
        return new CParallelDiskWriteSlaveActivity(container);
    return new CDiskWriteSlaveActivity(container);
}


//---------------------------------------------------------------------------



class CCsvWriteSlaveActivity : public CDiskWriteSlaveActivity
{
protected:
    IHThorCsvWriteArg *helper;
    CSVOutputStream csvOutput;
    bool singleHF;
protected:
    virtual void write()
    {
        if (!singleHF || firstNode())
        {
            OwnedRoxieString header(helper->queryCsvParameters()->getHeader());
            if (header)
            {
                csvOutput.beginLine();
                csvOutput.writeHeaderLn(strlen(header),header);
                const char * outText = csvOutput.str();
                unsigned outLength = csvOutput.length();

                outraw->write(outLength,outText);
                if (calcFileCrc)
                    fileCRC.tally(outLength, outText);
            }
        }
        while(!abortSoon)
        {
            OwnedConstThorRow r(input->ungroupedNextRow());
            if (!r) 
                break;

            csvOutput.beginLine();
            helper->writeRow((const byte *)r.get(), &csvOutput);
            csvOutput.endLine();

            const char * outText = csvOutput.str();
            unsigned outLength = csvOutput.length();

            outraw->write(outLength,outText);
            if (calcFileCrc)
                fileCRC.tally(outLength, outText);

            processed++;
        }
        if (!singleHF || lastNode())
        {
            OwnedRoxieString footer(helper->queryCsvParameters()->getFooter());
            if (footer)
            {
                csvOutput.beginLine();
                csvOutput.writeHeaderLn(strlen(footer),footer);
                const char * outText = csvOutput.str();
                unsigned outLength = csvOutput.length();

                outraw->write(outLength,outText);
                if (calcFileCrc)
                    fileCRC.tally(outLength, outText);
            }
        }
    }

public:
    CCsvWriteSlaveActivity(CGraphElementBase *container) : CDiskWriteSlaveActivity(container) { }
    void init(MemoryBuffer &data, MemoryBuffer &slaveData)
    {
        CDiskWriteSlaveActivity::init(data, slaveData);
        helper = static_cast <IHThorCsvWriteArg *> (queryHelper());

        singleHF = 0 != (ICsvParameters::singleHeaderFooter & helper->queryCsvParameters()->getFlags());
        csvOutput.init(helper->queryCsvParameters(), 0 != container.queryJob().getWorkUnitValueInt("oldCSVoutputFormat", 0));
    }
    virtual bool wantRaw() { return true; }
};


CActivityBase *createCsvWriteSlave(CGraphElementBase *container)
{
    return new CCsvWriteSlaveActivity(container);
}

#if 0
void CsvWriteSlaveActivity::setFormat(IFileDescriptor * desc)
{
    desc->queryAttributes().setProp("@format","csv");
}
#endif
