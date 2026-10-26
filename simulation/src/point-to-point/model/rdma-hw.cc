#include "rdma-hw.h"

#include "cn-header.h"
#include "ppp-header.h"
#include "qbb-header.h"
#include "rdma-queue-pair.h"

#include "ns3/boolean.h"
#include "ns3/data-rate.h"
#include "ns3/double.h"
#include "ns3/pointer.h"
#include "ns3/ppp-header.h"
#include "ns3/uinteger.h"
#include <ns3/ipv4-header.h>
#include <ns3/simple-seq-ts-header.h>
#include <ns3/simulator.h>
#include <ns3/udp-header.h>
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif
#include <deque>
#include <iostream> // debug

namespace ns3
{
NS_LOG_COMPONENT_DEFINE("RdmaHw");
NS_OBJECT_ENSURE_REGISTERED(RdmaHw);

TypeId
RdmaHw::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::RdmaHw")
            .SetParent<Object>()
            .AddAttribute("MinRate",
                          "Minimum rate of a throttled flow",
                          DataRateValue(DataRate("100Mb/s")),
                          MakeDataRateAccessor(&RdmaHw::m_minRate),
                          MakeDataRateChecker())
            .AddAttribute("Mtu",
                          "Mtu.",
                          UintegerValue(1000),
                          MakeUintegerAccessor(&RdmaHw::m_mtu),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("CcMode",
                          "which mode of DCQCN is running",
                          UintegerValue(0),
                          MakeUintegerAccessor(&RdmaHw::m_cc_mode),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("NACKGenerationInterval",
                          "The NACK Generation interval",
                          DoubleValue(500.0),
                          MakeDoubleAccessor(&RdmaHw::m_nack_interval),
                          MakeDoubleChecker<double>())
            .AddAttribute("L2ChunkSize",
                          "Layer 2 chunk size. Disable chunk mode if equals to 0.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&RdmaHw::m_chunk),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("L2AckInterval",
                          "Layer 2 Ack intervals. Disable ack if equals to 0.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&RdmaHw::m_ack_interval),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("L2BackToZero",
                          "Layer 2 go back to zero transmission.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RdmaHw::m_backto0),
                          MakeBooleanChecker())
            .AddAttribute("RateAI",
                          "Rate increment unit in AI period",
                          DataRateValue(DataRate("5Mb/s")),
                          MakeDataRateAccessor(&RdmaHw::m_rai),
                          MakeDataRateChecker())
            .AddAttribute("RateHAI",
                          "Rate increment unit in hyperactive AI period",
                          DataRateValue(DataRate("50Mb/s")),
                          MakeDataRateAccessor(&RdmaHw::m_rhai),
                          MakeDataRateChecker())
            .AddAttribute("VarWin",
                          "Use variable window size or not",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RdmaHw::m_var_win),
                          MakeBooleanChecker())
            .AddAttribute("RateBound",
                          "Bound packet sending by rate, for test only",
                          BooleanValue(true),
                          MakeBooleanAccessor(&RdmaHw::m_rateBound),
                          MakeBooleanChecker())
            .AddAttribute("GPUsPerServer",
                          "the number of gpus in a server, used for routing",
                          UintegerValue(1),
                          MakeUintegerAccessor(&RdmaHw::m_gpus_per_server),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("TotalPauseTimes",
                          "The number of pause times to simulate PFC pause due to PCIe",
                          UintegerValue(0),
                          MakeUintegerAccessor(&RdmaHw::m_total_pause_times),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("NVLS_enable",
                          "NVLS enable info",
                          UintegerValue(0),
                          MakeUintegerAccessor(&RdmaHw::nvls_enable),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("NicCoalesceMethod",
                          "The Nic's coalesce method",
                          EnumValue(NicCoalesceMethod::PER_QP),
                          MakeEnumAccessor(&RdmaHw::m_nic_coalesce_method),
                          MakeEnumChecker(NicCoalesceMethod::PER_QP,
                                          "PER_QP",
                                          NicCoalesceMethod::PER_IP,
                                          "PER_IP"))
            .AddAttribute("CnpInterval",
                          "The Cnp interval",
                          TimeValue(MicroSeconds(4)),
                          MakeTimeAccessor(&RdmaHw::m_cnp_interval),
                          MakeTimeChecker())
            .AddAttribute("EnableRto",
                          "Enable Rto timer (experimental)",
                          BooleanValue(false),
                          MakeBooleanAccessor(&RdmaHw::m_enable_rto),
                          MakeBooleanChecker());
    return tid;
}

RdmaHw::RdmaHw()
{
}

void
RdmaHw::enable_nvls()
{
    nvls_enable = 1;
}

void
RdmaHw::disable_nvls()
{
    nvls_enable = 0;
}

void
RdmaHw::add_nvswitch(uint32_t nvswitch_id)
{
    nvswitch_set.insert(nvswitch_id);
}

void
RdmaHw::SetNode(Ptr<Node> node)
{
    m_node = node;
}

void
RdmaHw::Setup(QpCompleteCallback cb,
              SendCompleteCallback send_cb,
              RecvCompleteCallback recv_cb,
              MessageCompleteCallback message_cb)
{
    tx_bytes.resize(m_nic.size());
    last_tx_bytes.resize(m_nic.size());
    m_nic_peripTable.resize(m_nic.size());
    for (uint32_t i = 0; i < m_nic.size(); i++)
    {
        tx_bytes[i] = 0;
        last_tx_bytes[i] = 0;
        Ptr<QbbNetDevice> dev = m_nic[i].dev;
        if (dev == NULL)
        {
            continue;
        }
        // share data with NIC
        dev->m_rdmaEQ->m_qpGrp = m_nic[i].qpGrp;
        // setup callback
        dev->m_rdmaReceiveCb = MakeCallback(&RdmaHw::Receive, this);
        dev->m_rdmaSentCb = MakeCallback(&RdmaHw::SendPacketComplete, this);
        dev->m_rdmaLinkDownCb = MakeCallback(&RdmaHw::SetLinkDown, this);
        dev->m_rdmaPktSent = MakeCallback(&RdmaHw::PktSent, this);
        dev->m_rdmaUpdateTxBytes = MakeCallback(&RdmaHw::UpdateTxBytes, this);
        // config NIC
        dev->m_rdmaEQ->m_rdmaGetNxtPkt = MakeCallback(&RdmaHw::GetNxtPacket, this);
    }
    // setup qp complete callback
    m_qpCompleteCallback = cb;
    m_sendCompleteCallback = send_cb;
    m_recvCompleteCallback = recv_cb;
    m_messageCompleteCallback = message_cb;
}

uint32_t
ip_to_node_id(Ipv4Address ip)
{
    return (ip.Get() >> 8) & 0xffff;
}

uint32_t
RdmaHw::GetNicIdxOfQp(Ptr<RdmaQueuePair> qp)
{
    uint32_t src = qp->m_src;
    uint32_t dst = qp->m_dest;
    if (src / m_gpus_per_server == dst / m_gpus_per_server ||
        m_rtTable_nxthop_nvswitch.count(qp->dip.Get()) != 0)
    { // src and dst are in the same server, communicate through nvswitch
        auto& v = m_rtTable_nxthop_nvswitch[qp->dip.Get()];
        if (v.empty())
        {
            throw std::runtime_error("No path found from node " + std::to_string(src) +
                                     " to node " + std::to_string(dst));
        }
        return v[qp->GetHash() % v.size()];
    }
    else
    { // src and dst don't in the same server, communicate through swicth
        auto& v = m_rtTable[qp->dip.Get()];
        if (v.empty())
        {
            throw std::runtime_error("No path found from node " + std::to_string(src) +
                                     " to node " + std::to_string(dst));
        }
        return v[qp->GetHash() % v.size()];
    }
}

uint64_t
RdmaHw::GetQpKey(uint32_t dip, uint16_t sport, uint16_t pg)
{
    return ((uint64_t)dip << 32) | ((uint64_t)sport << 16) | (uint64_t)pg;
}

Ptr<RdmaQueuePair>
RdmaHw::GetQp(uint32_t dip, uint16_t sport, uint16_t pg)
{
    uint64_t key = GetQpKey(dip, sport, pg);
    auto it = m_qpMap.find(key);
    if (it != m_qpMap.end())
    {
        return it->second;
    }
    return NULL;
}

Ptr<RdmaQueuePair>
RdmaHw::AddQueuePair(uint32_t src,
                     uint32_t dest,
                     uint64_t size,
                     uint16_t pg,
                     Ipv4Address sip,
                     Ipv4Address dip,
                     uint16_t sport,
                     uint16_t dport,
                     uint32_t win,
                     uint64_t baseRtt,
                     Callback<void> notifyAppFinish,
                     Callback<void> notifyAppSent)
{
    // create qp
    Ptr<RdmaQueuePair> qp = CreateObject<RdmaQueuePair>(pg, sip, dip, sport, dport);
    qp->SetSrc(src);
    qp->SetDest(dest);
    qp->SetWin(win);
    qp->SetBaseRtt(baseRtt);
    qp->SetVarWin(m_var_win);
    qp->SetAppNotifyCallback(notifyAppFinish);
    qp->SetAppSentCallback(notifyAppSent);

    if (size != 0)
    {
        qp->PushMessage(size, 0, 0, notifyAppFinish, notifyAppSent);
    }

    // add qp
    uint32_t nic_idx = GetNicIdxOfQp(qp);

    // Assign the qp to specific qbbnetdevice
    m_nic[nic_idx].qpGrp->AddQp(qp);
    uint64_t key = GetQpKey(dip.Get(), sport, pg);
    m_qpMap[key] = qp;
    qp_cnp[key] = 0;
    last_qp_cnp[key] = 0;
    last_qp_rate[key] = 0;

    // set init variables
    DataRate m_bps = m_nic[nic_idx].dev->GetDataRate();
    qp->m_rate = m_bps;
    qp->m_max_rate = m_bps;

    Ptr<Packet> mtuPacket = GenDataPacket(qp, m_mtu);
    qp->m_tokenBucket.Init(
        mtuPacket->GetSize(),
        Simulator::Now(),
        std::max(mtuPacket->GetSize(), m_nic[nic_idx].dev->m_rdmaEQ->m_token_per_round) * 2);

    QpStartCC(qp);

    // NVLS settings
    if (nvls_enable == 1)
    {
        qp->nvls_enable = 1;
    }
    else
    {
        qp->nvls_enable = 0;
    }
    // Notify Nic
    m_nic[nic_idx].dev->NewQp(qp);

    return qp;
}

void
RdmaHw::DeleteQueuePair(Ptr<RdmaQueuePair> qp)
{
    // remove qp from the m_qpMap
    uint64_t key = GetQpKey(qp->dip.Get(), qp->sport, qp->m_pg);
    m_qpMap.erase(key);
    qp_cnp.erase(key);
    last_qp_cnp.erase(key);
    last_qp_rate.erase(key);
}

void
RdmaHw::QpStartCC(Ptr<RdmaQueuePair> qp)
{
    TypeId congTypeId;
    switch (m_cc_mode)
    {
    case 1:
        congTypeId = MellanoxDcqcn::GetTypeId();
        break;
    case 3:
        congTypeId = Hpcc::GetTypeId();
        break;
    case 7:
        congTypeId = Timely::GetTypeId();
        break;
    case 8:
        congTypeId = Dctcp::GetTypeId();
        break;
    case 10:
        congTypeId = HpccPint::GetTypeId();
        break;
    case 11:
        congTypeId = Swift::GetTypeId();
        break;
    case 12:
        congTypeId = RealDcqcn::GetTypeId();
        break;
    case 13:
        congTypeId = HpccEcn::GetTypeId();
        break;
    default:
        NS_FATAL_ERROR("cc_mode" << m_cc_mode << " can not match any CongestionTypeId");
        break;
    }

    uint32_t nic_idx = GetNicIdxOfQp(qp);

    bool createAlgo = false;
    if (m_nic_coalesce_method == NicCoalesceMethod::PER_QP)
    {
        ObjectFactory congestionAlgorithmFactory;
        congestionAlgorithmFactory.SetTypeId(congTypeId);
        Ptr<RdmaCongestionOps> algo = congestionAlgorithmFactory.Create<RdmaCongestionOps>();
        qp->m_congestionControl = algo;
        createAlgo = true;
    }
    else if (m_nic_coalesce_method == NicCoalesceMethod::PER_IP)
    {
        auto& mp = m_nic_peripTable[nic_idx];
        if (mp.find(qp->dip.Get()) == mp.end())
        {
            // Create new tableEntry
            mp[qp->dip.Get()] = CreateObject<NicPerIpTableEntry>();

            ObjectFactory congestionAlgorithmFactory;
            congestionAlgorithmFactory.SetTypeId(congTypeId);
            Ptr<RdmaCongestionOps> algo = congestionAlgorithmFactory.Create<RdmaCongestionOps>();
            mp[qp->dip.Get()]->m_congestionControl = algo;
            createAlgo = true;
        }
        qp->m_congestionControl = mp[qp->dip.Get()]->m_congestionControl;
        mp[qp->dip.Get()]->qpNum++;
    }

    if (createAlgo)
    {
        qp->m_congestionControl->SetHw(this, m_nic[nic_idx].dev);
        qp->m_congestionControl->LazyInit(qp, qp->m_rate);
        if (!m_cc_configs.empty())
        {
            for (auto& cc_config : m_cc_configs)
            {
                qp->m_congestionControl->SetAttribute(cc_config.first, *(cc_config.second));
            }
            m_cc_configs.clear();
        }
    }
}

void
RdmaHw::QpStopCC(Ptr<RdmaQueuePair> qp)
{
    uint32_t nic_idx = GetNicIdxOfQp(qp);

    qp->m_congestionControl->QpComplete(qp);
    if (m_nic_coalesce_method == NicCoalesceMethod::PER_QP)
    {
        qp->m_congestionControl->AllComplete();
    }
    else if (m_nic_coalesce_method == NicCoalesceMethod::PER_IP)
    {
        m_nic_peripTable[nic_idx][qp->dip.Get()]->qpNum--;
        if (m_nic_peripTable[nic_idx][qp->dip.Get()]->qpNum == 0)
        {
            // No qp exists
            m_nic_peripTable[nic_idx][qp->dip.Get()]->m_congestionControl->AllComplete();
            m_nic_peripTable[nic_idx].erase(qp->dip.Get());
        }
    }
}

Ptr<RdmaRxQueuePair>
RdmaHw::GetRxQp(uint32_t sip,
                uint32_t dip,
                uint16_t sport,
                uint16_t dport,
                uint16_t pg,
                bool create)
{
    uint64_t key = ((uint64_t)dip << 32) | ((uint64_t)pg << 16) | (uint64_t)dport;
#ifdef NS3_MTP
    MtpInterface::CriticalSection cs;
#endif
    auto it = m_rxQpMap.find(key);
    if (it != m_rxQpMap.end())
    {
        return it->second;
    }
    if (create)
    {
        // create new rx qp
        Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
        // init the qp
        q->sip = sip;
        q->dip = dip;
        q->sport = sport;
        q->dport = dport;
        q->m_ecn_source.qIndex = pg;
        // store in map
        m_rxQpMap[key] = q;
        return q;
    }
    return NULL;
}

uint32_t
RdmaHw::GetNicIdxOfRxQp(Ptr<RdmaRxQueuePair> q)
{
    // Check m_rtTable first, then m_rtTable_nxthop_nvswitch; error if neither has a route.
    if (m_rtTable.count(q->dip) != 0)
    {
        auto& v = m_rtTable[q->dip];
        if (v.size() > 0)
        {
            return v[q->GetHash() % v.size()];
        }
        else
        {
            NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
        }
    }
    else if (m_rtTable_nxthop_nvswitch.count(q->dip) != 0)
    {
        auto& v = m_rtTable_nxthop_nvswitch[q->dip];
        if (v.size() > 0)
        {
            return v[q->GetHash() % v.size()];
        }
        else
        {
            NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
        }
    }
    else
    {
        NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
    }
    NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
    return 0;
}

void
RdmaHw::DeleteRxQp(uint32_t dip, uint16_t pg, uint16_t dport)
{
    uint64_t key = ((uint64_t)dip << 32) | ((uint64_t)pg << 16) | (uint64_t)dport;
    m_rxQpMap.erase(key);
}

/* ---------------------------------------------------------------------------
 * Continuous flow (ported from GitLab)
 * ------------------------------------------------------------------------- */
void
RdmaHw::CheckMessageCompletion(Ptr<RdmaQueuePair> qp)
{
    // STEP 1: Move fully-sent messages from PENDING -> IN-FLIGHT.
    // Fires the send-complete callback (once per message, even across retransmissions).
    while (qp->IsCurMessageFullySent())
    {
        NS_ASSERT(!m_sendCompleteCallback.IsNull());
        if (qp->m_pendingMessages.empty())
        {
            break;
        }

        bool needs_notification = !qp->m_pendingMessages.front().m_appNotified;
        if (needs_notification)
        {
            qp->m_pendingMessages.front().m_appNotified = true;
        }

        RdmaQueuePair::RdmaMessage msg = qp->MoveCurrentToInFlight();

        if (needs_notification)
        {
            m_sendCompleteCallback(qp, msg);
        }
    }

    // STEP 2: Complete ACKed messages from IN-FLIGHT -> DONE.
    // Fires the message-complete callback.
    while (qp->IsOldestInFlightAcked())
    {
        if (qp->m_inFlightMessages.empty())
        {
            NS_LOG_WARN("CheckMessageCompletion: IsOldestInFlightAcked returned true but queue "
                        "is empty");
            break;
        }

        const RdmaQueuePair::RdmaMessage msg = qp->m_inFlightMessages.front();
        qp->FinishMessage(); // removes from in-flight and calls the app callback

        if (!m_messageCompleteCallback.IsNull())
        {
            m_messageCompleteCallback(qp, msg);
        }
    }
}

int
RdmaHw::SendPacketComplete(Ptr<Packet> p, CustomHeader& ch)
{
    uint16_t qIndex = ch.udp.pg;
    uint16_t port = ch.udp.sport;
    Ptr<RdmaQueuePair> qp = GetQp(ch.dip, port, qIndex);
    if (qp == NULL)
    {
        return 0;
    }

    CheckMessageCompletion(qp);

    return 0;
}

void
RdmaHw::SendComplete(Ptr<RdmaQueuePair> qp)
{
    // Only used outside the normal flow; the main logic is in CheckMessageCompletion.
    NS_ASSERT(!m_sendCompleteCallback.IsNull());
    if (qp->m_pendingMessages.empty())
    {
        return;
    }

    // Move message from pending to in-flight
    RdmaQueuePair::RdmaMessage msg = qp->MoveCurrentToInFlight();

    // Trigger the send complete callback
    m_sendCompleteCallback(qp, msg);
}

int
RdmaHw::ReceiveUdp(Ptr<Packet> p, CustomHeader& ch)
{
    uint8_t ecnbits = ch.GetIpv4EcnBits();

    uint32_t payload_size = p->GetSize() - ch.GetSerializedSize();
    Ptr<RdmaRxQueuePair> rxQp =
        GetRxQp(ch.dip, ch.sip, ch.udp.dport, ch.udp.sport, ch.udp.pg, false);
    NS_ASSERT(rxQp);
    if (ecnbits != 0)
    {
        rxQp->m_ecn_source.ecnbits |= ecnbits;
        rxQp->m_ecn_source.qfb++;
    }
    rxQp->m_ecn_source.total++;
    rxQp->m_milestone_rx = m_ack_interval;

    const int x = ReceiverCheckSeq(ch.udp.seq, rxQp, payload_size, false);
    if (x == 1 || x == 2)
    { // generate ACK or NACK
        qbbHeader seqh;
        seqh.SetSeq(rxQp->ReceiverNextExpectedSeq);
        seqh.SetPG(ch.udp.pg);
        seqh.SetSport(ch.udp.dport);
        seqh.SetDport(ch.udp.sport);
        seqh.SetIntHeader(ch.udp.ih);
        if (ecnbits)
        {
            if (m_cc_mode == 1)
            {
                seqh.SetCnp();
            }
            else
            {
                uint64_t key = 0;
                if (m_nic_coalesce_method == NicCoalesceMethod::PER_IP)
                {
                    key = GetNicIdxOfRxQp(rxQp); // each nic has a timer
                }
                else if (m_nic_coalesce_method == NicCoalesceMethod::PER_QP)
                {
                    key = GetQpKey(ch.sip, ch.udp.sport, ch.udp.pg); // each rxqp has a timer
                }
                if (m_cnpTable.count(key) == 0)
                {
                    // Create new tableEntry
                    m_cnpTable[key] = Simulator::Now() - m_cnp_interval;
                }
                if (Simulator::Now() - m_cnpTable[key] >= m_cnp_interval)
                {
                    seqh.SetCnp();
                    m_cnpTable[key] = Simulator::Now();
                }
            }
        }

        Ptr<Packet> newp =
            Create<Packet>(std::max(60 - 14 - 20 - (int)seqh.GetSerializedSize(), 0));
        newp->AddHeader(seqh);

        Ipv4Header head; // Prepare IPv4 header
        head.SetDestination(Ipv4Address(ch.sip));
        head.SetSource(Ipv4Address(ch.dip));
        head.SetProtocol(x == 1 ? 0xFC : 0xFD); // ack=0xFC nack=0xFD
        head.SetTtl(64);
        head.SetPayloadSize(newp->GetSize());
        head.SetIdentification(rxQp->m_ipid++);
        // GPU receives the packet and generate ACK with NVLS tag
        if (ch.m_tos == 4)
        {
            head.SetTos(4);
        }

        newp->AddHeader(head);
        AddHeader(newp, 0x800); // Attach PPP header
        uint32_t dip = ch.dip;
        uint32_t did = (dip >> 8) & 0xffff;
        // send
        uint32_t nic_idx = GetNicIdxOfRxQp(rxQp);
        m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(newp);
        // packets sent to the destination NVSwitch
        if (did == m_node->GetId() && m_node->GetNodeType() == 2 && ch.m_tos == 4)
        {
            m_nic[nic_idx].dev->SwitchAsHostSend();
        }
        else
        {
            m_nic[nic_idx].dev->TriggerTransmit();
        }
    }

    // Continuous flow: one in-order packet (or a drained out-of-order run) can
    // complete several back-to-back messages, and completion must also be
    // detected on in-sequence packets that don't trigger an ACK (x == 5).
    if (x == 1 || x == 5)
    {
        while (!rxQp->m_messages.empty())
        {
            const RdmaRxQueuePair::RdmaMessage& msg = rxQp->m_messages.front();
            // >= for robustness: if we've received beyond the message end, it's complete
            if (rxQp->ReceiverNextExpectedSeq >= msg.m_startSeq + msg.m_size)
            {
                RecvComplete(rxQp);
            }
            else
            {
                break;
            }
        }
    }
    return 0;
}

int
RdmaHw::ReceiveCnp(Ptr<Packet> p, CustomHeader& ch)
{
    // QCN on NIC
    // This is a Congestion signal
    // Then, extract data from the congestion packet.
    // We assume, without verify, the packet is destinated to me
    uint32_t qIndex = ch.cnp.qIndex;
    if (qIndex == 1)
    { // DCTCP
        return 0;
    }
    uint16_t udpport = ch.cnp.fid; // corresponds to the sport

    // get qp
    Ptr<RdmaQueuePair> qp = GetQp(ch.sip, udpport, qIndex);
    if (qp == NULL)
    {
        std::cout << "ERROR: QCN NIC cannot find the flow\n";
        return 0;
    }
    // get nic
    uint32_t nic_idx = GetNicIdxOfQp(qp);
    Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;

    if (qp->m_rate == 0) // lazy initialization
    {
        qp->m_congestionControl->LazyInit(qp, dev->GetDataRate());
    }
    return 0;
}

int
RdmaHw::ReceiveAck(Ptr<Packet> p, CustomHeader& ch)
{
    uint16_t qIndex = ch.ack.pg;
    uint16_t port = ch.ack.dport;
    uint64_t seq = ch.ack.seq;
    uint8_t cnp = (ch.ack.flags >> qbbHeader::FLAG_CNP) & 1;

    Ptr<RdmaQueuePair> qp = GetQp(ch.sip, port, qIndex);
    if (qp == NULL)
    {
        return 0;
    }

    uint32_t nic_idx = GetNicIdxOfQp(qp);
    if (m_ack_interval == 0)
    {
        std::cout << "ERROR: shouldn't receive ack\n";
    }
    else
    {
        const bool isNack = (ch.l3Prot == 0xFD);

        // A NACK is obsolete if ACKs have already moved snd_una past its seq.
        // NOTE: strict '<' on purpose. A NACK with seq == snd_una is the normal
        // "packet at snd_una was lost" case and must trigger recovery.
        const bool obsoleteNack = isNack && !m_backto0 && seq < qp->snd_una;

        if (!obsoleteNack)
        {
            const uint64_t ackSeq = m_backto0 ? seq / m_chunk * m_chunk : seq;
            if (qp->Acknowledge(ackSeq))
            {
                InitRto(qp);
            }

            // Unified completion check, before recovery:
            //  - moves any fully-sent messages to in-flight (race with SendPacketComplete)
            //  - completes any fully-ACKed messages
            CheckMessageCompletion(qp);

            if (isNack)
            {
                // move unacked in-flight messages back to pending, rewind snd_nxt
                RecoverQueue(qp);
            }
        }
    }

    // The message-complete callback may have torn down the QP (QpComplete nulls
    // the CC object). Unlike the old code we no longer return early on message
    // completion, so guard against that here.
    if (!qp->m_congestionControl)
    {
        return 0;
    }

    // handle cnp
    if (cnp)
    {
        uint64_t key = GetQpKey(qp->dip.Get(), qp->sport, qp->m_pg);
        qp_cnp[key]++; // update for the number of cnp this qp has received
    }

    qp->m_congestionControl->HandleAck(qp, p, ch);

    uint32_t dip = ch.dip;
    uint32_t did = (dip >> 8) & 0xffff;
    // ACK may advance the on-the-fly window, allowing more packets to send
    if (did == m_node->GetId() && m_node->GetNodeType() == 2)
    {
        m_nic[nic_idx].dev->SwitchAsHostSend();
    }
    else
    {
        m_nic[nic_idx].dev->TriggerTransmit();
    }
    return 0;
}

int
RdmaHw::Receive(Ptr<Packet> p, CustomHeader& ch)
{
    if (ch.l3Prot == 0x11)
    { // UDP
        ReceiveUdp(p, ch);
    }
    else if (ch.l3Prot == 0xFF)
    { // CNP
        ReceiveCnp(p, ch);
    }
    else if (ch.l3Prot == 0xFD)
    { // NACK
        ReceiveAck(p, ch);
    }
    else if (ch.l3Prot == 0xFC)
    { // ACK
        ReceiveAck(p, ch);
    }
    return 0;
}

int
RdmaHw::ReceiverCheckSeq(uint64_t seq, Ptr<RdmaRxQueuePair> q, uint32_t size, bool ooo_recursion)
{
    uint64_t expected = q->ReceiverNextExpectedSeq;
    if (seq == expected)
    {
        q->ReceiverNextExpectedSeq = expected + size;

        // handle any previously received out of sequence packets
        bool ooo_ack = false;
        while (!q->m_outOfSeqPackets.empty())
        {
            const auto nxt = q->m_outOfSeqPackets.begin();
            uint64_t ooo_seq = nxt->first;
            uint32_t ooo_size = nxt->second;
            q->m_outOfSeqPackets.erase(nxt);
            if (const int ret = ReceiverCheckSeq(ooo_seq, q, ooo_size, true); ret != 1 && ret != 5)
            {
                q->m_outOfSeqPackets[ooo_seq] = ooo_size;
                break;
            }
            else if (ret == 1)
            {
                ooo_ack = true;
            }
        }

        if (q->ReceiverNextExpectedSeq >= q->m_milestone_rx)
        {
            q->m_milestone_rx += m_ack_interval;
            return 1; // Generate ACK
        }
        else if (ooo_ack || q->ReceiverNextExpectedSeq % m_chunk == 0)
        {
            return 1;
        }
        else
        {
            return 5;
        }
    }
    else if (ooo_recursion)
    {
        return 0;
    }
    else if (seq > expected)
    {
        // Store out-of-sequence packets up to a certain bound
        // FIXME m_win might be variable, find a better way to cap m_outOfSeqPackets
        if (q->m_ooo && q->m_outOfSeqPackets.size() < q->m_win / m_mtu + 2)
        {
            if (q->m_outOfSeqPackets.find(seq) == q->m_outOfSeqPackets.end())
            {
                q->m_outOfSeqPackets[seq] = size;
                return 0;
            }
            else
            {
                // duplicate
                return 3;
            }
        }
        // Generate NACK
        if (Simulator::Now() >= q->m_nackTimer || q->m_lastNACK != expected)
        {
            q->m_nackTimer = Simulator::Now() + NanoSeconds(m_nack_interval * 1000);
            q->m_lastNACK = expected;
            if (m_backto0)
            {
                q->ReceiverNextExpectedSeq = q->ReceiverNextExpectedSeq / m_chunk * m_chunk;
            }
            return 2;
        }
        else
        {
            return 4;
        }
    }
    else
    {
        // Duplicate.
        return 3;
    }
}

void
RdmaHw::AddHeader(Ptr<Packet> p, uint16_t protocolNumber)
{
    PppHeader ppp;
    ppp.SetProtocol(EtherToPpp(protocolNumber));
    p->AddHeader(ppp);
}

uint16_t
RdmaHw::EtherToPpp(uint16_t proto)
{
    switch (proto)
    {
    case 0x0800:
        return 0x0021; // IPv4
    case 0x86DD:
        return 0x0057; // IPv6
    default:
        NS_ASSERT_MSG(false, "PPP Protocol number not defined!");
    }
    return 0;
}

void
RdmaHw::RecoverQueue(Ptr<RdmaQueuePair> qp)
{
    // Go-back-N from snd_una across the pending and in-flight message queues.
    // Called on NACK and on RTO.
    uint64_t recover_point = qp->snd_una;

    NS_LOG_DEBUG("RecoverQueue: recover_point=" << recover_point << " snd_nxt=" << qp->snd_nxt
                                                << " pending=" << qp->m_pendingMessages.size()
                                                << " inflight="
                                                << qp->m_inFlightMessages.size());

    // Step 1: Save existing pending messages (possibly partially sent).
    // They are re-appended AFTER the in-flight messages being recovered.
    std::deque<RdmaQueuePair::RdmaMessage> saved_pending;
    while (!qp->m_pendingMessages.empty())
    {
        RdmaQueuePair::RdmaMessage msg = qp->m_pendingMessages.front();
        qp->m_pendingMessages.pop_front();
        msg.m_sent = false;
        saved_pending.push_back(msg);
    }

    // Step 2: Move in-flight messages that need retransmission back to pending.
    // Walk from newest to oldest so push_front keeps the original order.
    while (!qp->m_inFlightMessages.empty())
    {
        RdmaQueuePair::RdmaMessage& msg = qp->m_inFlightMessages.back();
        uint64_t msgEnd = msg.m_startSeq + msg.m_size;

        if (recover_point < msgEnd)
        {
            // has unacked data -> retransmit
            msg.m_sent = false;
            qp->m_pendingMessages.push_front(msg);
            qp->m_inFlightMessages.pop_back();
        }
        else
        {
            // fully ACKed: keep in in-flight and stop
            break;
        }
    }

    // Step 3: Re-append saved pending messages (original order)
    for (auto& msg : saved_pending)
    {
        qp->m_pendingMessages.push_back(msg);
    }

    // Reset snd_nxt to snd_una for retransmission
    qp->snd_nxt = qp->snd_una;

    NS_LOG_DEBUG("RecoverQueue done: snd_nxt=" << qp->snd_nxt
                                               << " pending=" << qp->m_pendingMessages.size()
                                               << " inflight="
                                               << qp->m_inFlightMessages.size());

    InitRto(qp);
}

void
RdmaHw::QpComplete(Ptr<RdmaQueuePair> qp)
{
    if (m_enable_rto)
    {
        qp->m_rtoEvent.Cancel();
    }
    NS_ASSERT(!m_qpCompleteCallback.IsNull());

    uint32_t nic_idx = GetNicIdxOfQp(qp);
    {
#ifdef NS3_MTP
        MtpInterface::CriticalSection cs;
#endif
        m_nic[nic_idx].dev->m_rdmaEQ->RemoveFinishedQps();
    }

    QpStopCC(qp);

    // This callback will log info
    // It may also delete the rxQp on the receiver
    m_qpCompleteCallback(qp);

    qp->m_congestionControl = nullptr;

    // delete the qp
    DeleteQueuePair(qp);
}

void
RdmaHw::RecvComplete(Ptr<RdmaRxQueuePair> rxQp)
{
    if (!m_recvCompleteCallback.IsNull())
    {
        const RdmaRxQueuePair::RdmaMessage msg = rxQp->m_messages.front();
        rxQp->m_messages.pop();
        m_recvCompleteCallback(rxQp, msg);
    }
}

void
RdmaHw::QpCompleteMessage(Ptr<RdmaQueuePair> qp)
{
    // Messages complete from the in-flight queue, not pending.
    // m_startSeq is fixed when the message is pushed (continuous flow), so there is
    // no longer any need to rebase the next message's start seq here.
    if (qp->m_inFlightMessages.empty())
    {
        NS_LOG_WARN("QpCompleteMessage called but no in-flight messages");
        return;
    }

    const RdmaQueuePair::RdmaMessage msg = qp->m_inFlightMessages.front();
    qp->FinishMessage(); // removes from in-flight and calls the callback

    if (!m_messageCompleteCallback.IsNull())
    {
        m_messageCompleteCallback(qp, msg);
    }
}

void
RdmaHw::SetLinkDown(Ptr<QbbNetDevice> dev)
{
    printf("RdmaHw: node:%u a link down\n", m_node->GetId());
}

void
RdmaHw::AddTableEntry(Ipv4Address& dstAddr, uint32_t intf_idx, bool is_nvswitch)
{
    uint32_t dip = dstAddr.Get();
    if (is_nvswitch == false)
    {
        m_rtTable[dip].push_back(intf_idx);
    }
    else
    {
        m_rtTable_nxthop_nvswitch[dip].push_back(intf_idx);
    }
}

void
RdmaHw::ClearTable()
{
    m_rtTable.clear();
    m_rtTable_nxthop_nvswitch.clear();
}

void
RdmaHw::RedistributeQp()
{
    // clear old qpGrp
    for (uint32_t i = 0; i < m_nic.size(); i++)
    {
        if (m_nic[i].dev == NULL)
        {
            continue;
        }
        m_nic[i].qpGrp->Clear();
    }

    // redistribute qp
    for (auto& it : m_qpMap)
    {
        Ptr<RdmaQueuePair> qp = it.second;
        uint32_t nic_idx = GetNicIdxOfQp(qp);
        m_nic[nic_idx].qpGrp->AddQp(qp);
        // Notify Nic
        m_nic[nic_idx].dev->ReassignedQp(qp);
    }
}

Ptr<Packet>
RdmaHw::GetNxtPacket(Ptr<RdmaQueuePair> qp)
{
    uint64_t payload_size = qp->GetBytesLeft();

    // Never create 0-byte packets
    if (payload_size == 0)
    {
        NS_LOG_DEBUG("GetNxtPacket: no bytes left to send (pending="
                     << qp->m_pendingMessages.size()
                     << " inflight=" << qp->m_inFlightMessages.size() << ")");
        return nullptr;
    }

    // A single packet must never span two messages. GetBytesLeft() spans all
    // queued messages, so cap the payload at the end of the message containing
    // snd_nxt. Back-to-back sending is preserved: the next call immediately
    // starts the following message without waiting for ACKs.
    //
    // snd_nxt is always at or past the end of every in-flight message (messages
    // only move to in-flight once fully sent, and RecoverQueue resets snd_nxt to
    // snd_una with only fully-acked messages left in-flight), so the message
    // containing snd_nxt is always in the pending queue.
    uint64_t bytesLeftInCurMsg = 0;
    for (const auto& msg : qp->m_pendingMessages)
    {
        const uint64_t msgEnd = msg.m_startSeq + msg.m_size;
        if (msgEnd > qp->snd_nxt)
        {
            bytesLeftInCurMsg = msgEnd - qp->snd_nxt;
            break;
        }
    }

    if (bytesLeftInCurMsg != 0 && bytesLeftInCurMsg < payload_size)
    {
        payload_size = bytesLeftInCurMsg;
    }

    if ((uint64_t)m_mtu < payload_size)
    {
        payload_size = m_mtu;
    }
    Ptr<Packet> p = GenDataPacket(qp, payload_size);

    // update state
    qp->snd_nxt += payload_size;
    qp->m_ipid++;

    // This will trigger retransmission in case the packet is not acknowledged in time
    if (qp->m_rtoEvent.IsExpired())
    {
        InitRto(qp);
    }

    return p;
}

void
RdmaHw::InitRto(Ptr<RdmaQueuePair> qp)
{
    // NOTE: condition kept exactly as in the GitHub version. It looks inverted
    // (the timer is armed when EnableRto is *false*), see porting notes.
    if (!m_enable_rto)
    {
        qp->m_rtoEvent.Cancel();
        if (qp->m_baseRtt > 0 && qp->snd_nxt > qp->snd_una)
        {
            // FIXME determine proper timeout value
            qp->m_rtoEvent =
                Simulator::Schedule(NanoSeconds(30 * qp->m_baseRtt), &RdmaHw::RtoHandler, this, qp);
        }
    }
}

void
RdmaHw::RtoHandler(Ptr<RdmaQueuePair> qp)
{
    if (qp->snd_nxt > qp->snd_una)
    { // still have unacked data
        RecoverQueue(qp); // go-back-N across message queues, snd_nxt = snd_una
        uint32_t nic_idx = GetNicIdxOfQp(qp);
        m_nic[nic_idx].dev->TriggerTransmit();
    }
}

Ptr<Packet>
RdmaHw::GenDataPacket(Ptr<RdmaQueuePair> qp, uint32_t pkt_size)
{
    Ptr<Packet> p = Create<Packet>(pkt_size);
    // add SimpleSeqTsHeader
    SimpleSeqTsHeader seqTs;
    seqTs.SetSeq(qp->snd_nxt);
    seqTs.SetPG(qp->m_pg);
    p->AddHeader(seqTs);
    // add udp header
    UdpHeader udpHeader;
    udpHeader.SetDestinationPort(qp->dport);
    udpHeader.SetSourcePort(qp->sport);
    p->AddHeader(udpHeader);
    // add ipv4 header
    Ipv4Header ipHeader;
    ipHeader.SetSource(qp->sip);
    ipHeader.SetDestination(qp->dip);
    ipHeader.SetProtocol(0x11);
    ipHeader.SetPayloadSize(p->GetSize());
    ipHeader.SetTtl(64);
    // nvls <-> ToS, ToS = 1 -> NVLS enable
    if (qp->nvls_enable == 1)
    {
        ipHeader.SetTos(4);
    }
    else
    {
        ipHeader.SetTos(0);
    }
    ipHeader.SetIdentification(qp->m_ipid);
    p->AddHeader(ipHeader);
    // add ppp header
    PppHeader ppp;
    ppp.SetProtocol(0x0021); // EtherToPpp(0x800), see point-to-point-net-device.cc
    p->AddHeader(ppp);

    return p;
}

void
RdmaHw::PktSent(Ptr<RdmaQueuePair> qp, Ptr<Packet> pkt, Time interframeGap)
{
    qp->lastPktSize = pkt->GetSize();
    qp->m_congestionControl->PktSent(qp, pkt);
    qp->m_rate = qp->m_congestionControl->m_ccRate;
    UpdateNextAvail(qp, interframeGap, pkt->GetSize());
}

void
RdmaHw::UpdateNextAvail(Ptr<RdmaQueuePair> qp, Time interframeGap, uint32_t pkt_size)
{
    Time sendingTime;
    if (m_rateBound)
    {
        sendingTime = interframeGap + qp->m_rate.CalculateBytesTxTime(pkt_size);
    }
    else
    {
        sendingTime = interframeGap + qp->m_max_rate.CalculateBytesTxTime(pkt_size);
    }
    qp->m_nextAvail = Simulator::Now() + sendingTime;
}

void
RdmaHw::ChangeRate(Ptr<RdmaQueuePair> qp, DataRate new_rate)
{
#if 1
    Time sendingTime = qp->m_rate.CalculateBytesTxTime(qp->lastPktSize);
    Time new_sendintTime = new_rate.CalculateBytesTxTime(qp->lastPktSize);
    qp->m_nextAvail = qp->m_nextAvail + new_sendintTime - sendingTime;
    // update nic's next avail event
    uint32_t nic_idx = GetNicIdxOfQp(qp);
    m_nic[nic_idx].dev->UpdateNextAvail(qp->m_nextAvail);
#endif

    // change to new rate
    qp->m_rate = new_rate;
}

/**
 * when nic send a packet, update the bytes it has sent
 */
void
RdmaHw::UpdateTxBytes(uint32_t port_id, uint64_t bytes)
{
    tx_bytes[port_id] += bytes;
}

/**
 * output format:
 * time, host_id, port_id, bandwidth
 */
void
RdmaHw::PrintHostBW(FILE* bw_output, uint32_t bw_mon_interval)
{
    for (uint32_t i = 0; i < m_nic.size(); ++i)
    {
        if (tx_bytes[i] == last_tx_bytes[i])
        {
            continue;
        }
        double bw = (tx_bytes[i] - last_tx_bytes[i]) * 8 * 1e6 / (bw_mon_interval); // bit/s
        bw = bw * 1.0 / 1e9;                                                        // Gbps
        fprintf(bw_output,
                "%lu, %u, %u, %f\n",
                Simulator::Now().GetTimeStep(),
                m_node->GetId(),
                i,
                bw);
        fflush(bw_output);
        last_tx_bytes[i] = tx_bytes[i];
    }
}

/**
 * output format:
 * time, src, dst, sport, dport, size, rate
 */
void
RdmaHw::PrintQPRate(FILE* rate_output)
{
    for (auto it = m_qpMap.begin(); it != m_qpMap.end(); it++)
    {
        Ptr<RdmaQueuePair> qp = it->second;
        uint64_t key = it->first;

        // message info from either pending or in-flight queue
        uint64_t startSeq = 0;
        if (!qp->m_pendingMessages.empty())
        {
            startSeq = qp->m_pendingMessages.front().m_startSeq;
        }
        else if (!qp->m_inFlightMessages.empty())
        {
            startSeq = qp->m_inFlightMessages.front().m_startSeq;
        }

        fprintf(rate_output,
                "%lu, %u, %u, %u, %u, %lu, %lu\n",
                Simulator::Now().GetTimeStep(),
                qp->m_src,
                qp->m_dest,
                qp->sport,
                qp->dport,
                qp->snd_nxt - startSeq,
                qp->m_rate.GetBitRate());
        fflush(rate_output);
        last_qp_rate[key] = qp->m_rate.GetBitRate();
    }
}

/**
 * output format:
 * time, src, dst, sport, dport, size, cnp_number
 */
void
RdmaHw::PrintQPCnpNumber(FILE* cnp_output)
{
    for (auto it = m_qpMap.begin(); it != m_qpMap.end(); it++)
    {
        Ptr<RdmaQueuePair> qp = it->second;
        uint64_t key = it->first;
        if (qp_cnp[key] != last_qp_cnp[key])
        {
            // message size from either pending or in-flight queue
            uint64_t msgSize = 0;
            if (!qp->m_pendingMessages.empty())
            {
                msgSize = qp->m_pendingMessages.front().m_size;
            }
            else if (!qp->m_inFlightMessages.empty())
            {
                msgSize = qp->m_inFlightMessages.front().m_size;
            }

            fprintf(cnp_output,
                    "%lu, %u, %u, %u, %u, %lu, %u\n",
                    Simulator::Now().GetTimeStep(),
                    qp->m_src,
                    qp->m_dest,
                    qp->sport,
                    qp->dport,
                    msgSize,
                    qp_cnp[key]);
            fflush(cnp_output);
            last_qp_cnp[key] = qp_cnp[key];
        }
    }
}
} // namespace ns3