// SPDX-License-Identifier: GPL-2.0-only
#include "ub-ingress-queue.h"

#include "ns3/ub-header.h"
#include "ub-switch-allocator.h"

#include "ns3/simulator.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(UbIngressQueue);
NS_LOG_COMPONENT_DEFINE("UbIngressQueue");

/*-----------------------------------------UbIngressQueue----------------------------------------------*/

UbIngressQueue::UbIngressQueue()
{
}

UbIngressQueue::~UbIngressQueue()
{
}

TypeId UbIngressQueue::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbIngressQueue")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbIngressQueue>();
    return tid;
}

bool UbIngressQueue::IsEmpty()
{
    return true;
}
Ptr<Packet> UbIngressQueue::GetNextPacket()
{
    return nullptr;
}
uint32_t UbIngressQueue::GetNextPacketSize()
{
    return 0;
}

bool UbIngressQueue::IsControlFrame()
{
    return m_ingressPriority == 0 && m_inPortId == m_outPortId;
}

bool UbIngressQueue::IsForwardedDataPacket()
{
    return m_inPortId != m_outPortId;
}

bool UbIngressQueue::IsGeneratedDataPacket()
{
    return m_ingressPriority != 0 && m_inPortId == m_outPortId;
}

void
UbIngressQueue::BindAllocatorSummary(UbSwitchAllocator* allocator)
{
    NS_ABORT_MSG_IF(allocator == nullptr, "Cannot bind ingress summary to a null allocator");
    NS_ABORT_MSG_IF(m_allocatorOwner != nullptr && m_allocatorOwner != allocator,
                    "Ingress queue cannot be bound to multiple allocators");
    m_allocatorOwner = allocator;
    RefreshAllocatorSummary();
}

void
UbIngressQueue::UnbindAllocatorSummary(UbSwitchAllocator* allocator)
{
    if (m_allocatorOwner == nullptr)
    {
        return;
    }
    NS_ABORT_MSG_IF(m_allocatorOwner != allocator,
                    "Ingress summary must be unbound by its owning allocator");
    if (m_reportedNonEmpty)
    {
        m_allocatorOwner->UpdateIngressSummary(this, false);
    }
    m_reportedNonEmpty = false;
    m_allocatorOwner = nullptr;
}

void
UbIngressQueue::RefreshAllocatorSummary()
{
    if (m_allocatorOwner == nullptr)
    {
        return;
    }
    const bool nonEmpty = !IsEmpty();
    if (nonEmpty == m_reportedNonEmpty)
    {
        return;
    }
    m_allocatorOwner->UpdateIngressSummary(this, nonEmpty);
    m_reportedNonEmpty = nonEmpty;
}

/*----------------------------------------- UbPacketQueue ----------------------------------------------*/
bool UbPacketQueue::IsEmpty()
{
    return m_packetCount == 0;
}

UbPacketQueue::UbPacketQueue()
{
}

UbPacketQueue::~UbPacketQueue()
{
}

Ptr<Packet> UbPacketQueue::GetNextPacket()
{
    auto p = Front();
    Pop();
    return p;
}

Ptr<Packet>
UbPacketQueue::Front()
{
    NS_ASSERT_MSG(m_packetCount > 0, "Cannot read the front of an empty UbPacketQueue");
    return m_frontPacket;
}

TypeId UbPacketQueue::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPacketQueue")
        .SetGroupName("UnifiedBus")
        .SetParent<UbIngressQueue>()
        .AddConstructor<UbPacketQueue>();
    return tid;
}

IngressQueueType UbPacketQueue::GetIngressQueueType()
{
    return m_ingressQueueType;
}

void
UbPacketQueue::Push(Ptr<Packet> p)
{
    const bool wasEmpty = m_packetCount == 0;
    if (wasEmpty)
    {
        m_headArrivalTime = Simulator::Now();
        m_frontPacket = p;
    }
    else
    {
        m_overflowQueue.push(p);
    }
    ++m_packetCount;
    if (wasEmpty)
    {
        RefreshAllocatorSummary();
    }
}

void
UbPacketQueue::Pop()
{
    NS_ASSERT_MSG(m_packetCount > 0, "Cannot pop from an empty UbPacketQueue");

    if (m_packetCount == 1) {
        m_frontPacket = nullptr;
        m_packetCount = 0;
        RefreshAllocatorSummary();
        return;
    }

    m_frontPacket = m_overflowQueue.front();
    m_overflowQueue.pop();
    --m_packetCount;
    m_headArrivalTime = Simulator::Now();
}

uint32_t UbPacketQueue::GetNextPacketSize()
{
    if (IsControlFrame()) { // crd报文等控制报文
        NS_LOG_DEBUG("[UbPacketQueue GetNextPacketSize] is ctrl pkt");
        UbDatalinkControlCreditHeader  DatalinkControlCreditHeader;
        uint32_t UbDataLinkCtrlSize = DatalinkControlCreditHeader.GetSerializedSize();
        return UbDataLinkCtrlSize;
    }
    Ptr<Packet> p = Front();
    uint32_t nextPktSize = p->GetSize();
    NS_LOG_DEBUG("[UbPacketQueue GetNextPacketSize] is forward pkt, nextPktSize:" << nextPktSize);

    return nextPktSize;
}

} // namespace ns3
