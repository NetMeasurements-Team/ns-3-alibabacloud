#include "rdma-queue-pair.h"

#include "rdma-congestion-ops.h"
#include <ns3/hash.h>
#include <ns3/ipv4-header.h>
#include <ns3/seq-ts-header.h>
#include <ns3/simulator.h>
#include <ns3/udp-header.h>
#include <ns3/uinteger.h>

#include <algorithm>
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif

namespace ns3
{
NS_LOG_COMPONENT_DEFINE("RdmaQueuePair");
NS_OBJECT_ENSURE_REGISTERED(RdmaQueuePair);

/**************************
 * RdmaQueuePair
 *************************/
TypeId
RdmaQueuePair::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::RdmaQueuePair").SetParent<Object>();
    return tid;
}

RdmaQueuePair::RdmaQueuePair(uint16_t pg,
                             Ipv4Address _sip,
                             Ipv4Address _dip,
                             uint16_t _sport,
                             uint16_t _dport)
{
    startTime = Simulator::Now();
    sip = _sip;
    dip = _dip;
    sport = _sport;
    dport = _dport;
    m_src = -1;
    m_dest = -1;
    snd_nxt = snd_una = 0;
    m_pg = pg;
    m_ipid = 0;
    m_win = 0;
    m_baseRtt = 0;
    m_rtoEnabled = false;
    m_max_rate = 0;
    m_var_win = false;
    m_rate = 0;
    m_nextAvail = Time(0);
    lastPktSize = 0;
}

void
RdmaQueuePair::SetSrc(uint32_t src)
{
    m_src = src;
}

void
RdmaQueuePair::SetDest(uint32_t dest)
{
    m_dest = dest;
}

uint32_t
RdmaQueuePair::GetSrc()
{
    return m_src;
}

uint32_t
RdmaQueuePair::GetDest()
{
    return m_dest;
}

void
RdmaQueuePair::SetWin(uint32_t win)
{
    m_win = win;
}

void
RdmaQueuePair::SetBaseRtt(uint64_t baseRtt)
{
    m_baseRtt = baseRtt;
}

void
RdmaQueuePair::SetVarWin(bool v)
{
    m_var_win = v;
}

void
RdmaQueuePair::SetAppNotifyCallback(Callback<void> notifyAppFinish)
{
    m_notifyAppFinish = notifyAppFinish;
}

void
RdmaQueuePair::SetAppSentCallback(Callback<void> notifyAppSent)
{
    m_notifyAppSent = notifyAppSent;
}

uint64_t
RdmaQueuePair::GetTotalEndSeq()
{
    // End sequence considering both pending and in-flight messages
    uint64_t endSeq = snd_nxt;

    if (!m_inFlightMessages.empty())
    {
        const RdmaMessage& lastInFlight = m_inFlightMessages.back();
        endSeq = std::max(endSeq, lastInFlight.m_startSeq + lastInFlight.m_size);
    }

    if (!m_pendingMessages.empty())
    {
        const RdmaMessage& lastPending = m_pendingMessages.back();
        endSeq = std::max(endSeq, lastPending.m_startSeq + lastPending.m_size);
    }

    return endSeq;
}

uint64_t
RdmaQueuePair::GetBytesLeft()
{
    // Total bytes left to send from snd_nxt, across pending AND in-flight
    // messages (in-flight matters during recovery, when snd_nxt is rewound).
    const uint64_t totalEnd = GetTotalEndSeq();
    return totalEnd > snd_nxt ? totalEnd - snd_nxt : 0;
}

uint32_t
RdmaQueuePair::GetHash(void)
{
    union {
        struct
        {
            uint32_t sip, dip;
            uint16_t sport, dport;
        };

        char c[12];
    } buf;

    buf.sip = sip.Get();
    buf.dip = dip.Get();
    buf.sport = sport;
    buf.dport = dport;
    return Hash32(buf.c, 12);
}

bool
RdmaQueuePair::Acknowledge(uint64_t ack)
{
    if (ack > snd_una)
    {
        snd_una = ack;
        // If the cumulative ACK jumps ahead of our sending counter (RTO / go-back-N),
        // pull snd_nxt forward so pending messages can be moved to in-flight.
        if (snd_nxt < snd_una)
        {
            snd_nxt = snd_una;
        }
        return true;
    }
    return false;
}

uint64_t
RdmaQueuePair::GetOnTheFly()
{
    if (snd_nxt < snd_una)
    {
        uint64_t flow_id = 0;
        if (!m_pendingMessages.empty())
        {
            flow_id = m_pendingMessages.front().m_flow_id;
        }
        else if (!m_inFlightMessages.empty())
        {
            flow_id = m_inFlightMessages.front().m_flow_id;
        }
        printf("WARN flow %lu, snd_nxt %lu, snd_una %lu, at %lu\n",
               flow_id,
               snd_nxt,
               snd_una,
               Simulator::Now().GetTimeStep());
    }
    return snd_nxt - snd_una;
}

bool
RdmaQueuePair::IsWinBound()
{
    uint64_t w = GetWin();
    return w != 0 && GetOnTheFly() >= w;
}

uint64_t
RdmaQueuePair::GetWin()
{
    if (m_win == 0)
    {
        return 0;
    }
    uint64_t w;
    if (m_var_win)
    {
        w = m_win * m_rate.GetBitRate() / m_max_rate.GetBitRate();
        if (w == 0)
        {
            w = 1; // must > 0
        }
    }
    else
    {
        w = m_win;
    }
    return w;
}

bool
RdmaQueuePair::IsFinished()
{
    // finished when nothing is left to send AND nothing is waiting for an ACK
    return m_pendingMessages.empty() && m_inFlightMessages.empty();
}

void
RdmaQueuePair::UpdateRate()
{
    m_rate = m_congestionControl->m_ccRate;
}

void
RdmaQueuePair::PushMessage(uint64_t size,
                           uint64_t flow_id,
                           uint64_t tag,
                           Callback<void> notifyAppFinish,
                           Callback<void> notifyAppSent)
{
    RdmaMessage msg;
    msg.m_size = size;
    msg.m_flow_id = flow_id;
    msg.m_tag = tag;
    msg.m_sent = false;
    msg.m_appNotified = false;
    msg.m_notifyAppFinish = notifyAppFinish;
    msg.m_notifyAppSent = notifyAppSent;

    // The start sequence goes right after all existing pending and in-flight
    // messages, so the QP is one continuous byte stream.
    uint64_t startSeq;
    if (!m_pendingMessages.empty())
    {
        const RdmaMessage& lastPending = m_pendingMessages.back();
        startSeq = lastPending.m_startSeq + lastPending.m_size;
    }
    else if (!m_inFlightMessages.empty())
    {
        const RdmaMessage& lastInFlight = m_inFlightMessages.back();
        startSeq = lastInFlight.m_startSeq + lastInFlight.m_size;
    }
    else
    {
        // No messages at all - start from current snd_nxt
        startSeq = snd_nxt;
        m_lastMessageStartTime = Simulator::Now();
    }

    msg.m_startSeq = startSeq;
    m_pendingMessages.push_back(msg);
}

RdmaQueuePair::RdmaMessage
RdmaQueuePair::MoveCurrentToInFlight()
{
    NS_ASSERT(!m_pendingMessages.empty());

    RdmaMessage msg = m_pendingMessages.front();
    m_pendingMessages.pop_front();
    msg.m_sent = true;

    // keep it for ACK tracking / retransmission
    m_inFlightMessages.push_back(msg);

    // the next pending message starts now
    if (!m_pendingMessages.empty())
    {
        m_lastMessageStartTime = Simulator::Now();
    }

    return msg;
}

void
RdmaQueuePair::FinishMessage()
{
    if (!m_inFlightMessages.empty())
    {
        // Copy and pop BEFORE calling the callback: the callback may push new
        // messages or inspect IsFinished(), and must not see the finished one.
        const RdmaMessage msg = m_inFlightMessages.front();
        m_inFlightMessages.pop_front();
        if (!msg.m_notifyAppFinish.IsNull())
        {
            msg.m_notifyAppFinish();
        }
    }
    else
    {
        NS_LOG_ERROR("RdmaQueuePair::FinishMessage(): in-flight queue is empty but try to finish");
    }
}

bool
RdmaQueuePair::IsCurMessageFullySent()
{
    if (m_pendingMessages.empty())
    {
        return false;
    }
    const RdmaMessage& msg = m_pendingMessages.front();
    return snd_nxt >= msg.m_startSeq + msg.m_size;
}

bool
RdmaQueuePair::IsOldestInFlightAcked()
{
    if (m_inFlightMessages.empty())
    {
        return false;
    }
    const RdmaMessage& msg = m_inFlightMessages.front();
    return snd_una >= msg.m_startSeq + msg.m_size;
}

/*********************
 * RdmaRxQueuePair
 ********************/

NS_OBJECT_ENSURE_REGISTERED(RdmaRxQueuePair);

TypeId
RdmaRxQueuePair::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::RdmaRxQueuePair").SetParent<Object>();
    return tid;
}

RdmaRxQueuePair::RdmaRxQueuePair()
{
    sip = dip = sport = dport = 0;
    m_ipid = 0;
    ReceiverNextExpectedSeq = 0;
    m_nackTimer = Time(0);
    m_milestone_rx = 0;
    m_lastNACK = 0;
}

void
RdmaRxQueuePair::PushMessage(uint64_t size, uint64_t flow_id, uint64_t tag)
{
    // Back-to-back on the receive side as well: each message starts where the
    // previous queued one ends (std::queue::back() is available).
    RdmaMessage msg;
    msg.m_size = size;
    msg.m_flow_id = flow_id;
    msg.m_tag = tag;
    if (m_messages.empty())
    {
        msg.m_startSeq = ReceiverNextExpectedSeq;
    }
    else
    {
        msg.m_startSeq = m_messages.back().m_startSeq + m_messages.back().m_size;
    }
    m_messages.push(msg);
}

uint32_t
RdmaRxQueuePair::GetHash(void)
{
    union {
        struct
        {
            uint32_t sip, dip;
            uint16_t sport, dport;
        };

        char c[12];
    } buf;

    buf.sip = sip;
    buf.dip = dip;
    buf.sport = sport;
    buf.dport = dport;
    return Hash32(buf.c, 12);
}

/*********************
 * RdmaQueuePairGroup
 ********************/

NS_OBJECT_ENSURE_REGISTERED(RdmaQueuePairGroup);

TypeId
RdmaQueuePairGroup::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::RdmaQueuePairGroup").SetParent<Object>();
    return tid;
}

RdmaQueuePairGroup::RdmaQueuePairGroup(void)
{
}

uint32_t
RdmaQueuePairGroup::GetN(void)
{
#ifdef NS3_MTP
    MtpInterface::CriticalSection cs;
#endif
    return m_qps.size();
}

Ptr<RdmaQueuePair>
RdmaQueuePairGroup::operator[](uint32_t idx)
{
#ifdef NS3_MTP
    MtpInterface::CriticalSection cs;
#endif
    return m_qps[idx];
}

Ptr<RdmaQueuePair>
RdmaQueuePairGroup::Get(uint32_t idx)
{
    return (*this)[idx];
}

void
RdmaQueuePairGroup::AddQp(Ptr<RdmaQueuePair> qp)
{
#ifdef NS3_MTP
    MtpInterface::CriticalSection cs;
#endif
    m_qps.push_back(qp);
}

void
RdmaQueuePairGroup::RemoveQp(Ptr<RdmaQueuePair> qp)
{
#ifdef NS3_MTP
    MtpInterface::CriticalSection cs;
#endif
    if (const auto it = std::find(m_qps.begin(), m_qps.end(), qp); it != m_qps.end())
    {
        m_qps.erase(it);
    }
}

#if 0
void RdmaQueuePairGroup::AddRxQp(Ptr<RdmaRxQueuePair> rxQp){
	m_rxQps.push_back(rxQp);
}
#endif

void
RdmaQueuePairGroup::RemoveFinishedQps(uint32_t& rr_last)
{
    uint32_t idx = 0;
    uint32_t rm_cnt_before_idx = 0;
    auto new_end = std::remove_if(m_qps.begin(), m_qps.end(), [&](Ptr<RdmaQueuePair> qp) {
        idx++;
        if (qp == nullptr || qp->IsFinished())
        {
            if (idx - 1 < rr_last)
            {
                rm_cnt_before_idx++;
            }
            return true;
        }
        return false;
    });
    rr_last -= rm_cnt_before_idx;
    m_qps.erase(new_end, m_qps.end());
}

void
RdmaQueuePairGroup::Clear(void)
{
    m_qps.clear();
}

} // namespace ns3