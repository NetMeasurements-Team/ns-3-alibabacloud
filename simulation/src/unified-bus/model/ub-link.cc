// SPDX-License-Identifier: GPL-2.0-only
#include "ub-link.h"
#include "ns3/boolean.h"
#include "ns3/log.h"
#include "ns3/ub-port.h"
#include "ns3/ub-reliable-rerouting.h"
#include "ns3/ub-switch.h"

#include <string>

#ifdef NS3_MPI
#include "ns3/mpi-interface.h"
#endif

#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif

NS_LOG_COMPONENT_DEFINE("UbLink");

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbLink);

UbLink* UbLink::s_failedLink = nullptr;
bool UbLink::s_guardReleaseScheduled = false;

namespace {

/**
 * @brief Reliable-rerouting runtime capability decision.
 *
 * The version-one model is single-process and single-simulation-thread. Compiling parallel support
 * without enabling that runtime stays permitted; enabling MPI or MTP at runtime is not.
 */
bool
IsParallelRuntimeEnabled()
{
#ifdef NS3_MPI
    if (MpiInterface::IsEnabled())
    {
        return true;
    }
#endif
#ifdef NS3_MTP
    if (MtpInterface::isEnabled())
    {
        return true;
    }
#endif
    return false;
}

} // namespace

TypeId UbLink::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbLink")
            .SetParent<Channel>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbLink>()
            .AddAttribute("Delay",
                          "Propagation delay of the point-to-point link.",
                          TimeValue(NanoSeconds(100)),
                          MakeTimeAccessor(&UbLink::m_delay),
                          MakeTimeChecker())
            .AddAttribute("EnableReliableRerouting",
                          "Arm eligible switch-to-switch link endpoints for UB 2.1 packet-level "
                          "reliable rerouting.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbLink::m_enableReliableRerouting),
                          MakeBooleanChecker())
            .AddAttribute("FailureDetectionDelay",
                          "Modeled physical-failure to endpoint-detection interval for reliable "
                          "rerouting.",
                          TimeValue(NanoSeconds(0)),
                          MakeTimeAccessor(&UbLink::m_failureDetectionDelay),
                          MakeTimeChecker())
            .AddAttribute("LastAckTimeout",
                          "Maximum wait for an actual routed LastAck.",
                          TimeValue(Seconds(1)),
                          MakeTimeAccessor(&UbLink::m_lastAckTimeout),
                          MakeTimeChecker());
    return tid;
}

UbLink::UbLink() : PointToPointChannel()
{
    NS_LOG_FUNCTION_NOARGS();
    m_nDevices = 0;
}

void UbLink::Attach(Ptr<UbPort> device)
{
    NS_LOG_FUNCTION(this << device);
    NS_ASSERT_MSG(m_nDevices < nDevices, "Only two devices permitted");
    NS_ASSERT(device != nullptr);

    m_link[m_nDevices++].m_src = device;
    //
    // If we have both devices connected to the link, then finish introducing
    // the two halves and set the links to IDLE.
    //
    if (m_nDevices == nDevices) {
        m_link[0].m_dst = m_link[1].m_src;
        m_link[1].m_dst = m_link[0].m_src;
        m_link[0].m_state = WireState::IDLE;
        m_link[1].m_state = WireState::IDLE;
        ArmEndpointsIfEligible();
    }
}

void UbLink::ArmEndpointsIfEligible()
{
    if (!m_enableReliableRerouting)
    {
        return;
    }
    NS_ABORT_MSG_IF(m_nDevices != nDevices, "Reliable rerouting arming requires a complete link");

    // A mixed host/switch topology shares one global default: only switch-to-switch links arm
    // recovery, so host access links keep their ordinary behavior.
    for (int32_t index = 0; index < nDevices; ++index)
    {
        Ptr<UbPort> port = m_link[index].m_src;
        NS_ABORT_MSG_IF(port == nullptr || port->GetNode() == nullptr,
                        "Reliable rerouting requires attached ports");
        Ptr<UbSwitch> sw = port->GetNode()->GetObject<UbSwitch>();
        if (sw == nullptr || sw->GetNodeType() != UB_SWITCH)
        {
            NS_LOG_DEBUG("Reliable rerouting stays disarmed on a link with a non-switch endpoint");
            return;
        }
        AbortIfUnsupportedRuntime(port);
    }

    NS_ABORT_MSG_IF(m_failureDetectionDelay.IsStrictlyNegative() ||
                        m_lastAckTimeout <= Time(0),
                    "Reliable rerouting delays must be non-negative");
    NS_ABORT_MSG_IF(m_failureDetectionDelay + m_lastAckTimeout < Time(0),
                    "Reliable rerouting delay sum does not fit the simulator time range");

    for (int32_t index = 0; index < nDevices; ++index)
    {
        Ptr<UbPort> port = m_link[index].m_src;
        port->GetReliableRerouting()->Arm(port, this, m_failureDetectionDelay, m_lastAckTimeout);
    }
    m_armedFailureDetectionDelay = m_failureDetectionDelay;
    m_armedLastAckTimeout = m_lastAckTimeout;
    m_reroutingArmed = true;
    NS_LOG_DEBUG("Armed reliable rerouting on link with node "
                 << m_link[0].m_src->GetNode()->GetId() << " port "
                 << m_link[0].m_src->GetIfIndex() << " and node "
                 << m_link[1].m_src->GetNode()->GetId() << " port "
                 << m_link[1].m_src->GetIfIndex());
}

void UbLink::AbortIfUnsupportedRuntime(Ptr<UbPort> port)
{
    if (!IsParallelRuntimeEnabled())
    {
        return;
    }
    NS_ABORT_MSG("Reliable rerouting does not support an enabled MPI/MTP runtime (node "
                 << port->GetNode()->GetId() << " port " << port->GetIfIndex()
                 << "). Run the model with a single simulation thread.");
}

bool UbLink::TransmitStart(Ptr<Packet> p, Ptr<UbPort> src, Time txTime)
{
    return TransmitStart(p, src, txTime, 0);
}

bool UbLink::TransmitStart(Ptr<Packet> p, Ptr<UbPort> src, Time txTime, uint64_t attemptToken)
{
    NS_LOG_FUNCTION(this << p << src);
    NS_LOG_LOGIC("UID is " << p->GetUid() << ")");

    NS_ASSERT(m_link[0].m_state != WireState::INITIALIZING);
    NS_ASSERT(m_link[1].m_state != WireState::INITIALIZING);

    uint32_t wire = src == m_link[0].m_src ? 0 : 1;

    if (!m_reroutingArmed)
    {
        Simulator::ScheduleWithContext(m_link[wire].m_dst->GetNode()->GetId(),
                                       txTime + m_delay,
                                       &UbPort::Receive,
                                       m_link[wire].m_dst,
                                       p);
        return true;
    }

    Simulator::ScheduleWithContext(m_link[wire].m_dst->GetNode()->GetId(),
                                   txTime + m_delay,
                                   &UbLink::Deliver,
                                   this,
                                   m_link[wire].m_dst,
                                   m_link[wire].m_src,
                                   p,
                                   attemptToken);

    return true;
}

void
UbLink::Deliver(Ptr<UbPort> dst, Ptr<UbPort> src, Ptr<Packet> p, uint64_t attemptToken)
{
    if (m_failed)
    {
        // The peer never completed this receipt, so the sender keeps the attempt in retry storage.
        // This is retained recovery work, not a terminal runtime packet drop.
        NS_LOG_DEBUG("Suppressed delivery on a failed link: node "
                     << dst->GetNode()->GetId() << " port " << dst->GetIfIndex()
                     << " packet uid " << p->GetUid());
        return;
    }

    // Receipt boundary, before ordinary port/switch/transport processing: releasing the sender's
    // backup happens in the same serial delivery event that hands the packet to the peer port.
    if (attemptToken != 0 && src != nullptr)
    {
        dst->GetReliableRerouting()->ReceiveAttempt(attemptToken);
        src->ConfirmTransmitAttempt(attemptToken);
    }
    dst->Receive(p);
}

void
UbLink::Fail()
{
    NS_ABORT_MSG_IF(m_nDevices != nDevices, "Cannot fail an unattached link");
    NS_ABORT_MSG_IF(!m_reroutingArmed,
                    "Reliable rerouting is not enabled on this link; enable "
                    "ns3::UbLink::EnableReliableRerouting on an eligible switch-to-switch link "
                    "before traffic starts");
    for (int32_t index = 0; index < nDevices; ++index)
    {
        AbortIfUnsupportedRuntime(m_link[index].m_src);
    }
    NS_ABORT_MSG_IF(m_failureDetectionDelay != m_armedFailureDetectionDelay ||
                        m_lastAckTimeout != m_armedLastAckTimeout,
                    "Reliable rerouting configuration is armed before traffic starts and cannot "
                    "change later: link attached to node "
                        << m_link[0].m_src->GetNode()->GetId() << " port "
                        << m_link[0].m_src->GetIfIndex());

    if (m_failed)
    {
        NS_LOG_DEBUG("Repeated failure action on an already failed link is a no-op");
        return;
    }

    if (s_failedLink != nullptr && s_failedLink != this)
    {
        NS_ABORT_MSG("Only one distinct link may fail per simulation; link attached to node "
                     << m_link[0].m_src->GetNode()->GetId() << " port "
                     << m_link[0].m_src->GetIfIndex() << " is already failed");
    }
    if (s_failedLink == nullptr)
    {
        s_failedLink = this;
        if (!s_guardReleaseScheduled)
        {
            s_guardReleaseScheduled = true;
            Simulator::ScheduleDestroy(&UbLink::ReleaseSimulationFailureGuard);
        }
    }

    m_failed = true;
    NS_LOG_DEBUG("Link failed at " << Simulator::Now().GetTimeStep() << " time steps; node "
                                   << m_link[0].m_src->GetNode()->GetId() << " port "
                                   << m_link[0].m_src->GetIfIndex());
    for (int32_t index = 0; index < nDevices; ++index)
    {
        Ptr<UbPort> port = m_link[index].m_src;
        port->GetReliableRerouting()->OnPhysicalFailure();
    }
}

void
UbLink::ReleaseSimulationFailureGuard()
{
    s_failedLink = nullptr;
    s_guardReleaseScheduled = false;
}

size_t UbLink::GetNDevices(void) const
{
    NS_LOG_FUNCTION_NOARGS();
    return m_nDevices;
}

Ptr<UbPort> UbLink::GetUbPort(uint32_t i) const
{
    NS_LOG_FUNCTION_NOARGS();
    NS_ASSERT(i < nDevices);
    return m_link[i].m_src;
}

Ptr<NetDevice> UbLink::GetDevice(std::size_t i) const
{
    NS_LOG_FUNCTION_NOARGS();
    return GetUbPort(i);
}

Time UbLink::GetDelay(void) const
{
    return m_delay;
}

Ptr<UbPort> UbLink::GetSource(uint32_t i) const
{
    return m_link[i].m_src;
}

Ptr<UbPort> UbLink::GetDestination(uint32_t i) const
{
    return m_link[i].m_dst;
}

Ptr<UbPort> UbLink::GetDestination(Ptr<UbPort> src) const
{
    uint32_t wire = src == m_link[0].m_src ? 0 : 1;
    return m_link[wire].m_dst;
}

bool UbLink::IsRemote(void) const
{
    return false;
}

bool UbLink::IsInitialized(void) const
{
    NS_ASSERT(m_link[0].m_state != WireState::INITIALIZING);
    NS_ASSERT(m_link[1].m_state != WireState::INITIALIZING);
    return true;
}

} // namespace ns3
