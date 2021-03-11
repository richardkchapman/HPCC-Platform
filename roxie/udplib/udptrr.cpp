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

#include <string>
#include <map>
#include <queue>

#include "jthread.hpp"
#include "jlog.hpp"
#include "jisem.hpp"
#include "jsocket.hpp"
#include "jencrypt.hpp"
#include "udplib.hpp"
#include "udptrr.hpp"
#include "udptrs.hpp"
#include "udpipmap.hpp"
#include "udpmsgpk.hpp"
#include "roxiemem.hpp"
#include "roxie.hpp"

#ifdef _WIN32
#include <io.h>
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include <thread>

using roxiemem::DataBuffer;
using roxiemem::IRowManager;

unsigned udpRetryBusySenders = 0;         // seems faster with 0 than 1 in my testing on small clusters and sustained throughput

static byte key[32] = {
    0xf7, 0xe8, 0x79, 0x40, 0x44, 0x16, 0x66, 0x18, 0x52, 0xb8, 0x18, 0x6e, 0x76, 0xd1, 0x68, 0xd3,
    0x87, 0x47, 0x01, 0xe6, 0x66, 0x62, 0x2f, 0xbe, 0xc1, 0xd5, 0x9f, 0x4a, 0x53, 0x27, 0xae, 0xa1,
};

class CReceiveManager : implements IReceiveManager, public CInterface
{
    /*
     * The ReceiveManager has several threads:
     * 1. receive_receive_flow (priority 3)
     *     - waits for packets on flow port
     *     - maintains list of nodes that have pending requests
     *     - sends ok_to_send to one sender at a time
     * 2. receive_sniffer (default priority 3, configurable)
     *     - waits for packets on sniffer port
     *     - updates information about what other node are currently up to
     *     - idea is to preferentially send "ok_to_send" to nodes that are not currently sending to someone else
     *     - doesn't run if no multicast
     *     - can I instead say "If I get a request to send and I'm sending to someone else, send a "later"?
     * 3. receive_data (priority 4)
     *     - reads data packets off data socket
     *     - runs at v. high priority
     *     - used to have an option to perform collation on this thread but a bad idea:
     *        - can block (ends up in memory manager via attachDataBuffer).
     *        - Does not apply back pressure
     *     - Just enqueues them. We don't give permission to send more than the queue can hold.
     * 4. PacketCollator (standard priority)
     *     - dequeues packets
     *     - collates packets
     *
     */

    /*
     * Handling lost packets
     *
     * We try to make lost packets unlikely by telling agents when to send (and making sure they don't send unless
     * there's a good chance that socket buffer will have room). But we can't legislate for network issues.
     *
     * What packets can be lost?
     * 1. Data packets - handled via retrying the whole query (not ideal). But will also leave the inflight count wrong. We correct it any time
     *    the data socket times out but that may not be good enough.
     * 2. RequestToSend - the sender's resend thread checks periodically. There's a short initial timeout for getting a reply (either "request_received"
     *    or "okToSend"), then a longer timeout for actually sending.
     * 3. OkToSend - there is a timeout after which the permission is considered invalid (based on how long it SHOULD take to send them).
     *    The requestToSend retry mechanism would then make sure retried.
     *    MORE - if I don't get a response from OkToSend I should assume lost and requeue it.
     * 4. complete - covered by same timeout as okToSend. A lost complete will mean incoming data to that node stalls for the duration of this timeout,
     *    and will also leave inflight count out-of-whack.
     * 4. Sniffers - expire anyway
     *
     */
    class UdpSenderEntry  // one per node in the system
    {
        // This is created the first time a message from a previously unseen IP arrives, and remains alive indefinitely
        // Note that the various members are accessed by different threads, but no member is accessed from more than one thread
        // (except where noted) so protection is not required

        // Note that UDP ordering rules mean we can't guarantee that we don't see a "request_to_send" for the next transfer before
        // we see the "complete" for the current one. Even if we were sure network stack would not reorder, these come from different
        // threads on the sender side and the order is not 100% guaranteed, so we need to cope with it.

        // We also need to recover gracefully (and preferably quickly) if any flow or data messages go missing. Currently the sender
        // will resend the rts if no ok_to_send within timeout, but there may be a better way?

    public:
        // Used only by receive_flow thread
        IpAddress dest;
        ISocket *flowSocket = nullptr;
        UdpSenderEntry *prevSender = nullptr;  // Used to form list of all senders that have outstanding requests
        UdpSenderEntry *nextSender = nullptr;  // Used to form list of all senders that have outstanding requests
        flowType::flowCmd state = flowType::send_completed;
        unsigned timeouts = 0;
        unsigned timeStamp = 0;                // When we last sent okToSend

        // Updated by receive_data thread, read atomically by receive_flow
        PacketTracker packetsSeen;

        // Only used by receive_data thread
        sequence_t prevSeq = 0;
        unsigned inflight = 0;

        UdpSenderEntry(const IpAddress &_dest, unsigned port) : dest(_dest)
        {
            SocketEndpoint ep(port, dest);
            flowSocket = ISocket::udp_connect(ep);
        }

        ~UdpSenderEntry()
        {
            if (flowSocket)
            {
                flowSocket->close();
                flowSocket->Release();
            }
        }

        inline void noteDone()
        {
            timeouts = 0;
        }

        inline bool retryOnTimeout()
        {
            ++timeouts;
            if (udpTraceLevel)
            {
                StringBuffer s;
                DBGLOG("Timed out %d times waiting for send_done from %s", timeouts, dest.getIpText(s).str());
            }
            if (udpMaxRetryTimedoutReqs && (timeouts >= udpMaxRetryTimedoutReqs))
            {
                if (udpTraceLevel)
                    DBGLOG("Abandoning");
                timeouts = 0;
                return false;
            }
            else
            {
                if (udpTraceLevel)
                    DBGLOG("Retrying");
                return true;
            }
        }

        int noteInFlight(unsigned packets, sequence_t seq)
        {
            // inflight tracks my best guess for how many packets are in flight from this sender
            // list packets and lost flow packets may this a little inexact, so try to make sure we self-correct
            // return value reflects how much our estimate changed based on the latest information
            unsigned oldInflight = inflight;
            int delta = seq-prevSeq;
            prevSeq = seq;
            if (delta > 0 && delta <= packets)   // check that it's reasonable, so we don't go crazy on lost flow packets or restarts
                inflight += delta;
            else
                inflight = 0;
            return inflight - oldInflight;
        }

        void acknowledgeRequest(const IpAddress &returnAddress)
        {
            try
            {
                UdpPermitToSendMsg msg;
                msg.cmd = flowType::request_received;
                msg.destNode = returnAddress;
                msg.max_data = 0;
                msg.seen = this->packetsSeen.copy();
                if (udpTraceLevel > 1)
                {
                    StringBuffer ipStr;
                    DBGLOG("UdpReceiver: sending request_received msg to node=%s", dest.getIpText(ipStr).str());
                }
                flowSocket->write(&msg, sizeof(UdpPermitToSendMsg));
            }
            catch(IException *e)
            {
                StringBuffer d, s;
                DBGLOG("UdpReceiver: acknowledgeRequest failed node=%s %s", dest.getIpText(d).str(), e->errorMessage(s).str());
                e->Release();
            }
        }

        void requestToSend(unsigned maxTransfer, const IpAddress &returnAddress)
        {
            try
            {
                UdpPermitToSendMsg msg;
                msg.cmd = maxTransfer ? flowType::ok_to_send : flowType::request_received;
                msg.destNode = returnAddress;
                msg.max_data = maxTransfer;
                msg.seen = this->packetsSeen.copy();
                if (udpTraceLevel > 1)
                {
                    StringBuffer ipStr;
                    DBGLOG("UdpReceiver: sending ok_to_send %d msg to node=%s", maxTransfer, dest.getIpText(ipStr).str());
                }
                flowSocket->write(&msg, sizeof(UdpPermitToSendMsg));
            }
            catch(IException *e)
            {
                StringBuffer d, s;
                DBGLOG("UdpReceiver: requestToSend failed node=%s %s", dest.getIpText(d).str(), e->errorMessage(s).str());
                e->Release();
            }
        }

    };

    class SenderList
    {
        UdpSenderEntry *head = nullptr;
        UdpSenderEntry *tail = nullptr;

        void checkListIsValid(UdpSenderEntry *lookfor)
        {
#ifdef _DEBUG
            UdpSenderEntry *prev = nullptr;
            UdpSenderEntry *finger = head;
            while (finger)
            {
                if (finger==lookfor)
                    lookfor = nullptr;
                prev = finger;
                finger = finger->nextSender;
            }
            assert(prev == tail);
            assert(lookfor==nullptr);
#endif
        }
    public:
        operator UdpSenderEntry *() const
        {
            return head;
        }
        void append(UdpSenderEntry *sender)
        {
            if (tail)
            {
                tail->nextSender = sender;
                sender->prevSender = tail;
                tail = sender;
            }
            else
            {
                head = tail = sender;
            }
            checkListIsValid(sender);
        }
        void remove(UdpSenderEntry *sender)
        {
            if (sender->prevSender)
                sender->prevSender->nextSender = sender->nextSender;
            else
                head = sender->nextSender;
            if (sender->nextSender)
                sender->nextSender->prevSender = sender->prevSender;
            else
                tail = sender->prevSender;
            sender->prevSender = nullptr;
            sender->nextSender = nullptr;
            checkListIsValid(nullptr);
        }
    };

    IpMapOf<UdpSenderEntry> sendersTable;

    class receive_receive_flow : public Thread 
    {
        CReceiveManager &parent;
        Owned<ISocket> flow_socket;
        const unsigned flow_port;
        const unsigned maxSlotsPerSender;
        std::atomic<bool> running = { false };
        
        SenderList pendingRequests;     // List of people wanting permission to send
        SenderList pendingPermits;      // List of people given permission to send

        void enqueueRequest(UdpSenderEntry *requester)
        {
            switch (requester->state)
            {
            case flowType::ok_to_send:
                pendingPermits.remove(requester);
                // Fall through
            case flowType::send_completed:
                pendingRequests.append(requester);
                requester->state = flowType::request_to_send;
                break;
            case flowType::request_to_send:
                // Perhaps the sender never saw our permission? Already on queue...
                break;
            default:
                // Unexpected state, should never happen!
                DBGLOG("ERROR: Unexpected state %s in enqueueRequest", flowType::name(requester->state));
                throwUnexpected();
                break;
            }
            requester->acknowledgeRequest(myNode.getIpAddress());  // Acknowledge receipt of the request

        }

        void okToSend(UdpSenderEntry *requester, unsigned slots)
        {
            switch (requester->state)
            {
            case flowType::request_to_send:
                pendingRequests.remove(requester);
                // Fall through
            case flowType::send_completed:
                pendingPermits.append(requester);
                requester->state = flowType::ok_to_send;
                break;
            case flowType::ok_to_send:
                // Perhaps the sender never saw our permission? Already on queue...
                break;
            default:
                // Unexpected state, should never happen!
                DBGLOG("ERROR: Unexpected state %s in enqueueRequest", flowType::name(requester->state));
                throwUnexpected();
                break;
            }
            requester->timeStamp = msTick();
            requester->requestToSend(slots, myNode.getIpAddress());
        }

        void noteDone(UdpSenderEntry *requester)
        {
            if (requester->state == flowType::ok_to_send)
            {
               pendingPermits.remove(requester);
               requester->state = flowType::send_completed;
            }
            else
            {
                StringBuffer s;
                DBGLOG("Received unexpected completed message (state is %s) from %s", flowType::name(requester->state), requester->dest.getIpText(s).str());
            }
        }

        unsigned slotsAvailable()
        {
            unsigned slots = parent.input_queue->available();
            if (slots>maxSlotsPerSender)
                slots = maxSlotsPerSender;
//          if (slots<minSlotsPerSender)
//              slots = 0;
            return slots;
        }
    public:
        receive_receive_flow(CReceiveManager &_parent, unsigned flow_p, unsigned _maxSlotsPerSender)
        : Thread("UdpLib::receive_receive_flow"), parent(_parent), flow_port(flow_p), maxSlotsPerSender(_maxSlotsPerSender)
        {
            if (check_max_socket_read_buffer(udpFlowSocketsSize) < 0) 
                throw MakeStringException(ROXIE_UDP_ERROR, "System Socket max read buffer is less than %i", udpFlowSocketsSize);
            flow_socket.setown(ISocket::udp_create(flow_port));
            flow_socket->set_receive_buffer_size(udpFlowSocketsSize);
            size32_t actualSize = flow_socket->get_receive_buffer_size();
            DBGLOG("UdpReceiver: receive_receive_flow created port=%d sockbuffsize=%d actual %d", flow_port, udpFlowSocketsSize, actualSize);
        }
        
        ~receive_receive_flow() 
        {
            running = false;
            if (flow_socket) 
                flow_socket->close();
            join();
        }

        virtual void start()
        {
            running = true;
            Thread::start();
        }

        virtual int run() override
        {
            DBGLOG("UdpReceiver: receive_receive_flow started");
        #ifdef __linux__
            setLinuxThreadPriority(3);
        #else
            adjustPriority(1);
        #endif
            UdpRequestToSendMsg msg;
            while (running)
            {
                unsigned timeout = 5000;
                try
                {
                    bool dataAvail = flow_socket->wait_read(timeout);
                    if (dataAvail)
                    {
                        const unsigned l = sizeof(msg);
                        unsigned int res ;
                        flow_socket->readtms(&msg, l, l, res, 0);
                        assert(res==l);
                        if (udpTraceLevel > 5)
                        {
                            StringBuffer ipStr;
                            DBGLOG("UdpReceiver: received %s msg from node=%s", flowType::name(msg.cmd), msg.sourceNode.getTraceText(ipStr).str());
                        }
                        UdpSenderEntry *sender = &parent.sendersTable[msg.sourceNode];
                        switch (msg.cmd)
                        {
                        case flowType::request_to_send:
                            enqueueRequest(sender);
                            break;

                        case flowType::send_completed:
                            noteDone(sender);
                            break;

                        case flowType::request_to_send_more:
                            noteDone(sender);
                            enqueueRequest(sender);
                            break;

                        default:
                            DBGLOG("UdpReceiver: received unrecognized flow control message cmd=%i", msg.cmd);
                        }
                    }
                    timeout = 5000;   // The default timeout is 5 seconds if nothing is waiting for response...
                    if (pendingPermits)
                    {
                        unsigned now = msTick();
                        for (UdpSenderEntry *finger = pendingPermits; finger != nullptr; )
                        {
                            if (now - finger->timeStamp >= 1000) //udpRequestToSendAckTimeout)
                            {
                                UdpSenderEntry *next = finger->nextSender;
                                pendingPermits.remove(finger);
                                pendingRequests.append(finger);
                                finger->state = flowType::request_received;  // Go to the back of the queue  - MORE - lets have some code to eventually give up! Or just give up here?
                                finger = next;
                            }
                            else
                            {
                                timeout = finger->timeStamp + 1000 - now; //udpRequestToSendAckTimeout
                                break;
                            }
                        }
                    }
                    for (UdpSenderEntry *finger = pendingRequests; finger != nullptr; finger = finger->nextSender)
                    {
                        unsigned slots = slotsAvailable();
                        if (!slots)
                        {
                            timeout = 1;   // Slots should free up very soon!
                            break;
                        }
                        okToSend(finger, slots);
                        if (timeout > udpRequestToSendAckTimeout)
                            timeout = udpRequestToSendAckTimeout;
                    }
                }
                catch (IException *e)
                {
                    if (running)
                    {
                        StringBuffer s;
                        DBGLOG("UdpReceiver: failed %i %s", flow_port, e->errorMessage(s).str());
                    }
                    e->Release();
                }
                catch (...)
                {
                    DBGLOG("UdpReceiver: receive_receive_flow::run unknown exception");
                }
            }
            return 0;
        }

    };

    class receive_data : public Thread 
    {
        CReceiveManager &parent;
        ISocket *receive_socket;
        std::atomic<bool> running = { false };
        Semaphore started;
        
    public:
        receive_data(CReceiveManager &_parent) : Thread("UdpLib::receive_data"), parent(_parent)
        {
            unsigned ip_buffer = parent.input_queue_size*DATA_PAYLOAD*2;
            if (ip_buffer < udpFlowSocketsSize) ip_buffer = udpFlowSocketsSize;
            if (check_max_socket_read_buffer(ip_buffer) < 0) 
                throw MakeStringException(ROXIE_UDP_ERROR, "System socket max read buffer is less than %u", ip_buffer);
            receive_socket = ISocket::udp_create(parent.data_port);
            receive_socket->set_receive_buffer_size(ip_buffer);
            size32_t actualSize = receive_socket->get_receive_buffer_size();
            DBGLOG("UdpReceiver: rcv_data_socket created port=%d requested sockbuffsize=%d actual sockbuffsize=%d", parent.data_port, ip_buffer, actualSize);
            running = false;
        }

        virtual void start()
        {
            running = true;
            Thread::start();
            started.wait();
        }
        
        ~receive_data()
        {
            running = false;
            if (receive_socket)
                receive_socket->close();
            join();
            ::Release(receive_socket);
        }

        virtual int run() 
        {
            DBGLOG("UdpReceiver: receive_data started");
        #ifdef __linux__
            setLinuxThreadPriority(4);
        #else
            adjustPriority(2);
        #endif
            DataBuffer *b = NULL;
            started.signal();
            MemoryBuffer encryptData;
            size32_t max_payload = DATA_PAYLOAD;
            void *encryptedBuffer = nullptr;
            if (parent.encrypted)
            {
                max_payload = DATA_PAYLOAD+16;  // AES function may add up to 16 bytes of padding
                encryptedBuffer = encryptData.reserveTruncate(max_payload);
            }
            while (running) 
            {
                try 
                {
                    unsigned int res;
                    b = bufferManager->allocate();
                    if (parent.encrypted)
                    {
                        receive_socket->read(encryptedBuffer, 1, max_payload, res, 5);
                        res = aesDecrypt(key, sizeof(key), encryptedBuffer, res, b->data, DATA_PAYLOAD);
                        // MORE - Not here - this is the high priority socket reader thread
                    }
                    else
                        receive_socket->read(b->data, 1, DATA_PAYLOAD, res, 5);

                    UdpPacketHeader &hdr = *(UdpPacketHeader *) b->data;
                    assert(hdr.length == res && hdr.length > sizeof(hdr));
                    UdpSenderEntry *sender = &parent.sendersTable[hdr.node];
                    if (sender->packetsSeen.noteSeen(hdr, parent.inflight))
                    {
                        if (udpTraceLevel > 5) // don't want to interrupt this thread if we can help it
                        {
                            StringBuffer s;
                            DBGLOG("UdpReceiver: discarding unwanted resent packet %" SEQF "u %u from %s", hdr.sendSeq, hdr.pktSeq, hdr.node.getTraceText(s).str());
                        }
                        unwantedDiscarded++;
                        ::Release(b);
                    }
                    else
                    {
                        if (udpTraceLevel > 5) // don't want to interrupt this thread if we can help it
                        {
                            StringBuffer s;
                            DBGLOG("UdpReceiver: %u bytes received, node=%s", res, hdr.node.getTraceText(s).str());
                        }
                        parent.input_queue->pushOwn(b);
                    }
                    b = NULL;
                }
                catch (IException *e) 
                {
                    ::Release(b);
                    b = NULL;
                    if (udpTraceLevel > 1 && parent.inflight)
                    {
                        DBGLOG("resetting inflight to 0 (was %d)", parent.inflight.load(std::memory_order_relaxed));
                    }
                    parent.inflight = 0;
                    if (running && e->errorCode() != JSOCKERR_timeout_expired)
                    {
                        StringBuffer s;
                        DBGLOG("UdpReceiver: receive_data::run read failed port=%u - Exp: %s", parent.data_port,  e->errorMessage(s).str());
                        MilliSleep(1000); // Give a chance for mem free
                    }
                    e->Release();
                }
                catch (...) 
                {
                    ::Release(b);
                    b = NULL;
                    DBGLOG("UdpReceiver: receive_data::run unknown exception port %u", parent.data_port);
                    MilliSleep(1000);
                }
            }
            ::Release(b);
            return 0;
        }
    };

    class CPacketCollator : public Thread
    {
        CReceiveManager &parent;
    public:
        CPacketCollator(CReceiveManager &_parent) : Thread("CPacketCollator"), parent(_parent) {}

        virtual int run() 
        {
            DBGLOG("UdpReceiver: CPacketCollator::run");
            parent.collatePackets();
            return 0;
        }
    } collatorThread;


    friend class receive_receive_flow;
    friend class receive_send_flow;
    friend class receive_data;
    friend class ReceiveFlowManager;
    friend class receive_sniffer;
    
    queue_t              *input_queue;
    int                  input_queue_size;
    receive_receive_flow *receive_flow;
    receive_data         *data;
    
    int                  receive_flow_port;
    int                  data_port;

    std::atomic<bool> running = { false };
    bool encrypted = false;

    typedef std::map<ruid_t, CMessageCollator*> uid_map;
    uid_map         collators;
    SpinLock collatorsLock; // protects access to collators map
    // inflight is my best guess at how many packets may be sitting in socket buffers somewhere.
    // Incremented when I am notified about packets having been sent, decremented as they are read off the socket.
    // Will be -ve if send_done messages have been lost, and during the gap between ok_to_send and send_done
    std::atomic<int> inflight = {0};

    int free_slots()
    {
        int free = input_queue->free_slots();  // May block if collator thread is not removing from my queue fast enough (priority-inversion problem!)
        if (false)  // useInflightEstimation)
        {
            // Ignore inflight if negative (can happen because we read some inflight before we see the send_done)
            int i = inflight.load(std::memory_order_relaxed);
            if (i < 0)
            {
                if (i < -input_queue->capacity())
                {
                    if (udpTraceLevel)
                        DBGLOG("UdpReceiver: ERROR: inflight has more packets in queue but not counted (%d) than queue capacity (%d)", -i, input_queue->capacity());  // Should never happen unless losing flow packets
                    inflight = -input_queue->capacity();
                }
                i = 0;
            }
            else if (i >= free)
            {
                if ((i > free) && (udpTraceLevel))
                    DBGLOG("UdpReceiver: ERROR: more packets in flight (%d) than slots free (%d)", i, free);  // Should never happen unless losing data packets
                inflight = i = free-1;
            }
            if (i && udpTraceLevel > 1)
                DBGLOG("UdpReceiver: adjusting free_slots to allow for %d in flight", i);
            assert(free-i > 0) ;
            return free - i;
        }
        else
            return free;
        //return 1;   // Provoke pathogenic behaviour
    }

    public:
    IMPLEMENT_IINTERFACE;
    CReceiveManager(int server_flow_port, int d_port, int client_flow_port, int snif_port, const IpAddress &multicast_ip, int queue_size, int m_slot_pr_client, bool _encrypted)
        : collatorThread(*this), sendersTable([client_flow_port](const ServerIdentifier &ip) { return new UdpSenderEntry(ip.getIpAddress(), client_flow_port);})
    {
#ifndef _WIN32
        setpriority(PRIO_PROCESS, 0, -15);
#endif
        encrypted = _encrypted;
        receive_flow_port = server_flow_port;
        data_port = d_port;
        input_queue_size = queue_size;
        input_queue = new queue_t(queue_size);
        data = new receive_data(*this);
        receive_flow = new receive_receive_flow(*this, server_flow_port, m_slot_pr_client);

        running = true;
        collatorThread.start();
        data->start();
        receive_flow->start();
        MilliSleep(15);
    }

    ~CReceiveManager() 
    {
        running = false;
        input_queue->interrupt();
        collatorThread.join();
        delete data;
        delete receive_flow;
        delete input_queue;
    }

    virtual void detachCollator(const IMessageCollator *msgColl) 
    {
        ruid_t ruid = msgColl->queryRUID();
        if (udpTraceLevel >= 2) DBGLOG("UdpReceiver: detach %p %u", msgColl, ruid);
        {
            SpinBlock b(collatorsLock);
            collators.erase(ruid);
        }
        msgColl->Release();
    }

    void collatePackets()
    {
        while(running) 
        {
            DataBuffer *dataBuff = input_queue->pop(true);
            collatePacket(dataBuff);
        }
    }

    void collatePacket(DataBuffer *dataBuff)
    {
        const UdpPacketHeader *pktHdr = (UdpPacketHeader*) dataBuff->data;

        if (udpTraceLevel >= 4) 
        {
            StringBuffer s;
            DBGLOG("UdpReceiver: CPacketCollator - unQed packet - ruid=" RUIDF " id=0x%.8X mseq=%u pkseq=0x%.8X len=%d node=%s",
                pktHdr->ruid, pktHdr->msgId, pktHdr->msgSeq, pktHdr->pktSeq, pktHdr->length, pktHdr->node.getTraceText(s).str());
        }

        Linked <CMessageCollator> msgColl;
        bool isDefault = false;
        {
            SpinBlock b(collatorsLock);
            try
            {
                msgColl.set(collators[pktHdr->ruid]);
                if (!msgColl)
                {
                    msgColl.set(collators[RUID_DISCARD]);
                    isDefault = true;
                    unwantedDiscarded++;
                }
            }
            catch (IException *E)
            {
                EXCLOG(E);
                E->Release();
            }
            catch (...)
            {
                IException *E = MakeStringException(ROXIE_INTERNAL_ERROR, "Unexpected exception caught in CPacketCollator::run");
                EXCLOG(E);
                E->Release();
            }
        }
        if (udpTraceLevel && isDefault)
        {
            StringBuffer s;
            DBGLOG("UdpReceiver: CPacketCollator NO msg collator found - using default - ruid=" RUIDF " id=0x%.8X mseq=%u pkseq=0x%.8X node=%s", pktHdr->ruid, pktHdr->msgId, pktHdr->msgSeq, pktHdr->pktSeq, pktHdr->node.getTraceText(s).str());
        }
        if (msgColl && msgColl->attach_databuffer(dataBuff))
            dataBuff = nullptr;
        else
            dataBuff->Release();
    }

    virtual IMessageCollator *createMessageCollator(IRowManager *rowManager, ruid_t ruid)
    {
        CMessageCollator *msgColl = new CMessageCollator(rowManager, ruid);
        if (udpTraceLevel >= 2)
            DBGLOG("UdpReceiver: createMessageCollator %p %u", msgColl, ruid);
        {
            SpinBlock b(collatorsLock);
            collators[ruid] = msgColl;
        }
        msgColl->Link();
        return msgColl;
    }
};

IReceiveManager *createReceiveManager(int server_flow_port, int data_port, int client_flow_port,
                                      int sniffer_port, const IpAddress &sniffer_multicast_ip,
                                      int udpQueueSize, unsigned maxSlotsPerSender,
                                      bool encrypted)
{
    assertex (maxSlotsPerSender <= (unsigned) udpQueueSize);
    return new CReceiveManager(server_flow_port, data_port, client_flow_port, sniffer_port, sniffer_multicast_ip, udpQueueSize, maxSlotsPerSender, encrypted);
}

/*
Thoughts on flow control / streaming:
1. The "continuation packet" mechanism does have some advantages
    - easy recovery from agent failures
    - agent recovers easily from Roxie server failures
    - flow control is simple (but is it effective?)

2. Abandoning continuation packet in favour of streaming would give us the following issues:
    - would need some flow control to stop getting ahead of a Roxie server that consumed slowly
        - flow control is non trivial if you want to avoid tying up a agent thread and want agent to be able to recover from Roxie server failure
    - Need to work out how to do GSS - the nextGE info needs to be passed back in the flow control?
    - can't easily recover from agent failures if you already started processing
        - unless you assume that the results from agent are always deterministic and can retry and skip N
    - potentially ties up a agent thread for a while 
        - do we need to have a larger thread pool but limit how many actually active?

3. Order of work
    - Just adding streaming while ignoring flow control and continuation stuff (i.e. we still stop for permission to continue periodically)
        - Shouldn't make anything any _worse_ ...
            - except that won't be able to recover from a agent dying mid-stream (at least not without some considerable effort)
                - what will happen then?
            - May also break server-side caching (that no-one has used AFAIK). Maybe restrict to nohits as we change....
    - Add some flow control
        - would prevent agent getting too far ahead in cases that are inadequately flow-controlled today
        - shouldn't make anything any worse...
    - Think about removing continuation mechanism from some cases

Per Gavin, streaming would definitely help for the lowest frequency term.  It may help for the others as well if it avoided any significant start up costs - e.g., opening the indexes,
creating the segment monitors, creating the various cursors, and serialising the context (especially because there are likely to be multiple cursors).

To add streaming:
    - Need to check for meta availability other than when first received
        - when ? 
    - Need to cope with a getNext() blocking without it causing issues
     - perhaps should recode getNext() of variable-size rows first?

More questions:
    - Can we afford the memory for the resend info?
        - Save maxPacketsPerSender per sender ?
    - are we really handling restart and sequence wraparound correctly?
    - what about server-side caching? Makes it hard
        - but maybe we should only cache tiny replies anyway....

Problems found while testing implemetnation:
    - the unpacker cursor read code is crap
    - there is a potential to deadlock when need to make a callback agent->server during a streamed result (indexread5 illustrates)
        - resolution callback code doesn't really need to be query specific - could go to the default handler
        - but other callbacks - ALIVE, EXCEPTION, and debugger are not so clear
    - It's not at all clear where to move the code for processing metadata
    - callback paradigm would solve both - but it has to be on a client thread (e.g. from within call to next()).

    The following are used in "pseudo callback" mode:
    #define ROXIE_DEBUGREQUEST 0x3ffffff7u
    #define ROXIE_DEBUGCALLBACK 0x3ffffff8u
    #define ROXIE_PING 0x3ffffff9u
        - goes to own handler anyway
    #define ROXIE_TRACEINFO 0x3ffffffau
        - could go in meta? Not time critical. Could all go to single handler? (a bit hard since we want to intercept for caller...)
    #define ROXIE_FILECALLBACK 0x3ffffffbu
        - could go to single handler
    #define ROXIE_ALIVE 0x3ffffffcu
        - currently getting delayed a bit too much potentially if downstream processing is slow? Do I even need it if streaming?
    #define ROXIE_KEYEDLIMIT_EXCEEDED 0x3ffffffdu
        - could go in metadata of standard response
    #define ROXIE_LIMIT_EXCEEDED 0x3ffffffeu
        - ditto
    #define ROXIE_EXCEPTION   0x3fffffffu
        - ditto
    And the continuation metadata.

    What if EVERYTHING was a callback? - here's an exception... here's some more rows... here's some tracing... here's some continuation metadata
    Somewhere sometime I need to marshall from one thread to another though (maybe more than once unless I can guarantee callback is always very fast)

    OR (is it the same) everything is metadata ? Metadata can contain any of the above information (apart from rows - or maybe they are just another type)
    If I can't deal quickly with a packet of information, I queue it up? Spanning complicates things though. I need to be able to spot complete portions of metadata
    (and in kind-of the same way I need to be able to spot complete rows of data even when they span multiple packets.) I think data is really a bit different from the rest -
    you expect it to be continuous and you want the others to interrupt the flow.

    If continuation info was restricted to a "yes/no" (i.e. had to be continued on same node as started on) could have simple "Is there any continuation" bit. Others are sent in their 
    own packets so are a little different. Does that make it harder to recover? Not sure that it does really (just means that the window at which a failure causes a problem starts earlier).
    However it may be an issue tying up agent thread for a while (and do we know when to untie it if the Roxie server abandons/restarts?)

    Perhaps it makes sense to pause at this point (with streaming disabled and with retry mechanism optional)





*/
