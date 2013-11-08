#include "jthread.hpp"
#include "jqueue.tpp"

unsigned iterations = 1000000;
//#define SINGLETHREAD

class FixedSizeCircularBuffer
{
public:
    FixedSizeCircularBuffer(unsigned _capacity)
    {
        capacity = _capacity;
        buffer = new const void *[capacity];
        head = tail = 0;
//        space.signal(capacity-1);
    }
    ~FixedSizeCircularBuffer()
    {
        delete[] buffer;
    }
    void enqueue(const void *item)
    {
//        space.wait();
        CriticalBlock b(lock);
        buffer[head] = item;
        head++;
        if (head==capacity)
            head = 0;
    }
    const void *dequeue()
    {
//        CriticalBlock b(lock);
        if (head != tail)
        {
            const void *ret = buffer[tail];
            tail++;
            if (tail==capacity)
                tail = 0;
//            space.signal();
            return ret;
        }
        return NULL;
    }
    unsigned ordinality()
    {
        return head - tail;  // Cheat by asserting meaningless if people still adding to it
    }
private:
    Semaphore space;
    CriticalSection lock;
    const void **buffer;
    unsigned head;
    unsigned tail;
    unsigned capacity;
};

//static SafeQueueOf<int, true> queue;
static FixedSizeCircularBuffer queue(4000000+2);
static atomic_t producer_count;
static atomic_t consumer_count;
static Semaphore available;
static bool done;

class Producer : public Thread
{
public:
    virtual int run()
    {
        for (int i = 0; i != iterations; ++i)
        {
            atomic_inc(&producer_count);
            queue.enqueue(NULL);
            available.signal();
        }
        return 0;
    }
};

class Consumer : public Thread
{
public:
    virtual int run()
    {
        loop
        {
//            available.wait();
            if (done && !queue.ordinality())
                break;
            queue.dequeue();
            atomic_inc(&consumer_count);
        }
        return 0;
    }
};

static bool singleThread = false;
static bool consumeConcurrently = true;

int main(int argc, const char**argv)
{
    if (argc > 1)
    {
        if (strcmp(argv[1], "-s")==0)
            singleThread = true;
        else if (strcmp(argv[1], "-c")==0)
            consumeConcurrently = false;
    }
    unsigned start = msTick();
    IArrayOf<Producer> producers;
    Consumer consumer;
    if (consumeConcurrently && !singleThread)
        consumer.start();
    for (unsigned i = 0; i < 4; i++)
    {
        Producer *producer = new Producer;
        if (singleThread)
            producer->run();
        else
            producer->start();
        producers.append(*producer);
    }
//    consumer.start();
    if (!singleThread)
        for (unsigned idx = 0; idx < 4; idx++)
            producers.item(idx).join();
    done = true;
    available.signal();
    if (singleThread)
        consumer.run();
    else
    {
        if (!consumeConcurrently)
            consumer.start();
        consumer.join();
    }
    printf ("%d produced\n", atomic_read(&producer_count));
    printf ("%d consumed\n", atomic_read(&consumer_count));
    printf ("%d elapsed\n", msTick() - start);
}
