/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2015 HPCC Systems®.

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

#include "jexcept.hpp"
#include "jthread.hpp"
#include "jqueue.hpp"
#include "roxiemem.hpp"
#include "thorstrand.hpp"

class CStrandJunction : public CInterfaceOf<IStrandJunction>
{
};


class OneToOneJunction : public CStrandJunction
{
public:
    OneToOneJunction() : input(NULL) {}

    virtual IEngineRowStream * queryOutput(unsigned n)
    {
        assertex(n == 0);
        assertex(input);
        return input;
    }
    virtual void setInput(unsigned n, IEngineRowStream * _stream)
    {
        assertex(n == 0);
        input = _stream;
    }
    virtual void stop()
    {
        assertex(input);
        input->stop();
    }
    virtual void ready()
    {
    }
    virtual void reset()
    {
    }
    virtual void abort()
    {
        //MORE?
    }

protected:
    IEngineRowStream * input;
};

//---------------------------------------------------------------------------------------------------------------------

void RoxieRowBlock::releaseBlock()
{
    releaseRows();
    readPos = 0;
    writePos = 0;
    endOfChunk = false;
    delete this;
}

void RoxieRowBlock::releaseRows()
{
    while (readPos < writePos)
        ReleaseRoxieRow(rows[readPos++]);
}

void RoxieRowBlock::throwAnyPendingException()
{
    if (exception)
        throw exception.getClear();
}

void RoxieRowBlock::operator delete (void * ptr)
{
    ReleaseRoxieRow(ptr);
}

//---------------------------------------------------------------------------------------------------------------------

RowBlockAllocator::RowBlockAllocator(roxiemem::IRowManager & rowManager, size32_t minRowsPerBlock) : rowsPerBlock(0)
{
    assertex(minRowsPerBlock);

    size_t requestedSize = sizeof(RoxieRowBlock) + minRowsPerBlock * sizeof(void*);
    unsigned heapFlags = roxiemem::RHFunique;
    heap.setown(rowManager.createFixedRowHeap(requestedSize, 0, heapFlags, 0));
    rowsPerBlock = (rowManager.getExpectedCapacity(requestedSize, heapFlags) - sizeof(RoxieRowBlock) ) / sizeof(void*);
    assertex(rowsPerBlock >= minRowsPerBlock);
}

//A bit of an experimental implementation - other options could include a list like the allocators
RoxieRowBlock * RowBlockAllocator::newBlock()
{
    return new (heap->allocate()) RoxieRowBlock(rowsPerBlock);
}


//---------------------------------------------------------------------------------------------------------------------

class ReadAheadThread : public CInterface, implements IThreaded
{
public:
    ReadAheadThread(IRowQueue * _queue) : queue(_queue), input(NULL) {}

    virtual void main()
    {
        loop
        {
            const void * row = input->nextRow();
            if (!row)
                break;
            if (!queue->enqueue(row))
            {
                //Abort
                ReleaseRoxieRow(row);
                break;
            }
        }
        queue->noteWriterStopped();
    }

    void setInput(IEngineRowStream * _input)
    {
        input = _input;
    }

protected:
    IEngineRowStream * input;
    IRowQueue * queue;
};

class ManyToOneJunction : public CStrandJunction, public IEngineRowStream
{
public:
    IMPLEMENT_IINTERFACE_USING(CStrandJunction)

    ManyToOneJunction(unsigned _numStrands) : numStrands(_numStrands)
    {
        const unsigned maxQueued = numStrands * 64;
        queue.setown(createRowQueue(1, numStrands, maxQueued, 0));

        producers = new ReadAheadThread * [numStrands];
        for (unsigned i=0; i < numStrands; i++)
            producers[i] = new ReadAheadThread(queue);
    }
    ~ManyToOneJunction()
    {
        for (unsigned i=0; i < numStrands; i++)
            producers[i]->Release();
        delete [] producers;
    }

    virtual IEngineRowStream * queryOutput(unsigned n)
    {
        assertex(n == 0);
        return this;
    }
    virtual void setInput(unsigned n, IEngineRowStream * _stream)
    {
        assertex(n < numStrands);
        producers[n]->setInput(_stream);
    }
    virtual void abort()
    {
        queue->abort();
    }
    virtual void stop()
    {
        queue->abort();
    }
    virtual void reset()
    {
        queue->reset();
        clearRowQueue(queue);
    }
    virtual void ready()
    {
        for (unsigned i=0; i < numStrands; i++)
            asyncStart("ReadAheadThread", *producers[i]);
    }
    virtual const void *nextRow()
    {
        const void * next;
        if (queue->dequeue(next))
            return next;
        return NULL;
    }
    virtual void resetEOF()
    {
        throwUnexpectedX("resetEOF called on ManyToOneJunction");
    }

protected:
    unsigned numStrands;
    Owned<IRowQueue> queue;
    ReadAheadThread * * producers;
};


//---------------------------------------------------------------------------------------------------------------------

static void resetBlockQueue(IRowQueue * queue)
{
    queue->reset();

    loop
    {
        const void * next;
        if (!queue->dequeue(next))
            break;
        RoxieRowBlock * curBlock = (RoxieRowBlock *)next;
        if (curBlock)
            curBlock->releaseBlock();
    }
}


class BlockedReadAheadThread : public CInterface, implements IThreaded
{
public:
    BlockedReadAheadThread(IRowQueue * _queue, RowBlockAllocator & _allocator) : queue(_queue), input(NULL), allocator(_allocator) {}

    virtual void main()
    {
        bool done = false;
        while (!done)
        {
            RoxieRowBlock * block = allocator.newBlock();
            for (;;)
            {
                const void * row = input->nextRow();
                if (!row)
                {
                    done = true;
                    break;
                }
                if (block->addRowNowFull(row))
                    break;
            }
            if (block->empty() || !queue->enqueue(block))
            {
                block->releaseBlock();
                break;
            }
        }
        queue->noteWriterStopped();
    }

    void setInput(IEngineRowStream * _input)
    {
        input = _input;
    }

    void setQueue(IRowQueue * _queue)
    {
        queue = _queue;
    }

protected:
    RowBlockAllocator & allocator;
    IEngineRowStream * input;
    IRowQueue * queue;
};

class BlockedReader : public CInterfaceOf<IEngineRowStream>
{
public:
    BlockedReader(IRowQueue & _queue) : queue(_queue)
    {
        curBlock = NULL;
    }

    virtual const void *nextRow()
    {
        const void * ret;
        loop
        {
            if (curBlock)
            {
                if (curBlock->nextRow(ret))
                    return ret;
                curBlock->releaseBlock();
                curBlock = NULL;
            }
            const void * next;
            if (!queue.dequeue(next))
                return NULL;
            curBlock = (RoxieRowBlock *)next;
        }
    }

    virtual void stop()
    {
        //queue.abort();??
    }

    virtual void resetEOF()
    {
        throwUnexpectedX("resetEOF called on BlockedReader");
    }

    void reset()
    {
        if (curBlock)
            curBlock->releaseBlock();
        curBlock = NULL;
    }

protected:
    IRowQueue & queue;
    RoxieRowBlock * curBlock;
};

//---------------------------------------------------------------------------------------------------------------------

//MORE: Use BlockReader as a contained class?

class BlockedManyToOneJunction : public CStrandJunction
{
public:
    BlockedManyToOneJunction(roxiemem::IRowManager & _rowManager, unsigned _numStrands, unsigned blockSize, IRowQueue * _queue)
    : numStrands(_numStrands), queue(_queue), allocator(_rowManager, blockSize), consumer(*_queue)
    {
        producers = new BlockedReadAheadThread * [numStrands];
        for (unsigned i=0; i < numStrands; i++)
            producers[i] = new BlockedReadAheadThread(queue, allocator);
    }
    ~BlockedManyToOneJunction()
    {
        for (unsigned i=0; i < numStrands; i++)
            producers[i]->Release();
        delete [] producers;
    }

    virtual IEngineRowStream * queryOutput(unsigned n)
    {
        assertex(n == 0);
        return &consumer;
    }
    virtual void setInput(unsigned n, IEngineRowStream * _stream)
    {
        assertex(n < numStrands);
        producers[n]->setInput(_stream);
    }
    virtual void abort()
    {
        queue->abort();
    }
    virtual void stop()
    {
        queue->abort();
    }
    virtual void reset()
    {
        consumer.reset();
        resetBlockQueue(queue);
    }
    virtual void ready()
    {
        for (unsigned i=0; i < numStrands; i++)
            asyncStart("ReadAheadThread", *producers[i]);
    }
    static BlockedManyToOneJunction * create(roxiemem::IRowManager & rowManager, unsigned numStrands, unsigned blockSize)
    {
        const unsigned maxQueued = 60 - numStrands;
        Owned<IRowQueue> queue = createRowQueue(1, numStrands, maxQueued, 0);
        return new BlockedManyToOneJunction(rowManager, numStrands, blockSize, queue.getClear());

    }

protected:
    unsigned numStrands;
    Owned<IRowQueue> queue;
    RowBlockAllocator allocator;
    BlockedReadAheadThread * * producers;
    BlockedReader consumer;
};

//---------------------------------------------------------------------------------------------------------------------


class BlockedOneToManyJunction : public CStrandJunction
{
public:
    BlockedOneToManyJunction(roxiemem::IRowManager & _rowManager, unsigned _numStrands, unsigned _maxQueueItems, unsigned _blockSize)
    : numStrands(_numStrands), allocator(_rowManager, _blockSize), producer(NULL, allocator)
    {
        queue.setown(createRowQueue(numStrands, 1, _maxQueueItems, 0));
        producer.setQueue(queue);

        consumers = new BlockedReader * [numStrands];
        for (unsigned i=0; i < numStrands; i++)
            consumers[i] = new BlockedReader(*queue);
    }
    ~BlockedOneToManyJunction()
    {
        for (unsigned i=0; i < numStrands; i++)
            consumers[i]->Release();
        delete [] consumers;
    }

    virtual IEngineRowStream * queryOutput(unsigned n)
    {
        assertex(n < numStrands);
        return consumers[n];
    }
    virtual void setInput(unsigned n, IEngineRowStream * _stream)
    {
        assertex(n == 0);
        producer.setInput(_stream);
    }
    virtual void abort()
    {
//        producer.abort();
        queue->abort();
    }
    virtual void stop()
    {
        queue->abort();
    }
    virtual void reset()
    {
        resetBlockQueue(queue);
        for (unsigned i=0; i < numStrands; i++)
            consumers[i]->reset();
    }
    virtual void ready()
    {
        asyncStart(producer);
    }

protected:
    unsigned numStrands;
    Owned<IRowQueue> queue;
    RowBlockAllocator allocator;
    BlockedReadAheadThread producer;
    BlockedReader * * consumers;
};

//---------------------------------------------------------------------------------------------------------------------

//#define USESPIN
//Trivial single element queue
class OrderedReadAheadQueue
{
public:
    OrderedReadAheadQueue()
    {
#ifdef USESPIN
        hasValue.store(false);
#else
        space.reinit(1);
#endif
        value = NULL;
        abortSoon = false;
        done = false;
    }

    ~OrderedReadAheadQueue()
    {
        if (value)
            value->releaseBlock();
    }

    void abort()
    {
        abortSoon = true;
    }

    void reset()
    {
#ifndef USESPIN
        space.reinit(1);
#endif
        if (value)
        {
            value->releaseRows();
            value->releaseBlock();
            value = NULL;
        }
        abortSoon = false;
        done = false;
    }

    bool enqueue(RoxieRowBlock * next)
    {
        if (abortSoon)
            return false;
#ifdef USESPIN
        while (hasValue.load(std::memory_order_acquire))
            _mm_pause();
#else
        space.wait();
#endif
        if (abortSoon)
            return false;
        value = next;
#ifdef USESPIN
        hasValue.store(true);
#else
        avail.signal();
#endif
        return true;
    }

    bool noteWriterStopped()
    {
        if (abortSoon)
            return false;
#ifdef USESPIN
        while (hasValue.load(std::memory_order_acquire))
            _mm_pause();
#else
        space.wait();
#endif
        if (abortSoon)
            return false;
        done = true;
#ifdef USESPIN
        hasValue.store(true);
#else
        avail.signal();
#endif
        return true;
    }

    bool dequeue(RoxieRowBlock * & ret)
    {
        if (abortSoon)
            return false;
#ifdef USESPIN
        while (!hasValue.load(std::memory_order_acquire))
            _mm_pause();
#else
        avail.wait();
#endif
        if (abortSoon)
            return false;

        if (done)
        {
#ifndef USESPIN
            avail.signal();
#endif
            return false;
        }

        ret = value;
        value = NULL;
#ifdef USESPIN
        hasValue.store(false);
#else
        space.signal();
#endif
        return true;
    }

protected:
    RoxieRowBlock * value;
    bool abortSoon;
    bool done;
#ifdef USESPIN
    std::atomic<bool> hasValue;
#else
    Semaphore space __attribute__((aligned(CACHE_LINE_SIZE)));
    Semaphore avail __attribute__((aligned(CACHE_LINE_SIZE)));
#endif
};


class OrderedReadAheadThread : public CInterface, implements IThreaded
{
    friend class OrderedManyToOneJunction;
public:
    OrderedReadAheadThread(RowBlockAllocator & _allocator) : input(NULL), allocator(_allocator)
    {
        finished = false;
        alive = true;
    }

    virtual void main()
    {
        bool done = false;
        while (!done)
        {
            RoxieRowBlock * block = allocator.newBlock();
            for (;;)
            {
                const void * row = input->nextRow();
                if (!row)
                {
                    done = true;
                    break;
                }
                if (block->addRowNowFull(row))
                    break;
            }

            //Purely for testing - this would normally be in an independent callback
            block->setEndOfChunk();
            if (block->empty() || !queue.enqueue(block))
            {
                block->releaseBlock();
                break;
            }
        }
        noteWriterStopped();
    }

    void abort()
    {
        queue.abort();
    }

    void reset()
    {
        queue.reset();
        finished = false;
        alive = true;
    }

    void stop()
    {
        input->stop();
    }

    void setInput(IEngineRowStream * _input)
    {
        input = _input;
    }

    void noteWriterStopped()
    {
        finished = true;
        queue.noteWriterStopped();
    }

    inline OrderedReadAheadQueue & queryQueue() { return queue; }

protected:
    RowBlockAllocator & allocator;
    IEngineRowStream * input;
    OrderedReadAheadQueue queue;
    bool finished;
    bool alive;
};

class OrderedManyToOneJunction : public CStrandJunction, public IEngineRowStream
{
public:
    IMPLEMENT_IINTERFACE_USING(CStrandJunction)

    OrderedManyToOneJunction(roxiemem::IRowManager & _rowManager, unsigned _numStrands, unsigned blockSize) : numStrands(_numStrands), allocator(_rowManager, blockSize)
    {
        producers = new OrderedReadAheadThread * [numStrands];
        for (unsigned i=0; i < numStrands; i++)
            producers[i] = new OrderedReadAheadThread(allocator);
        curBlock = NULL;
        curStrand = 0;
        numActiveStrands = numStrands;
    }
    ~OrderedManyToOneJunction()
    {
        for (unsigned i=0; i < numStrands; i++)
            producers[i]->Release();
        delete [] producers;
    }

    virtual IEngineRowStream * queryOutput(unsigned n)
    {
        assertex(n == 0);
        return this;
    }
    virtual void setInput(unsigned n, IEngineRowStream * _stream)
    {
        assertex(n < numStrands);
        producers[n]->setInput(_stream);
    }
    virtual void abort()
    {
        for (unsigned i=0; i < numStrands; i++)
            producers[i]->abort();
    }
    virtual void stop()
    {
        for (unsigned i=0; i < numStrands; i++)
            producers[i]->stop();
    }
    virtual void reset()
    {
        if (curBlock)
        {
            curBlock->releaseBlock();
            curBlock = NULL;
        }
        for (unsigned strand =0; strand < numStrands; strand++)
            producers[strand]->reset();
        curStrand = 0;
        numActiveStrands = numStrands;
    }
    virtual void ready()
    {
        for (unsigned i=0; i < numStrands; i++)
        {
            CThreaded * thread = new CThreaded("ReadAheadThread", producers[i]);
            thread->startRelease();
        }
    }
    virtual const void *nextRow()
    {
        if (numActiveStrands == 0)
            return NULL;

        loop
        {
            if (curBlock)
            {
                const void * ret;
                if (curBlock->nextRow(ret))
                    return ret;
                bool isEnd = curBlock->isEndOfChunk();
                curBlock->releaseBlock();
                curBlock = NULL;
                if (isEnd)
                    nextStrand();
            }

            loop
            {
                OrderedReadAheadThread & curProducer = *(producers[curStrand]);
                OrderedReadAheadQueue & queue = curProducer.queryQueue();
                if (!queue.dequeue(curBlock))
                {
                    //Abort requested
                    numActiveStrands = 0;
                    return NULL;
                }

                //DBGLOG("active(%d) strand(%d)", numActiveStrands, curStrand);
                if (curBlock)
                    break;

                assertex(curProducer.finished);
                assertex(curProducer.alive);
                curProducer.alive = false;
                if (--numActiveStrands == 0)
                    return NULL;
                nextStrand();
            }
        }
    }

    virtual void resetEOF()
    {
        throwUnexpectedX("resetEOF called on OrderedManyToOneJunction");
    }

protected:
    void nextStrand()
    {
        do {
            curStrand++;
            if (curStrand == numStrands)
                curStrand = 0;
        } while (!producers[curStrand]->alive);
    }


protected:
    unsigned numStrands;
    unsigned numActiveStrands;
    RowBlockAllocator allocator;
    OrderedReadAheadThread * * producers;
    RoxieRowBlock * curBlock;
    unsigned curStrand;
};

//---------------------------------------------------------------------------------------------------------------------

IStrandJunction * createStrandJunction(roxiemem::IRowManager & rowManager, unsigned numInputs, unsigned numOutputs, unsigned blockSize, bool isOrdered)
{
    if ((numInputs == 1) && (numOutputs == 1))
        return new OneToOneJunction();
    if (numOutputs == 1)
    {
        if (isOrdered)
            return new OrderedManyToOneJunction(rowManager, numInputs, blockSize);
        if (blockSize != 1)
            return BlockedManyToOneJunction::create(rowManager, numInputs, blockSize);
        return new ManyToOneJunction(numInputs);
    }
    if (numInputs == 1)
    {
        unsigned maxQueueItems = numOutputs * 2;
        return new BlockedOneToManyJunction(rowManager, numOutputs, maxQueueItems, blockSize);
    }

    UNIMPLEMENTED_X("createStrandJunction");
}

void clearRowQueue(IRowQueue * queue)
{
    const void * next;
    while (queue->tryDequeue(next))
        ReleaseRoxieRow(next);
}


//---------------------------------------------------------------------------------------------------------------------

//Class for managing processing on a single ordered strand
class OrderedStrand : public CInterface, implements IThreaded, implements IEngineRowStream
{
    friend class OrderedManyToOneJunction;

public:
    OrderedStrand(RowBlockAllocator & _allocator) : strand(NULL), allocator(_allocator)
    {
        finished = false;
        alive = true;
        curInputBlock = nullptr;
        curOutputBlock = nullptr;
        eoi = false;
    }
    IMPLEMENT_IINTERFACE

 //interface IEngineRowStream for the rows being supplied to the strand.
    virtual const void *nextRow()
    {
        loop
        {
            if (!curInputBlock)
            {
                if (!inputQueue.dequeue(curInputBlock))
                {
                    eoi = true;
                    return NULL;
                }
            }

            const void * row;
            if (curInputBlock->nextRow(row))
                return row;
            curInputBlock->throwAnyPendingException();
            if (curInputBlock->isEndOfChunk())
                noteEndOfInputChunk();
            curInputBlock->releaseBlock();
            curInputBlock = NULL;
        }
    }

    virtual void stop() // problem - clashes with other interfaces???
    {
    }

    virtual void resetEOF()
    {
        throwUnexpectedX("resetEOF called on OrderedStrand");
    }

//IThreaded - main function used to read rows from the strand and add to the output
    virtual void main()
    {
        bool done = false;
        while (!done)
        {
            loop
            {
                const void * row = strand->nextRow();
                //NB: Need to be check the final eog isn't lost when processing sequentially
                if (!row && eoi)
                {
                    done = true;
                    break;
                }

                //curOutputBlock may be modified within the call to strand->nextRow() above
                //(but not by any other threads)
                if (!curOutputBlock)
                    curOutputBlock = allocator.newBlock();

                if (curOutputBlock->addRowNowFull(row))
                    break;
            }

            if (curOutputBlock)
            {
                if (curOutputBlock->empty() || !outputQueue.enqueue(curOutputBlock))
                {
                    curOutputBlock->releaseBlock();
                    curOutputBlock = NULL;
                    break;
                }
                curOutputBlock = NULL;
            }
        }
        finished = true;
        outputQueue.noteWriterStopped();
    }

    void noteEndOfInputChunk()
    {
        if (!curOutputBlock)
            curOutputBlock = allocator.newBlock();
        curOutputBlock->setEndOfChunk();
        if (!outputQueue.enqueue(curOutputBlock))
        {
            curOutputBlock->releaseBlock();
            inputQueue.abort();
        }
        curOutputBlock = NULL;
    }

    void abort()
    {
        inputQueue.abort();
        outputQueue.abort();
    }

    void reset()
    {
        inputQueue.reset();
        outputQueue.reset();
        finished = false;
        alive = true;
        if (curInputBlock)
            curInputBlock->releaseBlock();
        curInputBlock = nullptr;
        if (curOutputBlock)
            curOutputBlock->releaseBlock();
        curOutputBlock = nullptr;
        eoi = false;
    }

    IEngineRowStream * queryStrandInput()
    {
        return this;
    }

    void setStrand(IEngineRowStream * _input)
    {
        strand = _input;
    }

    inline OrderedReadAheadQueue & queryInputQueue() { return inputQueue; }
    inline OrderedReadAheadQueue & queryOutputQueue() { return outputQueue; }

protected:
    RowBlockAllocator & allocator;
    IEngineRowStream * strand;
    OrderedReadAheadQueue inputQueue;
    OrderedReadAheadQueue outputQueue;
    RoxieRowBlock * curInputBlock;
    RoxieRowBlock * curOutputBlock;
    bool finished;
    bool alive;
    bool eoi;
};


class COrderedStrandBranch : public CInterface, public IStrandBranch, public IThreaded, public IEngineRowStream
{
    class OrderedInputJunction : public CInterfaceOf<IStrandJunction>
    {
    public:
        OrderedInputJunction(COrderedStrandBranch & _owner) : owner(_owner) {}
        virtual IEngineRowStream * queryOutput(unsigned n) { return owner.queryStrandInput(n); }
        virtual void setInput(unsigned n, IEngineRowStream * _stream) { assertex(n==0); owner.setInput(_stream); }
        virtual void stop() { }
        virtual void ready() { owner.startInputReader(); }
        virtual void reset() { }
        virtual void abort() { }
    protected:
        COrderedStrandBranch & owner;
    };
    class OrderedOutputJunction : public CInterfaceOf<IStrandJunction>
    {
    public:
        OrderedOutputJunction(COrderedStrandBranch & _owner) : owner(_owner) {}
        virtual IEngineRowStream * queryOutput(unsigned n) { assertex(n==0); return owner.queryOutput(); }
        virtual void setInput(unsigned n, IEngineRowStream * _stream) { owner.setStrand(n, _stream); }
        virtual void stop() { }
        virtual void ready() { owner.startStrandReaders(); }
        virtual void reset() { }
        virtual void abort() { }
    protected:
        COrderedStrandBranch & owner;
    };

public:
    COrderedStrandBranch(roxiemem::IRowManager & _rowManager, unsigned _numStrands, unsigned _blockSize, bool _isGrouped)
    : inputBlockAllocator(_rowManager, _blockSize), outputBlockAllocator(_rowManager, _blockSize),
      numStrands(_numStrands), blockSize(_blockSize), isGrouped(_isGrouped)
    {
        strands = new OrderedStrand * [numStrands];
        for (unsigned i=0; i < numStrands; i++)
            strands[i] = new OrderedStrand(outputBlockAllocator);
        input = NULL;
        inputJunction.setown(new OrderedInputJunction(*this));
        outputJunction.setown(new OrderedOutputJunction(*this));
        curOutputStrand = 0;
        curOutputBlock = nullptr;
    }
    IMPLEMENT_IINTERFACE

    ~COrderedStrandBranch()
    {
        for (unsigned i=0; i < numStrands; i++)
            delete strands[i];
        delete [] strands;
    }

    void startStrandReaders()
    {
        for (unsigned i=0; i < numStrands; i++)
            asyncStart(*strands[i]);
    }

    void startInputReader()
    {
        asyncStart(*this);
    }

    virtual void main()
    {
        //Input processing code....
        if (isGrouped)
        {
            UNIMPLEMENTED;
            //MORE: Code needs to very similar to the code below, but try and gather multiple groups
            //and only move onto the next strand at the end of a group
        }
        else
        {
            unsigned curStrand = 0;
            bool done = false;
            while (!done)
            {
                RoxieRowBlock * block = inputBlockAllocator.newBlock();
                //MORE: This could more efficiently loop 0..blockSize and remove the test in addRowNowFull().  Also in other places.
                try
                {
                    for (;;)
                    {
                        const void * row = input->nextRow();
                        if (!row)
                        {
                            done = true;
                            break;
                        }
                        if (block->addRowNowFull(row))
                            break;
                    }
                }
                catch (IException * e)
                {
                    //MORE: Protect against exceptions, ensure exception is fed and processed by the strand.  (Otherwise read ahead may cause
                    //premature failure...
                    block->setExceptionOwn(e);
                    done = true;
                }

                block->setEndOfChunk();

                if (block->empty() || !strands[curStrand]->queryInputQueue().enqueue(block))
                {
                    block->releaseBlock();
                    break;
                }

                curStrand = curStrand+1;
                if (curStrand == numStrands)
                    curStrand = 0;
            }
        }
        for (unsigned i=0; i < numStrands; i++)
            strands[i]->queryInputQueue().noteWriterStopped();
    }

    void reset()
    {
        curOutputStrand = 0;
        curOutputBlock = nullptr;
    }

    const void * nextOutputRow()
    {
        loop
        {
            if (likely(curOutputBlock))
            {
                const void * result;
                if (curOutputBlock->nextRow(result))
                    return result;
                curOutputBlock->throwAnyPendingException();
                if (curOutputBlock->isEndOfChunk())
                {
                    curOutputStrand++;
                    if (curOutputStrand == numStrands)
                        curOutputStrand = 0;
                }
                curOutputBlock->releaseBlock();
                curOutputBlock = NULL;
            }

            if (!strands[curOutputStrand]->queryOutputQueue().dequeue(curOutputBlock))
            {
                //If there is no more output on the next strand, then all the strands will have finished processing.
                return NULL;
            }
        }
    }

    virtual void resetEOF()
    {
        throwUnexpectedX("resetEOF called on OrderedStrandBranch");
    }

    void setInput(IEngineRowStream * _input)
    {
        input = _input;
    }

    IEngineRowStream * queryStrandInput(unsigned n)
    {
        assertex(n <= numStrands);
        return strands[n]->queryStrandInput();
    }

    void setStrand(unsigned n, IEngineRowStream * stream)
    {
        assertex(n <= numStrands);
        strands[n]->setStrand(stream);
    }

    IEngineRowStream * queryOutput()
    {
        return this;
    }


 //interface IEngineRowStream
     virtual const void *nextRow()
     {
         return nextOutputRow();
     }

     virtual void stop() // problem - clashes with other interfaces???
     {
     }

//interface IStrandBranch
    virtual IStrandJunction * queryInputJunction()
    {
        return inputJunction;
    }

    virtual IStrandJunction * queryOutputJunction()
    {
        return outputJunction;
    }

protected:
    Owned<IStrandJunction> inputJunction;
    Owned<IStrandJunction> outputJunction;
    RowBlockAllocator inputBlockAllocator;
    RowBlockAllocator outputBlockAllocator;
    IEngineRowStream * input;
    OrderedStrand * * strands;
    RoxieRowBlock * curOutputBlock;
    unsigned numStrands;
    unsigned blockSize;
    unsigned curOutputStrand;
    bool isGrouped;
};

//---------------------------------------------------------------------------------------------------------------------

class CSingleStrandBranch : public CInterfaceOf<IStrandBranch>
{
public:
    CSingleStrandBranch()
    {
        input.setown(new OneToOneJunction());
        output.setown(new OneToOneJunction());
    }

    virtual IStrandJunction * queryInputJunction()
    {
        return input;
    }

    virtual IStrandJunction * queryOutputJunction()
    {
        return output;
    }

protected:
    Owned<IStrandJunction> input;
    Owned<IStrandJunction> output;
};

//---------------------------------------------------------------------------------------------------------------------

class CUnorderedStrandBranch : public CInterfaceOf<IStrandBranch>
{
public:
    CUnorderedStrandBranch(roxiemem::IRowManager & rowManager, unsigned _numStrands, unsigned _blockSize)
    {
        input.setown(createStrandJunction( rowManager, 1, _numStrands, _blockSize, false));
        output.setown(createStrandJunction( rowManager, _numStrands, 1, _blockSize, false));
    }

    virtual IStrandJunction * queryInputJunction()
    {
        return input;
    }

    virtual IStrandJunction * queryOutputJunction()
    {
        return output;
    }

protected:
    Owned<IStrandJunction> input;
    Owned<IStrandJunction> output;
};

//---------------------------------------------------------------------------------------------------------------------

extern THORHELPER_API IStrandBranch * createStrandBranch(roxiemem::IRowManager & rowManager, unsigned numStrands, unsigned blockSize, bool isOrdered, bool isGrouped)
{
    //Slightly inefficient to go via a junction, but makes the testing code simpler!
    assertex(numStrands);
    if (numStrands == 1)
        return new CSingleStrandBranch();
    if (isOrdered)
        return new COrderedStrandBranch(rowManager, numStrands, blockSize, isGrouped);
    else if (!isGrouped)
        return new CUnorderedStrandBranch(rowManager, numStrands, blockSize);
    UNIMPLEMENTED;
}

//---------------------------------------------------------------------------------------------------------------------


class BlockedRowStreamWriter : public CInterfaceOf<IRowWriterEx>
{
public:
    BlockedRowStreamWriter(IRowQueue * _queue, RowBlockAllocator & _allocator) : queue(_queue), allocator(_allocator)
    {
        curBlock = NULL;
    }

    virtual void putRow(const void *row)
    {
        if (!curBlock)
            curBlock = allocator.newBlock();
        if (curBlock->addRowNowFull(row))
        {
            if (!queue->enqueue(curBlock))
                curBlock->releaseBlock();
            curBlock = NULL;
        }
    }

    virtual void flush()
    {
        if (curBlock)
        {
            if (!queue->enqueue(curBlock))
                curBlock->releaseBlock();
            curBlock = NULL;
        }
    }

    virtual void noteStopped()
    {
        flush();
        queue->noteWriterStopped();
    }

protected:
    IRowQueue * queue;
    RowBlockAllocator & allocator;
    RoxieRowBlock * curBlock;
};

class UnorderedManyToOneRowStream : public CInterfaceOf<IManyToOneRowSteam>
{
public:
    UnorderedManyToOneRowStream(roxiemem::IRowManager & _rowManager, unsigned _numStrands, unsigned blockSize) : numStrands(_numStrands), allocator(_rowManager, blockSize)
    {
        const unsigned maxQueued = 60 - numStrands;
        queue.setown(createRowQueue(1, numStrands, maxQueued, 0));

        producers = new BlockedRowStreamWriter * [numStrands];
        for (unsigned i=0; i < numStrands; i++)
            producers[i] = new BlockedRowStreamWriter(queue, allocator);
        curBlock = NULL;
    }
    ~UnorderedManyToOneRowStream()
    {
        for (unsigned i=0; i < numStrands; i++)
            producers[i]->Release();
        delete [] producers;
    }

    virtual void abort()
    {
        queue->abort();
    }
    virtual void stop()
    {
        //MORE: What should this do?
        queue->abort();
    }
    virtual void reset()
    {
        queue->reset();

        loop
        {
            if (curBlock)
                curBlock->releaseBlock();
            const void * next;
            if (!queue->dequeue(next))
                break;
            curBlock = (RoxieRowBlock *)next;
        }
        curBlock = NULL;
    }
    virtual const void *nextRow()
    {
        const void * ret;
        loop
        {
            if (curBlock)
            {
                if (curBlock->nextRow(ret))
                    return ret;
                curBlock->releaseBlock();
                curBlock = NULL;
            }
            const void * next;
            if (!queue->dequeue(next))
                return NULL;
            curBlock = (RoxieRowBlock *)next;
        }
    }
    virtual IRowWriterEx * getWriter(unsigned n)
    {
        return LINK(producers[n]);
    }

protected:
    unsigned numStrands;
    Owned<IRowQueue> queue;
    RowBlockAllocator allocator;
    BlockedRowStreamWriter * * producers;
    RoxieRowBlock * curBlock;
};

//---------------------------------------------------------------------------------------------------------------------


extern THORHELPER_API IManyToOneRowSteam * createManyToOneRowStream(roxiemem::IRowManager & rowManager, unsigned numInputs, unsigned blockSize, bool isOrdered)
{
    if (!isOrdered)
        return new UnorderedManyToOneRowStream(rowManager, numInputs, blockSize);
    return NULL;
}
