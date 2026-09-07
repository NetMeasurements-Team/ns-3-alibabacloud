// SPDX-License-Identifier: GPL-2.0-only
#include "ub-controller.h"
#include <ns3/log.h>
#include <algorithm>
#include "ns3/ub-transport.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-utils.h"

using namespace utils;
namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbController");
NS_OBJECT_ENSURE_REGISTERED(UbController);

TypeId UbController::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbController")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbController>();
    return tid;
}

UbController::UbController()
{
    NS_LOG_FUNCTION(this);
    m_tpnConn = CreateObject<TpConnectionManager>();
}

void UbController::CreateUbFunction()
{
    m_function = CreateObject<UbFunction>();
    m_function->Init(GetObject<Node>()->GetId());
}

void UbController::CreateUbTransaction()
{
    m_transaction = CreateObject<UbTransaction>(GetObject<Node>());
}

UbController::~UbController()
{
    NS_LOG_FUNCTION(this);
    m_numToTp.clear();
    m_tpGroups.clear();
    m_ports.clear();
    m_destinationToPortsMap.clear();
    m_portPairsToIter.clear();
    m_dstPriToTp.clear();
    m_dstPriToTpRrIndex.clear();
}

Ptr<UbTransportChannel> UbController::GetTpByTpn(uint32_t tpn)
{
    return m_numToTp[tpn];
}

// Transport channel management
// bool UbController::CreateTp(uint32_t src, uint32_t dest, uint8_t sport,
//                             uint8_t dport, UbPriority priority, uint32_t srcTpn,
//                             uint32_t dstTpn, Ptr<UbCongestionControl> congestionCtrl)
bool UbController::CreateTp(const Connection& conn, Ptr<UbCongestionControl> congestionCtrl)
{
    NS_LOG_FUNCTION(this << conn.node1 << conn.node2 << conn.port1 << conn.port2 << conn.priority << conn.tpn1);
    // 检查是否已存在
    if (m_numToTp.find(conn.tpn1) != m_numToTp.end()) {
        NS_LOG_DEBUG("Transport channel already exists");
        return false;
    }

    // 创建新的Transport Channel
    // 需要先获取当前节点
    Ptr<Node> currentNode = GetObject<Node>();

    if (!currentNode) {
        // 如果没有关联节点，可能需要从其他地方获取或设置默认值
        NS_LOG_WARN("No associated node found for UbController");
        currentNode = CreateObject<Node> ();
    }
    Ipv4Address sip = NodeIdToIp(conn.node1);
    Ipv4Address dip = NodeIdToIp(conn.node2);

    Ptr<UbTransportChannel> tp = CreateObject<UbTransportChannel>();
    tp->SetUbTransport(currentNode->GetId(), conn.node1, conn.node2, conn.tpn1, conn.tpn2, 0, static_cast<uint8_t>(conn.priority),
                       static_cast<uint16_t>(conn.port1), static_cast<uint16_t>(conn.port2), sip, dip, congestionCtrl);
    m_transaction->TpInit(tp);
    m_numToTp[conn.tpn1] = tp;
    m_transportsCount++;
    currentNode->GetObject<UbSwitch>()->RegisterTpWithAllocator(tp, conn.port1, conn.priority);  // register TP with allocator

    utils::UbUtils::Get()->SingleTpTraceConnect(conn.node1, conn.tpn1);
    NS_LOG_DEBUG("Created transport channel success");
    return true;
}

Ptr<UbTransportChannel> UbController::GetTp(uint32_t tpn)
{
    NS_LOG_FUNCTION(this);

    auto it = m_numToTp.find(tpn);
    if (it != m_numToTp.end()) {
        return it->second;
    }

    return nullptr;
}

void UbController::DestroyTp(uint32_t tpn)
{
    // 算法中删除tp记录
    GetObject<Node>()->GetObject<UbSwitch>()->RemoveTpFromAllocator(m_numToTp[tpn]);
    // 事务层删除tp记录
    m_transaction->TpDeinit(tpn);
    // 删除本地记录
    m_numToTp.erase(tpn);
    m_transportsCount--;
}

// Transport channel Group management
Ptr<UbTransportGroup> UbController::CreateTpGroup(uint32_t src, uint32_t dest,
    uint32_t type, uint32_t priority, uint32_t tpgn)
{
    NS_LOG_FUNCTION(this << src << dest << type << priority << tpgn);
    TpgTag tpgTag = GenTpGroupTag(src, dest, type, priority, tpgn);
    // 检查是否已存在
    if (m_tpGroups.find(tpgTag) != m_tpGroups.end()) {
        return m_tpGroups[tpgTag];
    }

    // 创建新的Transport Group
    Ptr<UbTransportGroup> tpGroup = CreateObject<UbTransportGroup>();
    // 初始化transport group字段...

    m_tpGroups[tpgTag] = tpGroup;

    // NS_LOG_DEBUG("Created transport group with tag " << tpgTag);
    return tpGroup;
}

Ptr<UbTransportGroup> UbController::GetTpGroup(TpgTag tpgTag)
{
    auto it = m_tpGroups.find(tpgTag);
    if (it != m_tpGroups.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<Ptr<UbTransportGroup>> UbController::GetTpGroup(uint64_t src, uint64_t dest, uint64_t priority)
{
    NS_LOG_FUNCTION(this << src << dest << priority);

    std::vector<Ptr<UbTransportGroup>> UbTransportGroupVec;

    for (auto it = m_tpGroups.begin(); it != m_tpGroups.end(); it++) {
        if (it->first.GetSrc() == src && it->first.GetDest() == dest && it->first.GetPriority() == priority) {
            UbTransportGroupVec.push_back(it->second);
        }
    }

    return UbTransportGroupVec;
}

TpgTag UbController::GenTpGroupTag(uint32_t src, uint32_t dest, uint32_t type, uint32_t priority, uint32_t tpgn)
{
    return TpgTag(src, dest, priority, type, tpgn & 0xF);
}

void UbController::DestroyTpGroup(uint32_t src, uint32_t dest, uint32_t type, uint32_t priority, uint32_t tpgn)
{
    NS_LOG_FUNCTION(this << src << dest << type << priority << tpgn);

    TpgTag tpgTag = GenTpGroupTag(src, dest, type, priority, tpgn);
    DestroyTpGroup(tpgTag);
}

void UbController::DestroyTpGroup(TpgTag tpgTag)
{
    auto it = m_tpGroups.find(tpgTag);
    if (it != m_tpGroups.end()) {
        m_tpGroups.erase(it);
    } else {
        NS_LOG_WARN("Transport group with tag not found for destruction");
    }
}

// Port management
void UbController::AddPortDestination(Ptr<UbPort> port, uint32_t destination)
{
    NS_LOG_FUNCTION(this << port << destination);
    if (!port) {
        NS_LOG_WARN("Trying to add null port");
        return;
    }

    m_destinationToPortsMap[destination].push_back(port);
    NS_LOG_DEBUG("Added port to destination " << destination);
}

void UbController::RemovePortDestination(Ptr<UbPort> port, uint32_t destination)
{
    NS_LOG_FUNCTION(this << port << destination);
    if (!port) {
        NS_LOG_WARN("Trying to remove null port");
        return;
    }

    auto it = m_destinationToPortsMap.find(destination);
    if (it != m_destinationToPortsMap.end()) {
        std::vector<Ptr<UbPort>>& ports = it->second;
        ports.erase(std::remove(ports.begin(), ports.end(), port), ports.end());

        if (ports.empty()) {
            m_destinationToPortsMap.erase(it);
        }
        NS_LOG_DEBUG("Removed port from destination " << destination);
    }
}

std::vector<Ptr<UbPort>> UbController::GetAvailablePorts(uint32_t destination) const
{
    NS_LOG_FUNCTION(this << destination);

    auto it = m_destinationToPortsMap.find(destination);
    if (it != m_destinationToPortsMap.end()) {
        return it->second;
    }

    return {};
}

Ptr<UbFunction> UbController::GetUbFunction()
{
    return m_function;
}

Ptr<UbTransaction> UbController::GetUbTransaction()
{
    return m_transaction;
}

std::map<uint32_t, Ptr<UbTransportChannel>> UbController::GetTpnMap() const
{
    return m_numToTp;
}

bool UbController::AddTpMapping(uint32_t key, Ptr<UbTransportChannel> tp)
{
    bool ret = false;

    auto it = m_tpsMapInIngressSource.find(key);
    if (it != m_tpsMapInIngressSource.end()) { // 当前tp已经映射过
        ret = false;
    } else {
        m_tpsMapInIngressSource.insert(std::make_pair(key, tp));
        ret = true;
    }

    return ret;
}

Ptr<UbTransportChannel> UbController::GetTpByMap(uint32_t key)
{
    auto it = m_tpsMapInIngressSource.find(key);
    if (it != m_tpsMapInIngressSource.end()) {
        return it->second;
    } else {
        NS_LOG_WARN("Map error!");
    }

    return nullptr;
}

bool UbController::IsTPExists(uint32_t tpn)
{
    return (m_numToTp.find(tpn) != m_numToTp.end());
}
} // namespace ns3
