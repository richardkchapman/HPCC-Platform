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
        unsigned _head = head;
        buffer[_head] = item;
        _head++;
        if (_head==capacity)
            _head = 0;
        head = _head;
    }
    const void *dequeue()
    {
//        CriticalBlock b(lock);
        unsigned _tail = tail;
        if (head != _tail)
        {
            const void *ret = buffer[_tail];
            _tail++;
            if (_tail==capacity)
                _tail = 0;
//            space.signal();
            tail = _tail;
            return ret;
        }
        return NULL;
    }
    inline unsigned ordinality()
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
static volatile bool done;
static bool useAvailable = false;

class Producer : public Thread
{
public:
    virtual int run()
    {
        bool _useAvailable = useAvailable;
        for (int i = 0; i != iterations; ++i)
        {
            queue.enqueue(NULL);
            if (_useAvailable)
                available.signal();
        }
        atomic_add(&producer_count, iterations);
        return 0;
    }
};

class Consumer : public Thread
{
public:
    virtual int run()
    {
        unsigned lcount = 0;
        bool _useAvailable = useAvailable;
        loop
        {
            if (_useAvailable)
                available.wait();
            if (done && !queue.ordinality())
                break;
            queue.dequeue();
            lcount++;
        }
        atomic_add(&consumer_count, lcount);
        return 0;
    }
};

static bool singleThread = false;
static bool consumeConcurrently = true;
static int numProducers = 4;

int main(int argc, const char**argv)
{
    for (int arg = 1; arg < argc; arg++)
    {
        if (strcmp(argv[arg], "-a")==0)
            useAvailable = true;
        else if (strcmp(argv[arg], "-s")==0)
            singleThread = true;
        else if (strcmp(argv[arg], "-c")==0)
            consumeConcurrently = false;
        else if (strncmp(argv[arg], "-n", 2)==0)
            numProducers = atoi(argv[arg]+2);
        else
        {
            printf("Unrecognized arg %s", argv[arg]);
            exit(2);
        }
    }
    unsigned start = msTick();
    IArrayOf<Producer> producers;
    Consumer consumer;
    if (consumeConcurrently && !singleThread)
        consumer.start();
    for (unsigned i = 0; i < numProducers; i++)
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
        for (unsigned idx = 0; idx < numProducers; idx++)
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
