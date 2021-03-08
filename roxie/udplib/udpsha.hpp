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

#ifndef updsha_include
#define updsha_include

#include "jmutex.hpp"
#include "roxiemem.hpp"
#include "jcrc.hpp"
#include <limits>

typedef unsigned sequence_t;
#define SEQF

extern roxiemem::IDataBufferManager *bufferManager;

typedef bool (*PKT_CMP_FUN) (const void *pkData, const void *key);


// Flag bits in pktSeq field
#define UDP_PACKET_COMPLETE           0x80000000  // Packet completes a single agent request
#define UDP_PACKET_RESENT             0x40000000  // Packet is a repeat of one that the server may have missed
#define UDP_PACKET_SEQUENCE_MASK      0x3fffffff

struct UdpPacketHeader
{
    unsigned short length;      // total length of packet including the header, data, and meta
    unsigned short metalength;  // length of metadata (comes after header and data)
    ServerIdentifier  node;        // Node this message came from
    unsigned       msgSeq;      // sequence number of messages ever sent from given node, used with ruid to tell which packets are from same message
    unsigned       pktSeq;      // sequence number of this packet within the message (top bit signifies final packet)
    sequence_t     sendSeq;     // sequence number of this packet among all those send from this node to this target
    // information below is duplicated in the Roxie packet header - we could remove? However, would make aborts harder, and at least ruid is needed at receive end
    ruid_t         ruid;        // The uid allocated by the server to this agent transaction
    unsigned       msgId;       // sub-id allocated by the server to this request within the transaction
};

#define USE_128BIT_TRACKER

class PacketTracker
{
    // Some more things we can consider:
    // 1. sendSeq gives us some insight into lost packets that might help is get inflight calcuation right (if it is still needed)
    // 2. If we can definitively declare that a packet is lost, we can fail that messageCollator earlier (and thus get the resend going earlier)
    // 3. Worth seeing why resend doesn't use same collator. We could skip sending (though would still need to calculate) the bit we already had...

private:
    sequence_t lastUnseen = 0;        // Sequence number of highest packet we have not yet seen
    unsigned __int64 seen1 = 0;        // bitmask representing whether we have seen (lastUnseen+n)
#ifdef USE_128BIT_TRACKER
    unsigned __int64 seen2 = 0;        // rest of bitmask
#endif
public:
    bool noteSeen(UdpPacketHeader &hdr)
    {
        bool resent = false;
        if (hdr.pktSeq & UDP_PACKET_RESENT)
        {
            resent = true;
            hdr.pktSeq &= ~UDP_PACKET_RESENT;
        }
        // Four cases: less than lastUnseen, equal to, within 64 of, or higher
        // Be careful to think about wrapping. Less than and higher can't really be distinguished, but we treat resent differently from original
        unsigned delta = hdr.sendSeq - lastUnseen;
        if (udpTraceLevel > 5)
        {
            DBGLOG("PacketTracker::noteSeen %" SEQF "u: delta %d", hdr.sendSeq, delta);
            dump();
        }
        if (delta==0)  // This should be the common case if packets are arriving reliably in order
        {
            for(;;)
            {
                lastUnseen++;
                bool nextSeen = (seen1 & 1) != 0;
                seen1 >>= 1;
#ifdef USE_128BIT_TRACKER
                unsigned __int64 carry = seen2 & 1;
                seen2 >>= 1;
                seen1 |= carry<<63;
#endif
                if (!nextSeen)
                    break;
            }
        }
        else if (delta <= 64)
            seen1 |= (1l<<(delta-1));
#ifdef USE_128BIT_TRACKER
        else if (delta <= 128)
            seen2 |= (1l<<(delta-65));
#endif
        else if (resent)
            return true;
        else
        {
            // We've gone forwards too far to track, or backwards because server restarted
            // We have taken steps to try to avoid the former...
            // In theory could try to preserve SOME information in the former case, but as it shouldn't happen, can we be bothered?
            lastUnseen = hdr.sendSeq;
            seen1 = 0;
#ifdef USE_128BIT_TRACKER
            seen2 = 0;
#endif
        }
        dump();
        return false;
    }
    const PacketTracker copy() const
    {
        // we don't want to put locks on the read or write, but we want to be able to read a consistent set of values
        // Probably needs some atomics...
        PacketTracker ret;
        for (;;)
        {
            ret.lastUnseen = lastUnseen;
            ret.seen1 = seen1;
#ifdef USE_128BIT_TRACKER
            ret.seen2 = seen2;
#endif
            if (ret.lastUnseen == lastUnseen)
                return ret;
        }
    }
    bool hasSeen(sequence_t seq) const
    {
        // Careful about wrapping!
        unsigned delta = seq - lastUnseen;
        if (udpTraceLevel > 5)
           DBGLOG("PacketTracker::hasSeen - have I seen %u, %u", seq, delta);
        dump();
        if (!delta)
            return false;
        else if (delta <= 64)
            return seen1 & (1l<<(delta-1));
#ifdef USE_128BIT_TRACKER
        else if (delta <= 128)
            return seen2 & (1l<<(delta-65));
#endif
        else if (delta > std::numeric_limits<sequence_t>::max() / 2)  // Or we could just make delta a signed int?
            return true;
        else
            return false;
    }

    // Would sending N new packets knock this packet off the recorded list?

    bool willBeLost(sequence_t seq, unsigned sendNew) const
    {
        assert(!hasSeen(seq));  // otherwise why even ask
#ifdef USE_128BIT_TRACKER
        assert(seq-lastUnseen <= 128);
#else
        assert(seq-lastUnseen <= 64);
#endif
        return lastUnseen + sendNew <= seq;
    }
    void dump() const
    {
#ifdef USE_128BIT_TRACKER
        DBGLOG("PacketTracker lastUnseen=%" SEQF "u, seen=%" I64F "x%" I64F "x", lastUnseen, seen1, seen2);
#else
        DBGLOG("PacketTracker lastUnseen=%" SEQF "u, seen=%" I64F "x", lastUnseen, seen1);
#endif
    }
};

class queue_t 
{

    class queue_element 
    {
    public:
        roxiemem::DataBuffer  *data;
        queue_element() 
        {
            data = NULL;
        }
    };

    queue_element   *elements;
    unsigned int    element_count;
    
    unsigned        first;
    unsigned        last;
    CriticalSection c_region;
    int             active_buffers;
    int             queue_size;
    InterruptableSemaphore data_avail;
    Semaphore       free_space;
    Semaphore       free_sl;
    unsigned        signal_free_sl;

    void removeElement(int ix);
    
public: 
    void interrupt();
    void pushOwn(roxiemem::DataBuffer *buffer);
    roxiemem::DataBuffer *pop(bool block);
    bool dataQueued(const void *key, PKT_CMP_FUN pkCmpFn);
    unsigned removeData(const void *key, PKT_CMP_FUN pkCmpFn);
    int  free_slots(); //block if no free slots
    void set_queue_size(unsigned int queue_size); //must be called immediately after constructor if default constructor is used
    queue_t(unsigned int queue_size);
    queue_t();
    ~queue_t();
    inline int capacity() const { return queue_size; }
};


template < class _et >
class simple_queue
{
    _et             *elements;
    unsigned int    element_count;
    int             first;
    int             last;
    int             active_buffers;
    CriticalSection c_region;
    Semaphore       data_avail;
    Semaphore       free_space;
    
public: 
    void push(const _et &element)
    {
        free_space.wait();
        c_region.enter();
        int next = (last + 1) % element_count;
        elements[last] = element;
        last = next;
        active_buffers++;
        c_region.leave();
        data_avail.signal();
    }
    
    bool push(const _et &element,long timeout)
    {
        if (free_space.wait(timeout) ) {
            c_region.enter();
            int next = (last + 1) % element_count;
            elements[last] = element;
            last = next;
            active_buffers++;
            c_region.leave();
            data_avail.signal();
            return true;
        }
        return false;
    }
    
    void pop (_et &element) 
    {
        data_avail.wait();
        c_region.enter();
        element = elements[first];
        first = (first + 1) % element_count;
        active_buffers--;
        c_region.leave();
        free_space.signal();
    }
    
    unsigned in_queue() {
        c_region.enter();
        unsigned res = active_buffers;
        c_region.leave();
        return res;
    }
    
    bool empty() 
    {
        c_region.enter();
        bool res = (active_buffers == 0);
        c_region.leave();
        return res;
    }

    simple_queue(unsigned int queue_size) 
    {
        element_count = queue_size;
        elements = new _et[element_count];
        free_space.signal(element_count);
        active_buffers = 0;
        first = 0;
        last = 0;
    }
    
    ~simple_queue() {
        delete [] elements;
    }

};

#ifndef _WIN32
#define HANDLE_PRAGMA_PACK_PUSH_POP
#endif

class flowType {
public:
    enum flowCmd : unsigned short { ok_to_send, request_received, request_to_send, send_completed, request_to_send_more };
    static const char *name(flowCmd m)
    {
        switch (m)
        {
        case ok_to_send: return "ok_to_send";
        case request_received: return "request_received";
        case request_to_send: return "request_to_send";
        case send_completed: return "send_completed";
        case request_to_send_more: return "request_to_send_more";
        default:
            assert(false);
            return "??";
        }
    };

};

class sniffType {
public:
    enum sniffCmd : unsigned short { busy, idle };
};

#pragma pack(push,1)
struct UdpPermitToSendMsg
{
    flowType::flowCmd cmd;
    unsigned short max_data;
    ServerIdentifier destNode;
    PacketTracker seen;
};

struct UdpRequestToSendMsg
{
    flowType::flowCmd cmd;
    unsigned short packets;
    ServerIdentifier sourceNode;
};

struct sniff_msg
{
    sniffType::sniffCmd cmd;
    ServerIdentifier nodeIp;
};
#pragma pack(pop)

int check_max_socket_read_buffer(int size);
int check_max_socket_write_buffer(int size);
int check_set_max_socket_read_buffer(int size);
int check_set_max_socket_write_buffer(int size);

#define TRACE_RETRY_DATA 0x08
#define TRACE_MSGPACK 0x10

inline bool checkTraceLevel(unsigned category, unsigned level)
{
    return (udpTraceLevel >= level);
}


#endif
