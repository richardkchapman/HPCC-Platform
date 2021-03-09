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

#include "udplib.hpp"
#include "udpsha.hpp"
#include "jsocket.hpp"
#include "jlog.hpp"
#include "roxie.hpp"
#include "roxiemem.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

using roxiemem::DataBuffer;
using roxiemem::IDataBufferManager;

IDataBufferManager *bufferManager;

unsigned udpTraceLevel = 0;
unsigned udpTraceCategories = (unsigned) -1;
unsigned udpFlowSocketsSize = 131072;
unsigned udpLocalWriteSocketSize = 1024000;
unsigned udpSnifferReadThreadPriority = 3;
unsigned udpSnifferSendThreadPriority = 3;

unsigned multicastTTL = 1;

MODULE_INIT(INIT_PRIORITY_STANDARD)
{
    bufferManager = roxiemem::createDataBufferManager(roxiemem::DATA_ALIGNMENT_SIZE);
    return true;
}

MODULE_EXIT()
{ 
    bufferManager->Release();
}


const IpAddress ServerIdentifier::getIpAddress() const
{
    IpAddress ret;
    ret.setIP4(netAddress);
    return ret;
}

bool ServerIdentifier::isMe() const
{
    return *this==myNode;
}

ServerIdentifier myNode;

//---------------------------------------------------------------------------------------------

void queue_t::set_queue_size(unsigned _limit)
{
    limit = _limit;
    free_space.signal(limit);
}

queue_t::queue_t(unsigned _limit)
{
    set_queue_size(_limit);
}


queue_t::~queue_t() 
{
    while (head)
    {
        auto p = head;
        head = head->msgNext;
        ::Release(p);
    }
}

int queue_t::free_slots() 
{
    int res=0;
    while (res <= 0)
    {
        c_region.enter();
        res = limit - count;
        if (res <= 0)
            signal_free_sl++;
        c_region.leave();
        if (res <= 0)
        {
            while (!free_sl.wait(3000))
            {
                if (udpTraceLevel >= 1)
                    DBGLOG("queue_t::free_slots blocked for 3 seconds waiting for free_sl semaphore");
            }
        }
    }
    return res;
}

void queue_t::interrupt()
{
    data_avail.interrupt();
}

void queue_t::pushOwn(DataBuffer *buf)
{
    while (!free_space.wait(3000))
    {
        if (udpTraceLevel >= 1)
            DBGLOG("queue_t::pushOwn blocked for 3 seconds waiting for free_space semaphore, count == %d", count);
    }
    // Could probably be done lock-free, which given one thread using this is high priority might avoid some
    // potential priority-inversion issues. Or we might consider using PI-aware futexes here?
    {
        CriticalBlock b(c_region);
        assert(!tail->msgNext);
        assert(!buf->msgNext);
        if (tail)
            tail->msgNext = buf;
        else
        {
            assert(!head);
            head = buf;
        }
        tail = buf;
    }
    data_avail.signal();
}

DataBuffer *queue_t::pop(bool block)
{
    if (!data_avail.wait(block ? INFINITE : 0))
        return nullptr;
    DataBuffer *ret = nullptr;
    bool must_signal;
    {
        CriticalBlock b(c_region);
        if (!count)
            return nullptr;
        ret = head;
        head = head->msgNext;
        if (!head)
            tail = nullptr;
        ret->msgNext = nullptr;
        count--;
        must_signal = signal_free_sl>0;
        if (must_signal) 
            signal_free_sl--;
    }
    free_space.signal();
    if (must_signal) 
        free_sl.signal();
    return ret;
}


unsigned queue_t::removeData(const void *key, PKT_CMP_FUN pkCmpFn)
{
    unsigned removed = 0;
    unsigned signalFree = 0;
    unsigned signalFreeSlots = 0;
    {
        CriticalBlock b(c_region);
        if (count)
        {
            DataBuffer *prev = nullptr;
            DataBuffer *finger = head;
            while (finger)
            {
                if (!key || !pkCmpFn || pkCmpFn((const void*) finger, key))
                {
                    auto temp = finger;
                    finger = finger->next;
                    ::Release(temp);
                    if (prev==nullptr)
                    {
                        assert(head==temp);
                        head = finger;
                    }
                    else
                        prev->msgNext = finger;
                    if (temp==tail)
                        tail = prev;
                    signalFree++;
                    count--;
                    removed++;
                }
                else
                {
                    prev = finger;
                    finger = finger->next;
                }
            }
            if (signalFree && signal_free_sl)
            {
                signal_free_sl--;
                signalFreeSlots++;
            }
        }
    }
    if (signalFree)
        free_space.signal(signalFree);
    if (signalFreeSlots)
        free_sl.signal(signalFreeSlots);
    return removed;
}


bool queue_t::dataQueued(const void *key, PKT_CMP_FUN pkCmpFn)
{
    bool ret = false;
    CriticalBlock b(c_region);
    DataBuffer *finger = head;
    while (finger)
    {
        if (pkCmpFn((const void*) finger, key))
        {
            ret = true;
            break;
        }
    }
    return ret;
}


#ifndef _WIN32
#define HOSTENT hostent
#include <netdb.h>
#endif

int check_set(const char *path, int value)
{
#ifdef __linux__
    FILE *f = fopen(path,"r");
    char res[32];
    char *r = 0;
    int si = 0;
    if (f) {
        r = fgets(res, sizeof(res), f);
        fclose(f);
    }
    if (r)
        si = atoi(r);
    if (!si)
    {
        OWARNLOG("WARNING: Failed to read value for %s", path);
        return 0;
    }
    else if (si<value)
        return -1;
#endif
    return 0;
}

int check_max_socket_read_buffer(int size) {
    return check_set("/proc/sys/net/core/rmem_max", size);
}
int check_max_socket_write_buffer(int size) {
    return check_set("/proc/sys/net/core/wmem_max", size);
}

#if defined( __linux__) || defined(__APPLE__)
void setLinuxThreadPriority(int level)
{
    pthread_t self = pthread_self();
    int policy;
    sched_param param;
    int rc;
    if (( rc = pthread_getschedparam(self, &policy, &param)) != 0) 
        DBGLOG("pthread_getschedparam error: %d", rc);
    if (level < 0)
        UNIMPLEMENTED;
    else if (!level)
    {
        param.sched_priority = 0;
        policy = SCHED_OTHER;
    }
    else
    {
        policy = SCHED_RR;
        param.sched_priority = level;
    }
    if(( rc = pthread_setschedparam(self, policy, &param)) != 0) 
        DBGLOG("pthread_setschedparam error: %d policy=%i pr=%i id=%" I64F "i TID=%i", rc, policy, param.sched_priority, (unsigned __int64) self, threadLogID());
    else
        DBGLOG("priority set id=%" I64F "i policy=%i pri=%i TID=%i", (unsigned __int64) self, policy, param.sched_priority, threadLogID());
}
#endif


extern UDPLIB_API void queryMemoryPoolStats(StringBuffer &memStats)
{
    if (bufferManager)
        bufferManager->poolStats(memStats);
}


bool PacketTracker::noteSeen(UdpPacketHeader &hdr, std::atomic<int> &inflight)
{
    bool resent = false;
    sequence_t seq = hdr.sendSeq;
    if (hdr.pktSeq & UDP_PACKET_RESENT)
        resent = true;
    // Four cases: less than lastUnseen, equal to, within TRACKER_BITS of, or higher
    // Be careful to think about wrapping. Less than and higher can't really be distinguished, but we treat resent differently from original
    bool duplicate = false;
    unsigned delta = seq - base;
    if (udpTraceLevel > 5)
    {
        DBGLOG("PacketTracker::noteSeen %" SEQF "u: delta %d", hdr.sendSeq, delta);
        dump();
    }
    if (delta < TRACKER_BITS)
    {
        unsigned idx = seq % (TRACKER_BITS/64);
        unsigned bit = seq % 64;
        __uint64 bitm = 1l<<bit;
        duplicate = (seen[idx] & bitm) != 0;
        seen[idx] |= bitm;
        if (seq==base)
        {
            while (seen[idx] & bitm)
            {
                seen[idx] &= ~bitm;
                base++;
                idx = base % (TRACKER_BITS/64);
                bit = base % 64;
                bitm = 1l<<bit;
            }
        }
        if (!duplicate)
            inflight--;
    }
    else if (resent)
        // Don't treat a resend that goes out of range as indicative of a restart - it probably just means
        // that the resend was not needed and the original moved things on when it arrived
        duplicate = true;
    else
    {
        // We've gone forwards too far to track, or backwards because server restarted
        // We have taken steps to try to avoid the former...
        // In theory could try to preserve SOME information in the former case, but as it shouldn't happen, can we be bothered?
#ifdef _DEBUG
        DBGLOG("Received packet %" SEQF "u will cause loss of information in PacketTracker", seq);
        dump();
        //assert(false);
#endif
        base = seq+1;
        memset(seen, 0, sizeof(seen));
    }
    return duplicate;
}

const PacketTracker PacketTracker::copy() const
{
    // we don't want to put locks on the read or write, but we want to be able to read a consistent set of values
    // Probably needs some atomics...
    // Do we still need this loop now that consistency is generally better
    PacketTracker ret;
    for (;;)
    {
        ret.base = base;
        memcpy(ret.seen, seen, sizeof(seen));
        if (ret.base == base)
            return ret;
    }
}

bool PacketTracker::hasSeen(sequence_t seq) const
{
    // Careful about wrapping!
    unsigned delta = seq - base;
    if (udpTraceLevel > 5)
    {
       DBGLOG("PacketTracker::hasSeen - have I seen %u, %u", seq, delta);
       dump();
    }
    if (delta < TRACKER_BITS)
    {
        unsigned idx = seq % (TRACKER_BITS/64);
        unsigned bit = seq % 64;
        return seen[idx] & (1l<<bit);
    }
    else if (delta > std::numeric_limits<sequence_t>::max() / 2)  // Or we could just make delta a signed int? But code above would have to check >0
        return true;
    else
        return false;
}

void PacketTracker::dump() const
{
    DBGLOG("PacketTracker base=%" SEQF "u, seen[0]=%" I64F "x", base, seen[0]);
}

#ifdef _USE_CPPUNIT
#include "unittests.hpp"

class PacketTrackerTest : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(PacketTrackerTest);
        CPPUNIT_TEST(testNoteSeen);
    CPPUNIT_TEST_SUITE_END();

    void testNoteSeen()
    {
        std::atomic<int> inflight;
        PacketTracker p;
        UdpPacketHeader hdr;
        hdr.pktSeq = 0;
        // Some simple tests
        CPPUNIT_ASSERT(!p.hasSeen(0));
        CPPUNIT_ASSERT(!p.hasSeen(1));
        hdr.sendSeq = 0;
        CPPUNIT_ASSERT(!p.noteSeen(hdr, inflight));
        CPPUNIT_ASSERT(p.hasSeen(0));
        CPPUNIT_ASSERT(!p.hasSeen(1));
        CPPUNIT_ASSERT(!p.hasSeen(2000));
        CPPUNIT_ASSERT(!p.hasSeen(2001));
        hdr.pktSeq = UDP_PACKET_RESENT;
        CPPUNIT_ASSERT(p.noteSeen(hdr, inflight));
        hdr.pktSeq = 0;
        hdr.sendSeq = 2000;
        CPPUNIT_ASSERT(!p.noteSeen(hdr, inflight));
        CPPUNIT_ASSERT(p.hasSeen(0));
        CPPUNIT_ASSERT(p.hasSeen(1));
        CPPUNIT_ASSERT(p.hasSeen(2000));
        CPPUNIT_ASSERT(!p.hasSeen(2001));
        hdr.sendSeq = 0;
        CPPUNIT_ASSERT(!p.noteSeen(hdr, inflight));
        CPPUNIT_ASSERT(p.hasSeen(0));
        CPPUNIT_ASSERT(!p.hasSeen(1));
        CPPUNIT_ASSERT(!p.hasSeen(2000));
        CPPUNIT_ASSERT(!p.hasSeen(2001));

        PacketTracker p2;
        hdr.sendSeq = 1;
        CPPUNIT_ASSERT(!p2.noteSeen(hdr, inflight));
        CPPUNIT_ASSERT(!p2.hasSeen(0));
        CPPUNIT_ASSERT(p2.hasSeen(1));
        hdr.sendSeq = TRACKER_BITS-1;  // This is the highest value we can record without losing information
        CPPUNIT_ASSERT(!p2.noteSeen(hdr, inflight));
        CPPUNIT_ASSERT(!p2.hasSeen(0));
        CPPUNIT_ASSERT(p2.hasSeen(1));
        CPPUNIT_ASSERT(p2.hasSeen(TRACKER_BITS-1));
        CPPUNIT_ASSERT(!p2.hasSeen(TRACKER_BITS));
        CPPUNIT_ASSERT(!p2.hasSeen(TRACKER_BITS+1));
        hdr.sendSeq = TRACKER_BITS;
        p2.noteSeen(hdr, inflight);
        CPPUNIT_ASSERT(p2.hasSeen(0));
        CPPUNIT_ASSERT(p2.hasSeen(1));
        CPPUNIT_ASSERT(p2.hasSeen(TRACKER_BITS-1));
        CPPUNIT_ASSERT(p2.hasSeen(TRACKER_BITS));
        CPPUNIT_ASSERT(!p2.hasSeen(TRACKER_BITS+1));
        CPPUNIT_ASSERT(!p2.hasSeen(TRACKER_BITS+2));
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION( PacketTrackerTest );
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION( PacketTrackerTest, "PacketTrackerTest" );

#endif

/*
Crazy thoughts on network-wide flow control

Avoid sending data that clashes with other outbound or inbound data
    is outbound really an issue?
    if only inbound, should be easier
        can have each inbound node police its own, for a start
            udplib already tries to do this
        when sending permission to send, best to pick someone that is not sending to anyone else
            udplib already tries to do this
            but it can still lead to idleness - id node 1 sending to node 2, and node2 to node 1, node3 can't find anyone idle.


If you do need global:
  Every bit of data getting sent (perhaps over a certain size threshold?) gets permission from central traffic cop
  Outbound packet says source node, target node size
  Reply says source,target,size
  Cop allows immediately if nothing inflight between those pairs
  Cop assumes completion 
  Cop redundancy
   - a backup cop is listening in?
     - use multicast for requests and replies?
   - no reply implies what?
   - backup cop just needs heartbeat from active cop
   - permission expires
   - multiple cops for blocks of targets?
    - but I want global view of who is sending 


*/
