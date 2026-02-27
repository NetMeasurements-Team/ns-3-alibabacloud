// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-header.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(UbIngressQueue);
NS_OBJECT_ENSURE_REGISTERED(UbQueueManager);
NS_LOG_COMPONENT_DEFINE("UbQueueManager");

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

/*----------------------------------------- UbPacketQueue ----------------------------------------------*/
bool UbPacketQueue::IsEmpty()
{
    return m_queue.empty();
}

UbPacketQueue::UbPacketQueue()
{
}

UbPacketQueue::~UbPacketQueue()
{
}

Ptr<Packet> UbPacketQueue::GetNextPacket()
{
    auto p = m_queue.front();
    m_queue.pop();
    if (!m_queue.empty()) {
        m_headArrivalTime = Simulator::Now();
    }
    return p;
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

uint32_t UbPacketQueue::GetNextPacketSize()
{
    if (GetInPortId() == GetOutPortId()) { // crd报文等控制报文
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

/*-----------------------------------------UbQueueManager----------------------------------------------*/
UbQueueManager::UbQueueManager(void)
{
}

TypeId UbQueueManager::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbQueueManager")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbQueueManager>()
        .AddAttribute("BufferSizePerInportPriority",
        "Buffer size limit per (inPort, priority) queue in bytes.",
        UintegerValue(DEFAULT_INPORT_PRIORITY_BUFFER_SIZE),
        MakeUintegerAccessor(&UbQueueManager::m_inPortPriorityBufferLimit),
        MakeUintegerChecker<uint32_t>());
    return tid;
}

void UbQueueManager::Init()
{
    // 初始化双视图统计数组
    m_inPortBuffer.resize(m_portsNum);
    for (auto& i : m_inPortBuffer) {
        i.resize(m_vlNum, 0);
    }
    m_outPortBuffer.resize(m_portsNum);
    for (auto& i : m_outPortBuffer) {
        i.resize(m_vlNum, 0);
    }
    
    NS_LOG_DEBUG("UbQueueManager Init: portsNum=" << m_portsNum 
                 << " vlNum=" << m_vlNum
                 << " inPortPriorityBufferLimit=" << m_inPortPriorityBufferLimit);
}

// ========== VOQ Dual-View Operations ==========

bool UbQueueManager::CheckVoqSpace(uint32_t inPort, uint32_t outPort, 
                                    uint32_t priority, uint32_t pSize)
{
    // Check if both views have space
    bool inPortOk = CheckInPortSpace(inPort, priority, pSize);
    bool outPortOk = CheckOutPortSpace(outPort, priority, pSize);
    
    return inPortOk && outPortOk;
}

bool UbQueueManager::CheckInPortSpace(uint32_t inPort, uint32_t priority, uint32_t pSize)
{
    bool hasSpace = (m_inPortBuffer[inPort][priority] + pSize < m_inPortPriorityBufferLimit);
    
    if (!hasSpace) {
        NS_LOG_DEBUG("CheckInPortSpace FAIL: inPort=" << inPort 
                     << " pri=" << priority
                     << " used=" << m_inPortBuffer[inPort][priority] 
                     << " limit=" << m_inPortPriorityBufferLimit);
    }
    
    return hasSpace;
}

bool UbQueueManager::CheckOutPortSpace(uint32_t outPort, uint32_t priority, uint32_t pSize)
{
    // OutPort视图纯用于统计，无物理缓冲区限制，不用于丢包决策
    uint64_t newUsage = m_outPortBuffer[outPort][priority] + pSize;
    
    NS_LOG_DEBUG("CheckOutPortSpace: outPort=" << outPort 
                 << " pri=" << priority
                 << " currentUsed=" << m_outPortBuffer[outPort][priority]
                 << " newUsage=" << newUsage);
    
    return true;  // Always return true, OutPort view has no hard limit
}

void UbQueueManager::PushToVoq(uint32_t inPort, uint32_t outPort, 
                                uint32_t priority, uint32_t pSize)
{
    // 同时更新两个视图（物理上只有一个包在VOQ中）
    m_inPortBuffer[inPort][priority] += pSize;
    m_outPortBuffer[outPort][priority] += pSize;
    
    NS_LOG_DEBUG("PushToVoq: inPort=" << inPort << " outPort=" << outPort 
                 << " pri=" << priority << " size=" << pSize
                 << " | inPortBuf=" << m_inPortBuffer[inPort][priority]
                 << " outPortBuf=" << m_outPortBuffer[outPort][priority]);
}

void UbQueueManager::PopFromVoq(uint32_t inPort, uint32_t outPort, 
                                 uint32_t priority, uint32_t pSize)
{
    // 同时更新两个视图
    m_inPortBuffer[inPort][priority] -= pSize;
    m_outPortBuffer[outPort][priority] -= pSize;
    
    NS_LOG_DEBUG("PopFromVoq: inPort=" << inPort << " outPort=" << outPort 
                 << " pri=" << priority << " size=" << pSize
                 << " | inPortBuf=" << m_inPortBuffer[inPort][priority]
                 << " outPortBuf=" << m_outPortBuffer[outPort][priority]);
}

// ========== 查询接口：InPort视图 ==========

uint64_t UbQueueManager::GetInPortBufferUsed(uint32_t inPort, uint32_t priority)
{
    return m_inPortBuffer[inPort][priority];
}

uint64_t UbQueueManager::GetTotalInPortBufferUsed(uint32_t inPort)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < m_vlNum; i++) {
        sum += m_inPortBuffer[inPort][i];
    }
    return sum;
}

// ========== 查询接口：OutPort视图 ==========

uint64_t UbQueueManager::GetOutPortBufferUsed(uint32_t outPort, uint32_t priority)
{
    return m_outPortBuffer[outPort][priority];
}

uint64_t UbQueueManager::GetTotalOutPortBufferUsed(uint32_t outPort)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < m_vlNum; i++) {
        sum += m_outPortBuffer[outPort][i];
    }
    return sum;
}

} // namespace ns3
