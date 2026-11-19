// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-dcqcn.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/random-variable-stream.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/data-rate.h"
#include "ns3/node-list.h"
#include "ns3/ub-port.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-utils.h"

namespace ns3 {

namespace {
constexpr uint8_t DCQCN_FECN_MODE = 0b100;
constexpr uint8_t DCQCN_FECN_LEVEL_MARKED = 0x1;

DataRate
AverageRate(const DataRate& lhs, const DataRate& rhs)
{
    return DataRate((lhs.GetBitRate() + rhs.GetBitRate()) / 2);
}

} // namespace

NS_OBJECT_ENSURE_REGISTERED(UbDcqcn);
NS_OBJECT_ENSURE_REGISTERED(UbHostDcqcn);
NS_OBJECT_ENSURE_REGISTERED(UbSwitchDcqcn);

NS_LOG_COMPONENT_DEFINE("UbDcqcn");

TypeId
UbDcqcn::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbDcqcn")
                            .SetParent<UbCongestionControl>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbDcqcn>();
    return tid;
}

UbDcqcn::UbDcqcn()
{
    m_algoType = DCQCN;
}

TypeId
UbHostDcqcn::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbHostDcqcn")
                            .SetParent<UbDcqcn>()
                            .SetGroupName("UnifiedBus")
                            .AddAttribute("CnpInterval",
                                          "Minimum interval between DCQCN CNP packets for one TP flow.",
                                          TimeValue(MicroSeconds(50)),
                                          MakeTimeAccessor(&UbHostDcqcn::m_cnpInterval),
                                          MakeTimeChecker())
                            .AddAttribute("LineRate",
                                          "Sender line rate used as the initial DCQCN pacing rate.",
                                          DataRateValue(DataRate("100Gbps")),
                                          MakeDataRateAccessor(&UbHostDcqcn::m_lineRate),
                                          MakeDataRateChecker())
                            .AddAttribute("InitialRate",
                                          "Sender initial rate when local host policy denies line-rate start.",
                                          DataRateValue(DataRate("50Gbps")),
                                          MakeDataRateAccessor(&UbHostDcqcn::m_initialRate),
                                          MakeDataRateChecker())
                            .AddAttribute("RateIncreaseTimer",
                                          "DCQCN rate increase timer.",
                                          TimeValue(MicroSeconds(55)),
                                          MakeTimeAccessor(&UbHostDcqcn::m_rateIncreaseTimer),
                                          MakeTimeChecker())
                            .AddAttribute("AlphaUpdateInterval",
                                          "DCQCN alpha decay timer interval.",
                                          TimeValue(MicroSeconds(55)),
                                          MakeTimeAccessor(&UbHostDcqcn::m_alphaUpdateInterval),
                                          MakeTimeChecker())
                            .AddAttribute("ByteCounterThreshold",
                                          "Byte threshold before additive target-rate increase.",
                                          UintegerValue(10 * 1024 * 1024),
                                          MakeUintegerAccessor(&UbHostDcqcn::m_byteCounterThreshold),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("RateAi",
                                          "Additive target-rate increase applied after fast recovery.",
                                          DataRateValue(DataRate("40Mbps")),
                                          MakeDataRateAccessor(&UbHostDcqcn::m_rateAi),
                                          MakeDataRateChecker())
                            .AddAttribute("HyperAiRate",
                                          "Hyper increase target-rate increase applied after both timer and byte counters exceed F.",
                                          DataRateValue(DataRate("100Mbps")),
                                          MakeDataRateAccessor(&UbHostDcqcn::m_rateHai),
                                          MakeDataRateChecker())
                            .AddAttribute("FastRecoveryLimit",
                                          "Recovery threshold F used by the timer/byte-counter state machine.",
                                          UintegerValue(5),
                                          MakeUintegerAccessor(&UbHostDcqcn::m_fastRecoveryLimit),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("AlphaGain",
                                          "DCQCN alpha EWMA gain g.",
                                          DoubleValue(1.0 / 256.0),
                                          MakeDoubleAccessor(&UbHostDcqcn::m_g),
                                          MakeDoubleChecker<double>(0.0, 1.0))
                            .AddConstructor<UbHostDcqcn>();
    return tid;
}

UbHostDcqcn::UbHostDcqcn()
    : m_cnpInterval(MicroSeconds(50)),
      m_lastCnpSent(Time(0)),
      m_lineRate(DataRate("100Gbps")),
      m_initialRate(DataRate("50Gbps")),
      m_currentRate(DataRate("100Gbps")),
      m_targetRate(DataRate("100Gbps")),
      m_nextAvailableSendTime(Time(0)),
      m_rateIncreaseTimer(MicroSeconds(55)),
      m_alphaUpdateInterval(MicroSeconds(55)),
      m_rateAi(DataRate("40Mbps")),
      m_rateHai(DataRate("100Mbps"))
{
}

void
UbHostDcqcn::OnTpAttached(Ptr<UbTransportChannel> tp)
{
    m_tp = tp;
    m_hostNodeId = tp != nullptr ? tp->GetSrc() : UINT32_MAX;
    m_port = (tp != nullptr && m_hostNodeId != UINT32_MAX)
                 ? DynamicCast<UbPort>(NodeList::GetNode(m_hostNodeId)->GetDevice(tp->GetSport()))
                 : nullptr;
    m_currentRate = m_initialRate;
    m_targetRate = m_initialRate;
    m_nextAvailableSendTime = Simulator::Now();
    InitTrackedEvents();
}

void
UbHostDcqcn::InitTrackedEvents()
{
    if (m_trackedEventsInitialized || m_tp == nullptr)
    {
        return;
    }
    Ptr<UbLifecycleCoordinator> coordinator =
        UbLifecycleCoordinator::GetOrCreate(NodeList::GetNode(m_hostNodeId));
    m_pendingCnpTrackedEvents.Init(coordinator);
    m_rateIncreaseTrackedEvents.Init(coordinator);
    m_alphaTrackedEvents.Init(coordinator);
    m_pacingTrackedEvents.Init(coordinator);
    m_trackedEventsInitialized = true;
}

EventId
UbHostDcqcn::SchedulePendingCnp(Time delay)
{
    const auto& owner = m_tp->GetLifecycleOwner();
    const uint64_t token = m_pendingCnpTrackedEvents.Track(owner);
    EventId event = Simulator::Schedule(delay, &UbHostDcqcn::HandleTrackedPendingCnp, this, token);
    m_pendingCnpTrackedEvents.Attach(owner, token, event, {});
    return event;
}

EventId
UbHostDcqcn::ScheduleRateIncrease()
{
    const auto& owner = m_tp->GetLifecycleOwner();
    const uint64_t token = m_rateIncreaseTrackedEvents.Track(owner);
    EventId event = Simulator::Schedule(m_rateIncreaseTimer,
                                        &UbHostDcqcn::HandleTrackedRateIncrease,
                                        this,
                                        token);
    m_rateIncreaseTrackedEvents.Attach(owner, token, event, {});
    return event;
}

EventId
UbHostDcqcn::ScheduleAlphaUpdate()
{
    const auto& owner = m_tp->GetLifecycleOwner();
    const uint64_t token = m_alphaTrackedEvents.Track(owner);
    EventId event = Simulator::Schedule(m_alphaUpdateInterval,
                                        &UbHostDcqcn::HandleTrackedAlphaUpdate,
                                        this,
                                        token);
    m_alphaTrackedEvents.Attach(owner, token, event, {});
    return event;
}

EventId
UbHostDcqcn::SchedulePacing(Time delay)
{
    const auto& owner = m_tp->GetLifecycleOwner();
    const uint64_t token = m_pacingTrackedEvents.Track(owner);
    EventId event =
        Simulator::Schedule(delay, &UbHostDcqcn::HandleTrackedPacingWakeup, this, token);
    m_pacingTrackedEvents.Attach(owner, token, event, {});
    return event;
}

void
UbHostDcqcn::CancelTrackedEvent(EventId& event, UbTrackedEventSet& trackedEvents)
{
    event.Cancel();
    if (m_trackedEventsInitialized && m_tp != nullptr)
    {
        trackedEvents.CancelPending(m_tp->GetLifecycleOwner());
    }
}

void
UbHostDcqcn::HandleTrackedPendingCnp(uint64_t token)
{
    const auto& owner = m_tp->GetLifecycleOwner();
    if (!m_pendingCnpTrackedEvents.Begin(owner, token))
    {
        return;
    }
    EmitPendingCnp();
    m_pendingCnpTrackedEvents.Resolve(owner, token);
}

void
UbHostDcqcn::HandleTrackedRateIncrease(uint64_t token)
{
    const auto& owner = m_tp->GetLifecycleOwner();
    if (!m_rateIncreaseTrackedEvents.Begin(owner, token))
    {
        return;
    }
    HandleRateIncreaseTimer();
    m_rateIncreaseTrackedEvents.Resolve(owner, token);
}

void
UbHostDcqcn::HandleTrackedAlphaUpdate(uint64_t token)
{
    const auto& owner = m_tp->GetLifecycleOwner();
    if (!m_alphaTrackedEvents.Begin(owner, token))
    {
        return;
    }
    HandleAlphaTimer();
    m_alphaTrackedEvents.Resolve(owner, token);
}

void
UbHostDcqcn::HandleTrackedPacingWakeup(uint64_t token)
{
    const auto& owner = m_tp->GetLifecycleOwner();
    if (!m_pacingTrackedEvents.Begin(owner, token))
    {
        return;
    }
    HandlePacingWakeup();
    m_pacingTrackedEvents.Resolve(owner, token);
}

bool
UbHostDcqcn::IsCcLimited(uint32_t bytes)
{
    (void)bytes;

    EnsureSenderStartState();

    if (!m_congestionCtrlEnabled)
    {
        return false;
    }

    const bool limited = Simulator::Now() < m_nextAvailableSendTime;
    if (limited)
    {
        SchedulePacingWakeup();
    }

    return limited;
}

void
UbHostDcqcn::DoDispose()
{
    CancelTrackedEvent(m_pendingCnpEvent, m_pendingCnpTrackedEvents);
    CancelTrackedEvent(m_rateIncreaseEvent, m_rateIncreaseTrackedEvents);
    CancelTrackedEvent(m_alphaEvent, m_alphaTrackedEvents);
    CancelTrackedEvent(m_pacingWakeupEvent, m_pacingTrackedEvents);
    ReleaseHostActiveFlow();

    m_tp = nullptr;
    m_port = nullptr;
    Object::DoDispose();
}

void
UbHostDcqcn::EnsureSenderStartState()
{
    if (m_senderStarted || !m_congestionCtrlEnabled)
    {
        return;
    }

    uint32_t activeFlows = 0;
    Ptr<UbController> controller;
    if (m_hostNodeId != UINT32_MAX)
    {
        Ptr<Node> node = NodeList::GetNode(m_hostNodeId);
        controller = node != nullptr ? node->GetObject<UbController>() : nullptr;
        activeFlows = controller != nullptr ? controller->GetActiveSenderFlowCount() : 0u;
    }
    const uint64_t startRate = activeFlows == 0 ? m_lineRate.GetBitRate() : m_initialRate.GetBitRate();
    const uint64_t clampedStartRate = ClampBitRate(startRate);

    m_currentRate = DataRate(clampedStartRate);
    m_targetRate = DataRate(clampedStartRate);
    m_nextAvailableSendTime = Simulator::Now();
    m_senderStarted = true;
    TraceSenderState("START");

    if (!m_registeredHostFlow && controller != nullptr)
    {
        controller->IncrementActiveSenderFlowCount();
        m_registeredHostFlow = true;
    }
}

void
UbHostDcqcn::TraceSenderState(const char* reason) const
{
    if (!m_congestionCtrlEnabled || !m_senderStarted || m_hostNodeId == UINT32_MAX || m_tp == nullptr)
    {
        return;
    }

    utils::UbUtils::DcqcnSenderStateNotify(m_hostNodeId,
                                           m_tp->GetTpn(),
                                           reason,
                                           m_alpha,
                                           m_currentRate.GetBitRate(),
                                           m_targetRate.GetBitRate(),
                                           m_bytesSinceLastIncrease,
                                           m_timeRecoveryEvents,
                                           m_byteRecoveryEvents);
}

uint64_t
UbHostDcqcn::ClampBitRate(uint64_t bitRate) const
{
    return std::max<uint64_t>(1u, std::min<uint64_t>(bitRate, m_lineRate.GetBitRate()));
}

void
UbHostDcqcn::ApplyRecoveryIncrease(bool byteDriven)
{
    if (byteDriven)
    {
        ++m_byteRecoveryEvents;
    }
    else
    {
        ++m_timeRecoveryEvents;
    }

    if (std::max(m_timeRecoveryEvents, m_byteRecoveryEvents) < m_fastRecoveryLimit)
    {
        m_currentRate = DataRate(ClampBitRate(AverageRate(m_currentRate, m_targetRate).GetBitRate()));
        TraceSenderState(byteDriven ? "BYTE_RECOVERY" : "TIMER_RECOVERY");
        return;
    }

    if (std::min(m_timeRecoveryEvents, m_byteRecoveryEvents) > m_fastRecoveryLimit)
    {
        m_targetRate = DataRate(ClampBitRate(m_targetRate.GetBitRate() + m_rateHai.GetBitRate()));
    }
    else
    {
        m_targetRate = DataRate(ClampBitRate(m_targetRate.GetBitRate() + m_rateAi.GetBitRate()));
    }

    m_currentRate = DataRate(ClampBitRate(AverageRate(m_currentRate, m_targetRate).GetBitRate()));
    TraceSenderState(byteDriven ? "BYTE_RECOVERY" : "TIMER_RECOVERY");
}

void
UbHostDcqcn::EmitPendingCnp()
{
    if (!m_pendingMarked || m_tp == nullptr)
    {
        return;
    }

    m_tp->EnqueueDcqcnCnp(m_pendingEcn, m_pendingLocation);
    utils::UbUtils::DcqcnCnpNotify(m_hostNodeId, m_tp->GetTpn(), "TX", m_pendingEcn, m_pendingLocation);
    m_lastCnpSent = Simulator::Now();
    m_hasSentCnp = true;
    m_pendingMarked = false;
    m_pendingEcn = 0;
    m_pendingLocation = false;
}

void
UbHostDcqcn::ResetRecoveryState()
{
    m_timeRecoveryEvents = 0;
    m_byteRecoveryEvents = 0;
    m_bytesSinceLastIncrease = 0;
    CancelTrackedEvent(m_rateIncreaseEvent, m_rateIncreaseTrackedEvents);
    CancelTrackedEvent(m_alphaEvent, m_alphaTrackedEvents);
    m_rateIncreaseEvent = ScheduleRateIncrease();
    m_alphaEvent = ScheduleAlphaUpdate();
}

void
UbHostDcqcn::HandleRateIncreaseTimer()
{
    EnsureSenderStartState();

    if (!m_congestionCtrlEnabled)
    {
        return;
    }

    ApplyRecoveryIncrease(false);
    m_rateIncreaseEvent = ScheduleRateIncrease();
}

void
UbHostDcqcn::HandleByteCounterEvent()
{
    EnsureSenderStartState();

    if (!m_congestionCtrlEnabled)
    {
        return;
    }

    ApplyRecoveryIncrease(true);
}

void
UbHostDcqcn::HandleAlphaTimer()
{
    if (!m_congestionCtrlEnabled)
    {
        return;
    }

    m_alpha = (1.0 - m_g) * m_alpha;
    TraceSenderState("ALPHA_DECAY");
    m_alphaEvent = ScheduleAlphaUpdate();
}

void
UbHostDcqcn::ReleaseHostActiveFlow()
{
    if (!m_registeredHostFlow || m_hostNodeId == UINT32_MAX)
    {
        return;
    }

    Ptr<Node> node = NodeList::GetNode(m_hostNodeId);
    Ptr<UbController> controller = node != nullptr ? node->GetObject<UbController>() : nullptr;
    if (controller != nullptr)
    {
        controller->DecrementActiveSenderFlowCount();
    }
    m_registeredHostFlow = false;
}

void
UbHostDcqcn::SchedulePacingWakeup()
{
    if (!m_congestionCtrlEnabled || m_port == nullptr)
    {
        return;
    }

    if (!m_pacingWakeupEvent.IsExpired())
    {
        return;
    }

    const Time now = Simulator::Now();
    const Time delay = m_nextAvailableSendTime > now ? (m_nextAvailableSendTime - now) : Time(0);
    m_pacingWakeupEvent = SchedulePacing(delay);
}

void
UbHostDcqcn::HandlePacingWakeup()
{
    if (!m_congestionCtrlEnabled || m_port == nullptr)
    {
        return;
    }

    m_port->TriggerTransmit();
}

void
UbHostDcqcn::OnSenderTransportIdle()
{
    if (!m_congestionCtrlEnabled)
    {
        return;
    }

    ReleaseHostActiveFlow();
    CancelTrackedEvent(m_rateIncreaseEvent, m_rateIncreaseTrackedEvents);
    CancelTrackedEvent(m_alphaEvent, m_alphaTrackedEvents);
    CancelTrackedEvent(m_pacingWakeupEvent, m_pacingTrackedEvents);
    m_senderStarted = false;
    m_alpha = 1.0;
    m_currentRate = m_initialRate;
    m_targetRate = m_initialRate;
    m_timeRecoveryEvents = 0;
    m_byteRecoveryEvents = 0;
    m_bytesSinceLastIncrease = 0;
    m_nextAvailableSendTime = Simulator::Now();
}

void
UbHostDcqcn::OnTpDestroyRequested()
{
    CancelTrackedEvent(m_pendingCnpEvent, m_pendingCnpTrackedEvents);
    OnSenderTransportIdle();
}

void
UbHostDcqcn::OnReceiverDataPacketReceived(uint64_t psn,
                                          uint32_t size,
                                          UbIpBasedNetworkHeader header)
{
    (void)psn;
    (void)size;

    if (!m_congestionCtrlEnabled || m_tp == nullptr)
    {
        return;
    }

    if (header.GetMode() != DCQCN_FECN_MODE || header.GetFecn() == 0)
    {
        return;
    }

    m_pendingMarked = true;
    if (header.GetFecn() > m_pendingEcn)
    {
        m_pendingEcn = header.GetFecn();
    }
    m_pendingLocation = m_pendingLocation || header.GetLocation();

    const Time now = Simulator::Now();
    if (!m_hasSentCnp || now - m_lastCnpSent >= m_cnpInterval)
    {
        if (!m_pendingCnpEvent.IsExpired())
        {
            CancelTrackedEvent(m_pendingCnpEvent, m_pendingCnpTrackedEvents);
        }
        EmitPendingCnp();
        return;
    }

    if (m_pendingCnpEvent.IsExpired())
    {
        const Time delay = (m_lastCnpSent + m_cnpInterval) - now;
        m_pendingCnpEvent = SchedulePendingCnp(delay);
    }
}

UbCongestionExtTph
UbHostDcqcn::OnReceiverPrepareAckCongestionHeader(uint64_t psnStart, uint64_t psnEnd)
{
    (void)psnStart;
    (void)psnEnd;

    UbCongestionExtTph cetph;
    cetph.SetAckSequence(0);
    cetph.SetC(0);
    cetph.SetI(false);
    cetph.SetHint(0);
    return cetph;
}

TpOpcode
UbHostDcqcn::GetAckOpcode() const
{
    return TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH;
}

void
UbHostDcqcn::OnSenderDataPacketSent(uint32_t psn, uint32_t size)
{
    (void)psn;

    EnsureSenderStartState();

    if (!m_congestionCtrlEnabled || m_currentRate.GetBitRate() == 0)
    {
        return;
    }

    if (m_nextAvailableSendTime < Simulator::Now())
    {
        m_nextAvailableSendTime = Simulator::Now();
    }

    const uint64_t bits = static_cast<uint64_t>(size) * 8u;
    const double seconds =
        static_cast<double>(bits) / static_cast<double>(m_currentRate.GetBitRate());
    m_nextAvailableSendTime += Seconds(seconds);
    m_bytesSinceLastIncrease += size;

    while (m_byteCounterThreshold > 0 && m_bytesSinceLastIncrease >= m_byteCounterThreshold)
    {
        m_bytesSinceLastIncrease -= m_byteCounterThreshold;
        HandleByteCounterEvent();
    }
}

void
UbHostDcqcn::OnSenderRetransmissionPacketSent(uint32_t psn, uint32_t size)
{
    OnSenderDataPacketSent(psn, size);
}

void
UbHostDcqcn::OnSenderCongestionNotification(TpOpcode opcode,
                                            uint32_t psn,
                                            UbCongestionExtTph header)
{
    (void)psn;

    EnsureSenderStartState();

    if (!m_congestionCtrlEnabled || opcode != TpOpcode::TP_OPCODE_CNP)
    {
        return;
    }

    const uint32_t raw = header.GetRawBytes4to7();
    const uint8_t ecn = static_cast<uint8_t>((raw >> 30) & 0x3U);
    const bool location = ((raw >> 29) & 0x1U) != 0;
    if (ecn == 0)
    {
        return;
    }

    utils::UbUtils::DcqcnCnpNotify(m_hostNodeId, m_tp->GetTpn(), "RX", ecn, location);

    m_targetRate = m_currentRate;

    const double cutFactor = 1.0 - m_alpha / 2.0;
    const double cutBitRate = static_cast<double>(m_currentRate.GetBitRate()) * cutFactor;
    const uint64_t nextBitRate = ClampBitRate(static_cast<uint64_t>(cutBitRate));
    const uint64_t previousBitRate = m_currentRate.GetBitRate();
    m_currentRate = DataRate(nextBitRate);
    const Time now = Simulator::Now();
    if (previousBitRate > 0 && m_nextAvailableSendTime > now && nextBitRate > 0)
    {
        const Time outstandingDebt = m_nextAvailableSendTime - now;
        const double rescale =
            static_cast<double>(previousBitRate) / static_cast<double>(nextBitRate);
        m_nextAvailableSendTime = now + Seconds(outstandingDebt.GetSeconds() * rescale);
        if (!m_pacingWakeupEvent.IsExpired())
        {
            CancelTrackedEvent(m_pacingWakeupEvent, m_pacingTrackedEvents);
        }
        SchedulePacingWakeup();
    }
    m_alpha = (1.0 - m_g) * m_alpha + m_g;
    TraceSenderState("CNP_RATE_CUT");
    ResetRecoveryState();
}

void
UbHostDcqcn::ApplySyntheticCnpForTest()
{
    EnsureSenderStartState();
    UbCongestionExtTph cnp;
    cnp.SetAckSequence(0);
    cnp.SetRawBytes4to7(static_cast<uint32_t>(0x1U) << 30);
    OnSenderCongestionNotification(TpOpcode::TP_OPCODE_CNP, 0, cnp);
}

TypeId
UbSwitchDcqcn::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbSwitchDcqcn")
            .SetParent<UbDcqcn>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbSwitchDcqcn>()
            .AddAttribute("KminBytes",
                          "DCQCN minimum queue occupancy in bytes before FECN marking can start.",
                          UintegerValue(5 * 1024),
                          MakeUintegerAccessor(&UbSwitchDcqcn::m_kminBytes),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("KmaxBytes",
                          "DCQCN queue occupancy in bytes at which the marking probability reaches Pmax.",
                          UintegerValue(200 * 1024),
                          MakeUintegerAccessor(&UbSwitchDcqcn::m_kmaxBytes),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("Pmax",
                          "DCQCN marking probability at KmaxBytes in the RED-ECN linear region.",
                          DoubleValue(0.01),
                          MakeDoubleAccessor(&UbSwitchDcqcn::m_pmax),
                          MakeDoubleChecker<double>(0.0, 1.0));
    return tid;
}

UbSwitchDcqcn::UbSwitchDcqcn()
    : m_random(CreateObject<UniformRandomVariable>())
{
}

void
UbSwitchDcqcn::OnSwitchAttached(Ptr<UbSwitch> sw)
{
    UbCongestionControl::OnSwitchAttached(sw);
    m_switch = sw;
}

uint64_t
UbSwitchDcqcn::GetOutPortBacklogBytes(uint32_t outPort) const
{
    NS_ASSERT_MSG(m_switch != nullptr, "UbSwitchDcqcn must bind to a switch before queue inspection");

    return m_switch->GetQueueManager()->GetOutPortQueueBacklogBytes(outPort);
}

void
UbSwitchDcqcn::MaybeMarkPacket(uint32_t outPort, Ptr<Packet> p)
{
    if (!m_congestionCtrlEnabled || p == nullptr)
    {
        return;
    }

    UbDatalinkHeader dlHeader;
    p->PeekHeader(dlHeader);
    if (!dlHeader.IsPacketIpv4Header())
    {
        return;
    }

    UbDatalinkPacketHeader dlPacketHeader;
    UbIpBasedNetworkHeader networkHeader;
    p->RemoveHeader(dlPacketHeader);
    p->RemoveHeader(networkHeader);

    const auto restoreHeaders = [&]() {
        p->AddHeader(networkHeader);
        p->AddHeader(dlPacketHeader);
    };

    if (networkHeader.GetMode() == DCQCN_FECN_MODE && networkHeader.GetFecn() != 0)
    {
        restoreHeaders();
        return;
    }

    const uint64_t totalQueueOccupancy = GetOutPortBacklogBytes(outPort);
    if (totalQueueOccupancy <= m_kminBytes)
    {
        restoreHeaders();
        return;
    }

    double markProbability = 1.0;
    if (m_kmaxBytes > m_kminBytes && totalQueueOccupancy <= m_kmaxBytes)
    {
        const double occupancySpan = static_cast<double>(m_kmaxBytes - m_kminBytes);
        markProbability =
            m_pmax * (static_cast<double>(totalQueueOccupancy - m_kminBytes) / occupancySpan);
    }

    if (m_random->GetValue() > markProbability)
    {
        restoreHeaders();
        return;
    }

    networkHeader.SetMode(DCQCN_FECN_MODE);
    networkHeader.SetLocation(false);
    networkHeader.SetFecn(DCQCN_FECN_LEVEL_MARKED);

    restoreHeaders();

    utils::UbUtils::DcqcnMarkNotify(m_switch->GetObject<Node>()->GetId(),
                                    outPort,
                                    totalQueueOccupancy,
                                    markProbability);

    NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                     << " outPort=" << outPort
                     << " totalQueueOccupancy=" << totalQueueOccupancy
                     << " markProbability=" << markProbability
                     << " set FECN");
}

void
UbSwitchDcqcn::OnSwitchPostEnqueue(uint32_t inPort, uint32_t outPort, Ptr<Packet> p)
{
    (void)inPort;
    MaybeMarkPacket(outPort, p);
}

void
UbSwitchDcqcn::OnSwitchPostDequeue(uint32_t inPort, uint32_t outPort, Ptr<Packet> p)
{
    (void)inPort;
    (void)outPort;
    (void)p;
}

} // namespace ns3
