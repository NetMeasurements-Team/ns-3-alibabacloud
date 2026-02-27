// SPDX-License-Identifier: GPL-2.0-only
#include <algorithm>
#include <limits>
#include "ns3/ub-controller.h"
#include "ns3/ub-header.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-routing-process.h"
#include "ns3/udp-header.h"
#include "ns3/ipv4-header.h"
using namespace utils;

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbRoutingProcess);
NS_LOG_COMPONENT_DEFINE("UbRoutingProcess");

/*-----------------------------------------UbRoutingProcess----------------------------------------------*/
TypeId UbRoutingProcess::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbRoutingProcess")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbRoutingProcess>()
        .AddAttribute("RoutingAlgorithm",
                    "Routing algorithm applied by UbRoutingProcess.",
                    EnumValue(UbRoutingAlgorithm::HASH),
                    MakeEnumAccessor(&UbRoutingProcess::m_routingAlgorithm),
                    MakeEnumChecker(UbRoutingAlgorithm::HASH, "HASH",
                                    UbRoutingAlgorithm::ADAPTIVE, "ADAPTIVE"));
    return tid;
}

UbRoutingProcess::UbRoutingProcess()
{
}

void UbRoutingProcess::AddShortestRoute(const uint32_t destIP, const std::vector<uint16_t>& outPorts)
{
    // 标准化端口集合（排序去重）
    std::vector<uint16_t> target;
    auto itRt = m_rtShortest.find(destIP);
    if (itRt != m_rtShortest.end()) {
        target.insert(target.end(), (*(itRt->second)).begin(), (*(itRt->second)).end());
    }
    target.insert(target.end(), outPorts.begin(), outPorts.end());
    std::vector<uint16_t> normalized = normalizePorts(outPorts);
    
    // 查找或创建共享端口集合
    auto it = m_portSetPool.find(normalized);
    if (it != m_portSetPool.end()) {
        //  已存在相同端口集合，共享指针
        m_rtShortest[destIP] = it->second;
    } else {
        // 创建新端口集合并加入池中
        auto sharedPorts = std::make_shared<std::vector<uint16_t>>(normalized);
        m_portSetPool[normalized] = sharedPorts;
        m_rtShortest[destIP] = sharedPorts;
    }
}

void UbRoutingProcess::AddOtherRoute(const uint32_t destIP, const std::vector<uint16_t>& outPorts)
{
    // 标准化端口集合（排序去重）
    std::vector<uint16_t> target;
    auto itRt = m_rtOther.find(destIP);
    if (itRt != m_rtOther.end()) {
        target.insert(target.end(), (*(itRt->second)).begin(), (*(itRt->second)).end());
    }
    target.insert(target.end(), outPorts.begin(), outPorts.end());
    std::vector<uint16_t> normalized = normalizePorts(target);
    
    // 查找或创建共享端口集合
    auto it = m_portSetPool.find(normalized);
    if (it != m_portSetPool.end()) {
        // 已存在相同端口集合，共享指针
        m_rtOther[destIP] = it->second;
    } else {
        // 创建新端口集合并加入池中
        auto sharedPorts = std::make_shared<std::vector<uint16_t>>(normalized);
        m_portSetPool[normalized] = sharedPorts;
        m_rtOther[destIP] = sharedPorts;
    }
}

void UbRoutingProcess::GetShortestOutPorts(const uint32_t destIP, std::vector<uint16_t>& outPorts)
{
    outPorts.clear();
    auto it = m_rtShortest.find(destIP);
    if (it != m_rtShortest.end()) {
        outPorts.insert(outPorts.end(), (*(it->second)).begin(), (*(it->second)).end());
    }
}

void UbRoutingProcess::GetOtherOutPorts(const uint32_t destIP, std::vector<uint16_t>& outPorts)
{
    outPorts.clear();
    auto it = m_rtOther.find(destIP);
    if (it != m_rtOther.end()) {
        outPorts.insert(outPorts.end(), (*(it->second)).begin(), (*(it->second)).end());
    }
}

void UbRoutingProcess::GetShortestCandidates(uint32_t &dip, uint16_t inPortId, std::vector<uint16_t>& outPorts)
{
    // 1. 首先基于目的节点的port地址进行选择
    GetShortestOutPorts(dip, outPorts);
    if (outPorts.empty()) {
        // 2. 如果找不到，掩盖port地址，使用主机的primary地址进行寻址
        Ipv4Mask mask("255.255.255.0");
        uint32_t maskedDip = Ipv4Address(dip).CombineMask(mask).Get();
        if (maskedDip != dip) {
            GetShortestOutPorts(maskedDip, outPorts);
            dip = maskedDip;
        }
    }

    // 3. 过滤掉入端口
    if (inPortId != UINT16_MAX) {
        auto it = std::remove_if(outPorts.begin(), outPorts.end(), 
                                  [inPortId](uint16_t port) { return port == inPortId; });
        outPorts.erase(it, outPorts.end());
    }
}

void UbRoutingProcess::GetNonShortestCandidates(uint32_t &dip, uint16_t inPortId, std::vector<uint16_t>& outPorts)
{
    // 1. 首先基于目的节点的port地址进行选择
    GetOtherOutPorts(dip, outPorts);
    if (outPorts.empty()) {
        // 2. 如果找不到，掩盖port地址，使用主机的primary地址进行寻址
        Ipv4Mask mask("255.255.255.0");
        uint32_t maskedDip = Ipv4Address(dip).CombineMask(mask).Get();
        if (maskedDip != dip) {
            GetOtherOutPorts(maskedDip, outPorts);
            dip = maskedDip;
        }
    }

    // 3. 过滤掉入端口
    if (inPortId != UINT16_MAX) {
        auto it = std::remove_if(outPorts.begin(), outPorts.end(), 
                                  [inPortId](uint16_t port) { return port == inPortId; });
        outPorts.erase(it, outPorts.end());
    }
}

const std::vector<uint16_t> UbRoutingProcess::GetAllOutPorts(const uint32_t destIP)
{
    std::vector<uint16_t> res;
    auto it = m_rtOther.find(destIP);
    if (it != m_rtOther.end()) {
        res.insert(res.end(), (*(it->second)).begin(), (*(it->second)).end());
    }
    it = m_rtShortest.find(destIP);
    if (it != m_rtShortest.end()) {
        res.insert(res.end(), (*(it->second)).begin(), (*(it->second)).end());
    }
    return res;
}


// 删除路由条目
bool UbRoutingProcess::RemoveShortestRoute(const uint32_t destIP)
{
    return m_rtShortest.erase(destIP) > 0;
}

// 删除路由条目
bool UbRoutingProcess::RemoveOtherRoute(const uint32_t destIP)
{
    return m_rtOther.erase(destIP) > 0;
}

int UbRoutingProcess::SelectAdaptiveOutPort(RoutingKey &rtKey, const std::vector<uint16_t>& shortestPorts,
                                             const std::vector<uint16_t>& nonShortestPorts, bool &selectedShortestPath)
{
    auto node = NodeList::GetNode(m_nodeId);
    auto ubSwitch = node->GetObject<UbSwitch>();
    auto queueManager = ubSwitch->GetQueueManager();
    uint8_t priority = rtKey.priority;

    auto calcLoadScore = [&](uint16_t outPort) -> uint64_t {
        if (queueManager == nullptr) {
            return 0;
        }
        // 使用OutPort视图统计VOQ占用
        uint64_t voqLoad = queueManager->GetOutPortBufferUsed(outPort, static_cast<uint32_t>(priority));
        
        // 加上EgressQueue的字节占用
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(outPort));
        uint64_t egressLoad = port->GetUbQueue()->GetCurrentBytes();
        
        // 总负载 = VOQ + EgressQueue
        return voqLoad + egressLoad;
    };

    // 构造总候选列表：先 shortest，后 nonShortest
    std::vector<uint16_t> candidatePorts;
    candidatePorts.insert(candidatePorts.end(), shortestPorts.begin(), shortestPorts.end());
    candidatePorts.insert(candidatePorts.end(), nonShortestPorts.begin(), nonShortestPorts.end());

    if (candidatePorts.empty()) {
        return -1;
    }

    uint64_t bestScore = std::numeric_limits<uint64_t>::max();
    std::vector<uint16_t> bestPorts;
    size_t bestIndex = 0;
    for (size_t i = 0; i < candidatePorts.size(); ++i) {
        uint16_t port = candidatePorts[i];
        uint64_t score = calcLoadScore(port);
        if (score < bestScore) {
            bestScore = score;
            bestPorts.clear();
            bestPorts.push_back(port);
            bestIndex = i;
        } else if (score == bestScore) {
            bestPorts.push_back(port);
        }
    }

    if (bestPorts.empty()) {
        return -1;
    }

    // 通过索引判断是否选中最短路径
    selectedShortestPath = (bestIndex < shortestPorts.size());
    uint16_t selectedPort = bestPorts.front();
    return selectedPort;
}

uint64_t UbRoutingProcess::CalcHash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint8_t priority)
{
    uint8_t buf[13];
    buf[0] = (sip >> 24) & 0xff;
    buf[1] = (sip >> 16) & 0xff;
    buf[2] = (sip >> 8) & 0xff;
    buf[3] = sip & 0xff;
    buf[4] = (dip >> 24) & 0xff;
    buf[5] = (dip >> 16) & 0xff;
    buf[6] = (dip >> 8) & 0xff;
    buf[7] = dip & 0xff;
    buf[8] = (sport >> 8) & 0xff;
    buf[9] = sport & 0xff;
    buf[10] = (dport >> 8) & 0xff;
    buf[11] = dport & 0xff;
    buf[12] = priority;
    std::string str(reinterpret_cast<const char*>(buf), sizeof(buf));
    uint64_t hash = Hash64(str);
    return hash;
}

int UbRoutingProcess::SelectOutPort(RoutingKey &rtKey, const std::vector<uint16_t>& shortestPorts, 
                                     const std::vector<uint16_t>& nonShortestPorts, bool &selectedShortestPath)
{
    uint32_t sip = rtKey.sip;
    uint32_t dip = rtKey.dip;
    uint16_t sport = rtKey.sport;
    uint16_t dport = rtKey.dport;
    uint8_t priority = rtKey.priority;
    bool usePacketSpray = rtKey.usePacketSpray;

    size_t totalSize = shortestPorts.size() + nonShortestPorts.size();

    if (totalSize == 0) {
        return -1;
    }

    uint64_t hash64 = 0;
    if (usePacketSpray) {
        hash64 = CalcHash(sip, dip, sport, dport, priority);
    } else {
        // usePacketSpray == LB_MODE_PER_FLOW
        hash64 = CalcHash(sip, dip, 0, 0, priority);
    }
    
    size_t idx = hash64 % totalSize;
    
    // 通过索引判断是否选中最短路径，并直接返回对应集合中的端口
    if (idx < shortestPorts.size()) {
        selectedShortestPath = true;
        return shortestPorts[idx];
    } else {
        selectedShortestPath = false;
        return nonShortestPorts[idx - shortestPorts.size()];
    }
}

// 1. GetCandidatePorts基于useShortestPath选择可用的出端口集合
// 2. 基于用户设定的UbRoutingAlgorithm在candidatePorts中选择最终的出端口
// 2.1 如果是 HASH 算法，基于五元组哈希选择出端口(如果是usePacketSpray则使用完整五元组，否则掩盖sport和dport为0)
// 2.2 如果是 ADAPTIVE 算法，基于QueueManager信息选择负载最小的出端口
// 3. 如果找不到出端口，报错
int UbRoutingProcess::GetOutPort(RoutingKey &rtKey, bool &selectedShortestPath, uint16_t inPort)
{
    uint32_t sip = rtKey.sip;
    uint32_t dip = rtKey.dip;
    uint16_t sport = rtKey.sport;
    uint16_t dport = rtKey.dport;
    uint8_t priority = rtKey.priority;
    bool useShortestPath = rtKey.useShortestPath;
    bool usePacketSpray = rtKey.usePacketSpray;
    NS_LOG_DEBUG("[UbRoutingProcess GetOutPort]: sip: " << Ipv4Address(sip)
                << " dip: " << Ipv4Address(dip)
                << " sport: " << sport
                << " dport: " << dport
                << " priority: " << (uint16_t)priority
                << " useShortestPath: " << useShortestPath
                << " usePacketSpray: " << usePacketSpray);
    
    uint32_t tempDip = dip;
    
    // 分别获取最短路径和非最短路径候选端口
    std::vector<uint16_t> shortestPorts;
    std::vector<uint16_t> nonShortestPorts;
    GetShortestCandidates(tempDip, inPort, shortestPorts);
    if (!useShortestPath) {
        // 只有在不限制最短路径时，才获取非最短路径候选端口
        GetNonShortestCandidates(tempDip, inPort, nonShortestPorts);
    }
    
    // 检查是否有可用端口
    if (shortestPorts.empty() && nonShortestPorts.empty()) {
        NS_LOG_ERROR("No candidate ports found for dip: " << Ipv4Address(dip));
        return -1;
    }

    // 基于路由算法选择出端口，同时获得 selectedShortestPath 标记
    int outPortId = -1;
    if(m_routingAlgorithm == UbRoutingProcess::UbRoutingAlgorithm::HASH){
        outPortId = SelectOutPort(rtKey, shortestPorts, nonShortestPorts, selectedShortestPath);
    } else if(m_routingAlgorithm == UbRoutingProcess::UbRoutingAlgorithm::ADAPTIVE){
        outPortId = SelectAdaptiveOutPort(rtKey, shortestPorts, nonShortestPorts, selectedShortestPath);
    }

    // 若找不到出端口，报ASSERT
    NS_ASSERT_MSG(outPortId != -1, "No available output port found");
    
    return outPortId;
}
} // namespace ns3
