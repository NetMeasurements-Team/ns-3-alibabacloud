// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-switch-allocator.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-port.h"
#include "protocol/ub-routing-process.h"
#include "protocol/ub-transport.h"
#include "ub-queue-manager.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(UbSwitchAllocator);
NS_OBJECT_ENSURE_REGISTERED(UbDwrrAllocator);
NS_LOG_COMPONENT_DEFINE("UbSwitchAllocator");

TypeId UbSwitchAllocator::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbSwitchAllocator")
        .SetParent<Object>()
        .AddConstructor<UbSwitchAllocator>()
        .AddAttribute("AllocationTime",
                      "Time of Allocation Used.",
                      TimeValue(NanoSeconds(10)),
                      MakeTimeAccessor(&UbSwitchAllocator::m_allocationTime),
                      MakeTimeChecker());
    return tid;
}
UbSwitchAllocator::UbSwitchAllocator()
{
}

UbSwitchAllocator::~UbSwitchAllocator()
{
}

void UbSwitchAllocator::DoDispose()
{
    m_ingressSources.clear();
    m_isRunning.clear();
    m_oneMoreRound.clear();
}

void UbSwitchAllocator::TriggerAllocator(Ptr<UbPort> outPort)
{
    std::string typeName = GetInstanceTypeId().GetName();
    NS_LOG_DEBUG("[" << typeName << " TriggerAllocator] portId: " << outPort->GetIfIndex());

    auto outPortId = outPort->GetIfIndex();

    if (outPortId >= m_isRunning.size()) {
         NS_LOG_WARN("Port ID out of range in Allocator");
         return;
    }

    if (m_isRunning[outPortId]) {
        // one more round flag
        // 为了避免 running 过程中新生成的包：
        // 1. 无法被当前轮次调度
        // 2. 下一次 trigger 会被当前轮次的状态掩盖
        m_oneMoreRound[outPortId] = true;
        NS_LOG_DEBUG("[" << typeName << " TriggerAllocator] Allocator is running, will retrigger.");
        return;
    }
    m_isRunning[outPortId] = true;
    Simulator::Schedule(m_allocationTime, &UbSwitchAllocator::AllocateNextPacket, this, outPort);
}

void UbSwitchAllocator::AllocateNextPacket(Ptr<UbPort> outPort)
{
}

void UbSwitchAllocator::Init()
{
    Simulator::Schedule(MilliSeconds(10), &UbSwitchAllocator::CheckDeadlock, this);
}

void UbSwitchAllocator::RegisterUbIngressQueue(Ptr<UbIngressQueue> ingressQueue, uint32_t outPort, uint32_t priority)
{
    m_ingressSources[outPort][priority].push_back(ingressQueue);
}

void UbSwitchAllocator::CheckDeadlock()
{
    Time now = Simulator::Now();
    Time threshold = MilliSeconds(10);

    for (uint32_t outPort = 0; outPort < m_ingressSources.size(); ++outPort) {
        for (uint32_t pri = 0; pri < m_ingressSources[outPort].size(); ++pri) {
            for (const auto& queue : m_ingressSources[outPort][pri]) {
                if (queue && !queue->IsEmpty()) {
                    if (now - queue->GetHeadArrivalTime() > threshold) {
                        std::stringstream ss;
                        ss << "Potential Deadlock in Node " << m_nodeId
                           << " OutPort:" << outPort << " Pri:" << pri;

                        if (queue->GetIngressQueueType() == IngressQueueType::VOQ) {
                            ss << " QueueType:VOQ InPort:" << queue->GetInPortId();
                        } else if (queue->GetIngressQueueType() == IngressQueueType::TP &&
                                   !queue->IsGeneratedDataPacket()) {
                            auto tp = DynamicCast<UbTransportChannel>(queue);
                            if (tp) {
                                ss << " QueueType:TP TPN:" << tp->GetTpn();
                            } else {
                                ss << " QueueType:TP (Cast Failed)";
                            }
                        } else {
                            ss << " QueueType:Unknown(" << (int)queue->GetIngressQueueType() << ")";
                        }

                        ss << " Head packet stuck for " << (now - queue->GetHeadArrivalTime()).GetMilliSeconds() << " ms";
                        NS_LOG_WARN(ss.str());
                    }
                }
            }
        }
    }
    Simulator::Schedule(MilliSeconds(10), &UbSwitchAllocator::CheckDeadlock, this);
}

void UbSwitchAllocator::UnregisterUbIngressQueue(Ptr<UbIngressQueue> ingressQueue, uint32_t outPort, uint32_t priority)
{
    m_ingressSources[outPort][priority].erase(
        std::remove(m_ingressSources[outPort][priority].begin(), m_ingressSources[outPort][priority].end(), ingressQueue),
        m_ingressSources[outPort][priority].end());
}

void UbSwitchAllocator::RegisterEgressStauts(uint32_t portsNum)
{
    m_egressStatus.resize(portsNum, true);
}

void UbSwitchAllocator::SetEgressStatus(uint32_t portId, bool status)
{
    m_egressStatus[portId] = status;
}

bool UbSwitchAllocator::GetEgressStatus(uint32_t portId)
{
    return m_egressStatus[portId];
}

/*-----------------------------------------UbRoundRobinAllocator----------------------------------------------*/
TypeId UbRoundRobinAllocator::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbRoundRobinAllocator")
        .SetParent<UbSwitchAllocator>()
        .AddConstructor<UbRoundRobinAllocator>();
    return tid;
}

void UbRoundRobinAllocator::Init()
{
    UbSwitchAllocator::Init();
    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();
    m_rrIdx.resize(portsNum);
    for (auto &v: m_rrIdx) {
        v.resize(vlNum, 0);
    }
    m_ingressSources.resize(portsNum);
    m_isRunning.assign(portsNum, false);
    m_oneMoreRound.assign(portsNum, false);
    for (auto &i : m_ingressSources) {
        i.resize(vlNum);
    }
}

void UbRoundRobinAllocator::AllocateNextPacket(Ptr<UbPort> outPort)
{
    // 轮询调度
    NS_LOG_DEBUG("[UbRoundRobinAllocator AllocateNextPacket] portId: " << outPort->GetIfIndex());
    auto outPortId = outPort->GetIfIndex();
    auto ingressQueue = SelectNextIngressQueue(outPort);
    // 调度得到的ingressqueue加入egressqueue
    if (ingressQueue != nullptr) {
        auto packet = ingressQueue->GetNextPacket();
        auto inPortId = ingressQueue->GetInPortId();
        auto priority = ingressQueue->GetIngressPriority();
        auto packetEntry = std::make_tuple(inPortId, priority, packet);
        outPort->GetFlowControl()->HandleSentPacket(packet, ingressQueue);
        outPort->GetUbQueue()->DoEnqueue(packetEntry);
        outPort->GetFlowControl()->HandleReleaseOccupiedFlowControl(packet, inPortId, outPortId);

        // Packet moved from VOQ to EgressQueue, notify Switch to update buffer statistics
        if (ingressQueue->GetIngressQueueType() != IngressQueueType::TP &&
            !ingressQueue->IsGeneratedDataPacket()) {
            // Forwarded packet (not locally generated)
            auto node = NodeList::GetNode(m_nodeId);
            node->GetObject<UbSwitch>()->NotifySwitchDequeue(inPortId, outPortId, priority, packet);
        }
    }
    m_isRunning[outPortId] = false;
    // 通知port发包
    Simulator::ScheduleNow(&UbPort::NotifyAllocationFinish, outPort);
    if (m_oneMoreRound[outPortId] == true) {
        m_oneMoreRound[outPortId] = false;
        Simulator::ScheduleNow(&UbRoundRobinAllocator::TriggerAllocator, this, outPort);
        NS_LOG_DEBUG("[UbRoundRobinAllocator AllocateNextPacket] ReTriggerAllocator portId: " << outPort->GetIfIndex());
        return;
    }
}

Ptr<UbIngressQueue> UbRoundRobinAllocator::SelectNextIngressQueue(Ptr<UbPort> outPort)
{
    uint32_t idx;
    uint32_t pi;
    uint32_t outPortId = outPort->GetIfIndex();
    auto node = NodeList::GetNode(m_nodeId);
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();
    for (pi = 0 ; pi < vlNum; pi++) {
        size_t qSize = m_ingressSources[outPortId][pi].size();
        for (idx = 0; idx < qSize; idx++) {
            auto qidx = (idx + m_rrIdx[outPortId][pi]) % qSize;
            if (!m_ingressSources[outPortId][pi][qidx]->IsEmpty() &&
                !m_ingressSources[outPortId][pi][qidx]->IsLimited() &&
                !outPort->GetFlowControl()->IsFcLimited(m_ingressSources[outPortId][pi][qidx])) {
                m_rrIdx[outPortId][pi] = (qidx + 1) % qSize;
                NS_LOG_DEBUG("[UbSwitchAllocator DispatchPacket] " << " NodeId: " << node->GetId()
                << " PortId: " << outPortId <<" qidx: "<< qidx);
                return m_ingressSources[outPortId][pi][qidx];
            }
        }
    }
    return nullptr;
}


/*-----------------------------------------UbDwrrAllocator----------------------------------------------*/

TypeId UbDwrrAllocator::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbDwrrAllocator")
        .SetParent<UbSwitchAllocator>()
        .AddConstructor<UbDwrrAllocator>()
        .AddAttribute("DefaultQuantum",
                      "Default DWRR quantum in bytes for all VLs.",
                      UintegerValue(1500),
                      MakeUintegerAccessor(&UbDwrrAllocator::m_defaultQuantum),
                      MakeUintegerChecker<uint32_t>(500, 1<<30))
        .AddAttribute("VlQuantums",
                      "Per-VL quantum overrides as 'vl:bytes,vl:bytes', e.g. '7:6000,8:12000'.",
                      StringValue(""),
                      MakeStringAccessor(&UbDwrrAllocator::m_vlQuantumsStr),
                      MakeStringChecker())
        ;
    return tid;
}

void UbDwrrAllocator::Init()
{
    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    m_rrIdx.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_quantum.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_deficit.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_lastSelectedQIdx.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_currVlIdx.assign(portsNum, 0);

    m_isRunning.assign(portsNum, false);
    m_oneMoreRound.assign(portsNum, false);

    m_ingressSources.resize(portsNum);
    for (auto &i : m_ingressSources) {
        i.resize(vlNum);
    }

    ApplyDefaultQuantum();
    ParseAndApplyVlQuantums(m_vlQuantumsStr);
}

void UbDwrrAllocator::SetQuantum(uint32_t priority, uint32_t quantum)
{
    for (uint32_t port = 0; port < m_quantum.size(); ++port) {
        if (priority < m_quantum[port].size()) {
            m_quantum[port][priority] = quantum;
        }
    }
}

void UbDwrrAllocator::SetQuantum(uint32_t outPort, uint32_t priority, uint32_t quantum)
{
    if (outPort >= m_quantum.size()) {
        return;
    }
    if (priority >= m_quantum[outPort].size()) {
        return;
    }
    m_quantum[outPort][priority] = quantum;
}

Ptr<UbIngressQueue> UbDwrrAllocator::SelectNextIngressQueue(Ptr<UbPort> outPort)
{
    uint32_t outPortId = outPort->GetIfIndex();
    auto node = NodeList::GetNode(m_nodeId);
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    if (vlNum == 0) {
        return nullptr;
    }

    uint32_t startVl = m_currVlIdx[outPortId] % vlNum;

    for (uint32_t cnt = 0; cnt < vlNum; ++cnt) {
        uint32_t pi = (startVl + cnt) % vlNum;
        auto &queues = m_ingressSources[outPortId][pi];
        size_t qSize = queues.size();

        if (qSize == 0) {
            m_deficit[outPortId][pi] = 0;
            continue;
        }

        bool hasNonEmpty = false;
        for (uint32_t idx = 0; idx < qSize; ++idx) {
            uint32_t qidx = (idx + m_rrIdx[outPortId][pi]) % qSize;
            auto q = queues[qidx];

            if (!q->IsEmpty() &&
                !q->IsLimited() &&
                !outPort->GetFlowControl()->IsFcLimited(q)) {
                hasNonEmpty = true;

                m_deficit[outPortId][pi] += m_quantum[outPortId][pi];
                m_lastSelectedQIdx[outPortId][pi] = qidx;
                m_rrIdx[outPortId][pi] = (qidx + 1) % qSize;
                m_currVlIdx[outPortId] = (pi + 1) % vlNum;

                NS_LOG_DEBUG("[UbDwrrAllocator SelectNextIngressQueue]"
                             << " NodeId: " << node->GetId()
                             << " OutPortId: " << outPortId
                             << " VL: " << pi
                             << " qidx: " << qidx
                             << " deficit: " << m_deficit[outPortId][pi]);
                return q;
            }
        }

        if (!hasNonEmpty) {
            m_deficit[outPortId][pi] = 0;
        }
    }

    return nullptr;
}

void UbDwrrAllocator::AllocateNextPacket(Ptr<UbPort> outPort)
{
    NS_LOG_DEBUG("[UbDwrrAllocator AllocateNextPacket] portId: " << outPort->GetIfIndex());
    auto outPortId = outPort->GetIfIndex();

    // 标记这轮调度是否实际发出了至少一个包
    bool sentAny = false;

    auto ingressQueue = SelectNextIngressQueue(outPort);
    if (ingressQueue != nullptr) {
        auto priority = ingressQueue->GetIngressPriority();
        uint32_t pi = priority;
        auto &queues = m_ingressSources[outPortId][pi];
        size_t qSize = queues.size();
        if (qSize > 0) {
            uint32_t qidx = m_lastSelectedQIdx[outPortId][pi];
            uint32_t &deficit = m_deficit[outPortId][pi];

            bool first = true;

            while (deficit > 0 && qSize > 0) {
                auto q = queues[qidx];

                if (q->IsEmpty() || outPort->GetFlowControl()->IsFcLimited(q)) {
                    bool found = false;
                    for (uint32_t i = 0; i < qSize; ++i) {
                        uint32_t candIdx = (qidx + i) % qSize;
                        auto candQ = queues[candIdx];
                        if (!candQ->IsEmpty() &&
                            !candQ->IsLimited() &&
                            !outPort->GetFlowControl()->IsFcLimited(candQ)) {
                            qidx = candIdx;
                            q = candQ;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        deficit = 0;
                        break;
                    }
                }

                uint32_t pktSize = q->GetNextPacketSize();

                // 第一个包就发不出去：不扣赤字，并回滚 m_rrIdx
                if (pktSize > deficit) {
                    if (!sentAny) {
                        m_rrIdx[outPortId][pi] = qidx;
                    }
                    break;
                }

                auto packet = q->GetNextPacket();
                auto inPortId = q->GetInPortId();
                auto packetEntry = std::make_tuple(inPortId, priority, packet);
                outPort->GetFlowControl()->HandleSentPacket(packet, q);
                outPort->GetUbQueue()->DoEnqueue(packetEntry);
                outPort->GetFlowControl()->HandleReleaseOccupiedFlowControl(packet, inPortId, outPortId);

                // Packet moved from VOQ to EgressQueue, notify Switch to update buffer statistics
                if (ingressQueue->GetIngressQueueType() != IngressQueueType::TP &&
                    !ingressQueue->IsGeneratedDataPacket()) {
                    // Forwarded packet (not locally generated)
                    auto node = NodeList::GetNode(m_nodeId);
                    node->GetObject<UbSwitch>()->NotifySwitchDequeue(inPortId, outPortId, priority, packet);
                }

                sentAny = true;
                deficit -= pktSize;

                if (first) {
                    qidx = m_rrIdx[outPortId][pi];
                    first = false;
                } else {
                    m_rrIdx[outPortId][pi] = (qidx + 1) % qSize;
                    qidx = m_rrIdx[outPortId][pi];
                }
            }

            bool vlanEmpty = true;
            for (auto &qq : queues) {
                if (!qq->IsEmpty() &&
                    !outPort->GetFlowControl()->IsFcLimited(qq)) {
                    vlanEmpty = false;
                    break;
                }
            }
            if (vlanEmpty) {
                deficit = 0;
            }
        }
    }

    if (ingressQueue != nullptr && !sentAny) {
        Simulator::Schedule(m_allocationTime,
                            &UbDwrrAllocator::AllocateNextPacket,
                            this, outPort);
        return;
    }

    m_isRunning[outPortId] = false;
    // 通知 port 发包
    Simulator::ScheduleNow(&UbPort::NotifyAllocationFinish, outPort);
    if (m_oneMoreRound[outPortId]) {
        m_oneMoreRound[outPortId] = false;
        Simulator::ScheduleNow(&UbDwrrAllocator::TriggerAllocator, this, outPort);
        NS_LOG_DEBUG("[UbDwrrAllocator AllocateNextPacket] ReTriggerAllocator portId: "
                     << outPort->GetIfIndex());
    }
}

void UbDwrrAllocator::ApplyDefaultQuantum()
{
    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    uint32_t q = std::max<uint32_t>(m_defaultQuantum, 64);
    for (uint32_t p = 0; p < portsNum; ++p) {
        for (uint32_t vl = 0; vl < vlNum; ++vl) {
            m_quantum[p][vl] = q;
        }
    }
}

void UbDwrrAllocator::ParseAndApplyVlQuantums(const std::string& s)
{
    if (s.empty()) return;

    std::istringstream ss(s);
    std::string token;

    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;

        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);

        auto pos = token.find(':');
        if (pos == std::string::npos) continue;

        std::string vlStr = token.substr(0, pos);
        std::string qStr  = token.substr(pos + 1);

        vlStr.erase(0, vlStr.find_first_not_of(" \t"));
        vlStr.erase(vlStr.find_last_not_of(" \t") + 1);
        qStr.erase(0, qStr.find_first_not_of(" \t"));
        qStr.erase(qStr.find_last_not_of(" \t") + 1);

        char* endp = nullptr;
        long vl = std::strtol(vlStr.c_str(), &endp, 10);
        if (endp == vlStr.c_str() || vl < 0 || (uint32_t)vl >= vlNum) {
            NS_LOG_WARN("UbDwrrAllocator::VlQuantums invalid vl: " << vlStr);
            continue;
        }

        endp = nullptr;
        long q = std::strtol(qStr.c_str(), &endp, 10);
        if (endp == qStr.c_str() || q <= 0) {
            NS_LOG_WARN("UbDwrrAllocator::VlQuantums invalid quantum: " << qStr);
            continue;
        }

        for (uint32_t p = 0; p < portsNum; ++p) {
            m_quantum[p][(uint32_t)vl] = (uint32_t)q;
        }
    }
}

} // namespae ns3
