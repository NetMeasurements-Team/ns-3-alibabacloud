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
    m_tag = -1;
    snd_nxt = snd_una = 0;
    m_pg = pg;
    m_ipid = 0;
    m_win = 0;
    m_baseRtt = 0;
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
RdmaQueuePair::SetTag(uint64_t tag)
{
    m_tag = tag;
}

uint64_t
RdmaQueuePair::GetTag()
{
    return m_tag;
}
void
RdmaQueuePair::SetWin(uint32_t win)
{
    m_win = win;
    // std::cout << "set win: " << m_win << std::endl;
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
RdmaQueuePair::GetBytesLeft()
{
    if (m_messages.empty())
    {
        return 0;
    }
    uint64_t size = m_messages.front().m_size + m_messages.front().m_startSeq;
    uint64_t actual_size = 0;
    if (size >= snd_nxt)
    {
        actual_size = size - snd_nxt;
    }
    return actual_size;
}

void
RdmaRxQueuePair::PushMessage(
    uint64_t size,
    uint64_t curr_flow_num)
{
    // TODO does this need to be guarded?
    RdmaMessage msg;
    msg.m_size = size;
    msg.m_cur_id = curr_flow_num;
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

void
RdmaQueuePair::Acknowledge(uint64_t ack)
{
    if (ack > snd_una)
    {
        snd_una = ack;
    }
}

uint64_t
RdmaQueuePair::GetOnTheFly()
{
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
    return m_messages.empty();
}

void
RdmaQueuePair::UpdateRate()
{
    m_rate = m_congestionControl->m_ccRate;
}

void
RdmaQueuePair::PushMessage(uint64_t size,
                           uint64_t curr_flow_num,
                           Callback<void> notifyAppFinish,
                           Callback<void> notifyAppSent)
{
    RdmaMessage msg;
    msg.m_size = size;
    msg.m_cur_id = curr_flow_num;
    if (m_messages.empty())
    {
        // there are no old messages in the queue, so set start_seq to snd_nxt
        msg.m_startSeq = snd_nxt;
        m_lastMessageStartTime = Simulator::Now();
    }
    else
    {
        // there are old messages in the queue, so modify start_seq when finish an old message
        msg.m_startSeq = 0;
    }

    msg.m_notifyAppFinish = notifyAppFinish;
    msg.m_notifyAppSent = notifyAppSent;
    m_messages.push(msg);
}

void
RdmaQueuePair::FinishMessage()
{
    if (!m_messages.empty())
    {
        RdmaMessage msg = m_messages.front();
        m_messages.pop();
        msg.m_notifyAppFinish();

        // snd_nxt = snd_una = 0; //TODO is it needed?
    }
    else
    {
        NS_LOG_ERROR("RdmaQueuePair::FinishMessage(): message is empty but try to finish");
    }
}

bool
RdmaQueuePair::IsCurMessageFinished()
{
    if (m_messages.empty())
    {
        return false;
    }
    return snd_una >= m_messages.front().m_size + m_messages.front().m_startSeq;
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
RdmaRxQueuePair::SetTag(uint64_t tag)
{
    m_tag = tag;
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

Ptr<RdmaQueuePair> RdmaQueuePairGroup::Get(uint32_t idx)
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
