// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/log.h"
#include "ns3/simulator.h"

#include "ns3/ub-datatype.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-utils.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbTransaction");

NS_OBJECT_ENSURE_REGISTERED(UbTransaction);

TypeId UbTransaction::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbTransaction")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus");
    return tid;
}

UbTransaction::UbTransaction()
{
    NS_LOG_DEBUG("UbTransaction created");
}

UbTransaction::~UbTransaction()
{
}

UbTransaction::UbTransaction(Ptr<Node> node)
{
    m_nodeId = node->GetId();
    m_random = CreateObject<UniformRandomVariable>();
    m_random->SetAttribute("Min", DoubleValue(0.0));
    m_random->SetAttribute("Max", DoubleValue(1.0));
    m_pushWqeSegmentToTpCb = MakeCallback(&UbTransaction::OnScheduleWqeSegmentFinish, this);
}

void UbTransaction::TpInit(Ptr<UbTransportChannel> tp)
{
    uint32_t tpn = tp->GetTpn();
    m_tpnMap[tpn] = tp;
    m_tpRRIndex[tpn] = 0;
    m_tpSchedulingStatus[tpn] = false;
}

void UbTransaction::TpDeinit(uint32_t tpn)
{
    // 删除与该tp绑定的jetty
    m_tpRelatedJetties.erase(tpn);
    for (auto it = m_jettyTpGroup.begin(); it != m_jettyTpGroup.end(); ++it) {
        // 删除所有jetty中该tp的绑定关系
        auto tpIt = std::find(it->second.begin(), it->second.end(), m_tpnMap[tpn]);
        if (tpIt != it->second.end()) {
            it->second.erase(tpIt);
        }
    }
    m_tpnMap.erase(tpn);
    m_tpRRIndex.erase(tpn);
    m_tpSchedulingStatus.erase(tpn);
    m_tpRelatedRemoteRequests.erase(tpn);
}

Ptr<UbFunction> UbTransaction::GetFunction()
{
    return NodeList::GetNode(m_nodeId)->GetObject<UbController>()->GetUbFunction();
}

Ptr<UbJetty> UbTransaction::GetJetty(uint32_t jettyNum)
{
    return GetFunction()->GetJetty(jettyNum);

}

bool UbTransaction::JettyBindTp(uint32_t src, uint32_t dest, uint32_t jettyNum,
                                bool multiPath, std::vector<uint32_t> tpns)
{
    NS_LOG_DEBUG(this);
    Ptr<UbJetty> ubJetty = GetJetty(jettyNum);
    if (ubJetty == nullptr) {
        return false;
    }

    std::vector<Ptr<UbTransportChannel>> ubTransportGroup;

    for (uint32_t i = 0; i < tpns.size(); i++) {
        uint32_t tpn = tpns[i];
        ubTransportGroup.push_back(m_tpnMap[tpn]);
        if (m_tpRelatedJetties.find(tpn) == m_tpRelatedJetties.end()) {
            m_tpRelatedJetties[tpn] = std::vector<Ptr<UbJetty>>();
        }
    }
    // 在事务层模式为ROL时只能开启单路径模式
    if (m_serviceMode[jettyNum] == TransactionServiceMode::ROL) {
        NS_LOG_WARN("ROL, set to single path forced.");
        multiPath = false;
    }
    if (multiPath) {
        NS_LOG_DEBUG("Multiple tp");
        for (uint32_t i = 0; i < ubTransportGroup.size(); i++) {
            if (std::find(m_tpRelatedJetties[tpns[i]].begin(),
                          m_tpRelatedJetties[tpns[i]].end(),
                          ubJetty) == m_tpRelatedJetties[tpns[i]].end()) {
                m_tpRelatedJetties[tpns[i]].push_back(ubJetty);
            }
        }
    } else {
        NS_LOG_DEBUG("Single tp");
        // 根据随机结果选择TP
        int pos = (int)(m_random->GetValue() * ubTransportGroup.size());
        if (std::find(m_tpRelatedJetties[tpns[pos]].begin(),
                      m_tpRelatedJetties[tpns[pos]].end(),
                      ubJetty) == m_tpRelatedJetties[tpns[pos]].end()) {
            m_tpRelatedJetties[tpns[pos]].push_back(ubJetty);
        }
    }

    m_jettyTpGroup[jettyNum] = ubTransportGroup;
    return true;
}

void UbTransaction::DestroyJettyTpMap(uint32_t jettyNum)
{
    auto itJettyTp = m_jettyTpGroup.find(jettyNum);
    if (itJettyTp != m_jettyTpGroup.end()) {
        // 解除关系
        m_jettyTpGroup.erase(itJettyTp);
        NS_LOG_DEBUG("Destroyed jetty in m_jettyTpGroup");
    } else {
        NS_LOG_WARN("Jetty Tp map not found for destruction");
    }

    for (auto it = m_tpRelatedJetties.begin(); it != m_tpRelatedJetties.end(); it++) {
        for (auto jettyIt = it->second.begin(); jettyIt != it->second.end();) {
            if ((*jettyIt)->GetJettyNum() == jettyNum) {
                jettyIt = it->second.erase(jettyIt);
            } else {
                ++jettyIt;
            }
        }
    }
}

const std::vector<Ptr<UbTransportChannel>> UbTransaction::GetJettyRelatedTpVec(uint32_t jettyNum)
{
    NS_LOG_DEBUG(this);
    auto it = m_jettyTpGroup.find(jettyNum);
    if (it != m_jettyTpGroup.end()) {
        return it->second;
    }
    NS_LOG_DEBUG("UbTransportChannel vector not found");
    return {};
}

std::vector<Ptr<UbJetty>> UbTransaction::GetTpRelatedJettyVec(uint32_t tpn)
{
    NS_LOG_DEBUG(this);
    auto it = m_tpRelatedJetties.find(tpn);
    if (it != m_tpRelatedJetties.end()) {
        return it->second;
    }
    NS_LOG_DEBUG("UbJetty vector not found");
    return {};
}

void UbTransaction::TriggerScheduleWqeSegment(uint32_t jettyNum)
{
    // 遍历与该jetty绑定的tp，全部进行调度
    auto tpVec = GetJettyRelatedTpVec(jettyNum);
    if (!tpVec.empty()) {
        for (uint32_t i = 0; i < tpVec.size(); i++) {
            Simulator::ScheduleNow(&UbTransaction::ScheduleWqeSegment, this, tpVec[i]);
        }
    }
}

void UbTransaction::ApplyScheduleWqeSegment(Ptr<UbTransportChannel> tp)
{
    Simulator::ScheduleNow(&UbTransaction::ScheduleWqeSegment, this, tp);
}

void UbTransaction::ScheduleWqeSegment(Ptr<UbTransportChannel> tp)
{
    if (tp == nullptr) {
        NS_LOG_DEBUG("TP not exist, Stop schedule.");
        return;
    }
    uint32_t tpn = tp->GetTpn();
    // 若tpnmap中没有该tp记录，表示该tp对应的traffic已经完成，TP即将删除
    if (m_tpnMap.find(tpn) == m_tpnMap.end()) {
        NS_LOG_DEBUG("TP already delete, WQE finished. Stop schedule.");
        return;
    }
    // 若当前TP正处于调度状态，则结束，否则继续进行，并将状态设置为true
    if (m_tpSchedulingStatus[tpn]) {
        return;
    }
    m_tpSchedulingStatus[tpn] = true;
    // 找到tp相关的Jetty
    auto tpRelatedJetties = GetTpRelatedJettyVec(tpn);
    std::map<uint32_t, std::vector<Ptr<UbWqeSegment>>> remoteRequestSegMap;
    // 找到tp相关的remoteRequest
    if (m_tpRelatedRemoteRequests.find(tpn) != m_tpRelatedRemoteRequests.end()) {
        remoteRequestSegMap = m_tpRelatedRemoteRequests[tpn];
    }

    // 记录开始轮询的位置， 避免无限循环
    uint32_t jettyCount = tpRelatedJetties.size();
    uint32_t rrCount = jettyCount + remoteRequestSegMap.size();

    // 该TP无对应jetty，不进行调度，状态重置
    if (rrCount == 0) {
        m_tpSchedulingStatus[tpn] = false;
        return;
    }

    // 当前TP队列满，不进行调度，状态重置
    if (tp->IsWqeSegmentLimited() ) {
        tp->SetTpFullStatus(true);
        NS_LOG_DEBUG("Full TP");
        // 满队列或满segment
        m_tpSchedulingStatus[tpn] = false;
        return;
    }

    // tp的wqesegment队列长度大于2，不进行调度，状态重置
    if (tp->GetWqeSegmentVecSize() > 1) {
        NS_LOG_DEBUG("tp wqe segment vector size > 1");
        m_tpSchedulingStatus[tpn] = false;
    }
    // m_tpRRIndex每次更新时都会进行取余操作，不会大于rrCount
    // 只有某个jetty完成后删除，导致rrCount变小时才会出现这种情况。此时重置轮询位置
    if (m_tpRRIndex[tpn] > rrCount) {
        m_tpRRIndex[tpn] = 0;
    }

    Ptr<UbWqeSegment> wqeSegment = nullptr;
    // 从tpRRIndex开始轮询，找到第一个非空且可以拿到wqesegment的jetty，获取wqesegment
    for (uint32_t i = 0; i < rrCount; i++) {
        uint32_t rrIndex = (m_tpRRIndex[tpn] + i) % rrCount;
        if (rrIndex < jettyCount) { // 轮询本地jetty
            // 获取当前jetty
            Ptr<UbJetty> currentJetty = tpRelatedJetties[rrIndex];
            if (currentJetty == nullptr) {
                continue;
            }
            wqeSegment = currentJetty->GetNextWqeSegment();
            if (wqeSegment == nullptr) {
                continue;
            }
        } else { // 轮询remoteRequest
            uint32_t remoteIndex = rrIndex - jettyCount;
            auto it = remoteRequestSegMap.begin();
            std::advance(it, remoteIndex);
            if (it->second.size() == 0) {
                continue;
            }

            for (auto vecIt = it->second.begin(); vecIt != it->second.end();) {
                if (*vecIt == nullptr) {
                    vecIt = it->second.erase(vecIt);
                } else {
                    wqeSegment = *vecIt;
                    break;
                }
            }
            if (wqeSegment == nullptr) {
                continue;
            }
        }
        if (wqeSegment != nullptr) {
            m_tpRRIndex[tpn] = (rrIndex + 1) % rrCount;
            break;
        }
    }
    if (wqeSegment != nullptr) {
        wqeSegment->SetTpn(tpn);
        Simulator::ScheduleNow(&UbTransaction::OnScheduleWqeSegmentFinish, this, wqeSegment);
    } else {
        m_tpSchedulingStatus[tpn] = false;
    }

}

void UbTransaction::OnScheduleWqeSegmentFinish(Ptr<UbWqeSegment> segment)
{
    Ptr<UbTransportChannel> tp = m_tpnMap[segment->GetTpn()];
    segment->SetTpMsn(tp->GetMsnCnt());
    segment->SetPsnStart(tp->GetPsnCnt());
    tp->UpdatePsnCnt(segment->GetPsnSize());
    tp->UpDateMsnCnt(1);
    tp->PushWqeSegment(segment);
    NS_LOG_INFO("WQE Segment Sends, taskId:" << segment->GetTaskId()
        << "TASSN: "<< segment->GetTaSsn());
    tp->WqeSegmentTriggerPortTransmit(segment);
    // TP调度状态重置
    m_tpSchedulingStatus[segment->GetTpn()] = false;
    ScheduleWqeSegment(tp);
}

bool UbTransaction::ProcessWqeSegmentComplete(Ptr<UbWqeSegment> wqeSegment)
{
    Ptr<UbJetty> jetty = GetJetty(wqeSegment->GetJettyNum());
    return jetty->ProcessWqeSegmentComplete(wqeSegment->GetTaSsn());
}

void UbTransaction::TriggerTpTransmit(uint32_t jettyNum)
{
    const std::vector<Ptr<UbTransportChannel>> ubTransportGroupVec = GetJettyRelatedTpVec(jettyNum);
    for (uint32_t i = 0; i < ubTransportGroupVec.size(); i++) {
        ubTransportGroupVec[i]->ApplyNextWqeSegment();
    }
}

bool UbTransaction::IsOrderedByInitiator(uint32_t jettyNum, Ptr<UbWqe> wqe)
{
    if (m_serviceMode.find(jettyNum) == m_serviceMode.end()) {
        return false;
    }
    if (m_serviceMode[jettyNum] != TransactionServiceMode::ROI) { // 不是ROI，直接返回true
        return true;
    }
    bool res = false;
    bool orderedEmpty = m_jettyOrderedWqe[jettyNum].empty();
    switch (wqe->GetOrderType()) {
        case OrderType::ORDER_NO:
        case OrderType::ORDER_RESERVED:
            res = true;
            break;
        case OrderType::ORDER_RELAX:
            NS_ASSERT_MSG(!orderedEmpty, "RO/SO Wqe should in Ordered vector!");
            res = true;
            break;
        case OrderType::ORDER_STRONG:
            NS_ASSERT_MSG(!orderedEmpty, "RO/SO Wqe should in Ordered vector!");
            res = (m_jettyOrderedWqe[jettyNum].front() == wqe->GetWqeId());
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Transaction Order Type!");
    }
    return res;
}

void UbTransaction::SetTransactionServiceMode(uint32_t jettyNum, TransactionServiceMode mode)
{
    m_serviceMode[jettyNum] = mode;
    // ROI模式下且wqeVector中尚无该jetty的记录，则新建
    if (mode == TransactionServiceMode::ROI && m_jettyOrderedWqe.find(jettyNum) == m_jettyOrderedWqe.end()) {
        m_jettyOrderedWqe[jettyNum] = std::vector<uint32_t>();
    }
}

TransactionServiceMode UbTransaction::GetTransactionServiceMode(uint32_t jettyNum)
{
    if (m_serviceMode.find(jettyNum) != m_serviceMode.end()) {
        return m_serviceMode[jettyNum];
    } else { //默认为ROI
        return TransactionServiceMode::ROI;
    }
}


bool UbTransaction::IsOrderedByTarget(Ptr<UbWqe> wqe)
{
    NS_LOG_DEBUG("IsOrderedByTarget");
    return true;
}

bool UbTransaction::IsReliable(Ptr<UbWqe> wqe)
{
    return true;
}

bool UbTransaction::IsUnreliable(Ptr<UbWqe> wqe)
{
    return false;
}

void UbTransaction::AddWqe(uint32_t jettyNum, Ptr<UbWqe> wqe)
{
    if (m_serviceMode.find(jettyNum) != m_serviceMode.end()) {
        // ROI模式且wqe是RO或SO
        if (m_serviceMode[jettyNum] == TransactionServiceMode::ROI
            && (wqe->GetOrderType() == OrderType::ORDER_RELAX || wqe->GetOrderType() == OrderType::ORDER_STRONG)) {
            m_jettyOrderedWqe[jettyNum].push_back(wqe->GetWqeId());
        }
    } else {
        SetTransactionServiceMode(jettyNum, TransactionServiceMode::ROI);
        AddWqe(jettyNum, wqe);
    }
}

void UbTransaction::WqeFinish(uint32_t jettyNum, Ptr<UbWqe> wqe)
{
    if (m_serviceMode.find(jettyNum) == m_serviceMode.end() || m_serviceMode[jettyNum] != TransactionServiceMode::ROI) {
        return;
    }
    // 从vector中寻找该wqe并删除
    auto it = std::find(m_jettyOrderedWqe[jettyNum].begin(), m_jettyOrderedWqe[jettyNum].end(), wqe->GetWqeId());
    if (it != m_jettyOrderedWqe[jettyNum].end()) {
        m_jettyOrderedWqe[jettyNum].erase(it);
    }
}

void UbTransaction::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_tpnMap.clear();
    m_jettyOrderedWqe.clear();
    for (auto &it : m_jettyTpGroup) {
        for (auto tp : it.second) {
            tp = nullptr;
        }
    }
    m_jettyTpGroup.clear();
    for (auto &it : m_tpRelatedJetties) {
        for(auto jetty : it.second) {
            jetty = nullptr;
        }
    }
    m_tpRelatedJetties.clear();
    for (auto &it : m_tpRelatedRemoteRequests) {
        for (auto &remoteMap : it.second) {
            for (auto segment : remoteMap.second) {
                segment = nullptr;
            }
        }
    }
    m_tpRelatedRemoteRequests.clear();
    m_tpRRIndex.clear();
    m_tpSchedulingStatus.clear();
    m_random = nullptr;
    m_serviceMode.clear();
    for (auto &it : m_jettyOrderedWqe) {
        it.second.clear();
    }
    m_jettyOrderedWqe.clear();
    Object::DoDispose();
}

std::vector<uint32_t> UbTransaction::GetUselessTpns()
{
    std::vector<uint32_t> tpns;
    // 遍历tp，找出所有本地和对端都不再使用的tp
    for (auto it = m_tpnMap.begin(); it != m_tpnMap.end(); it++) {
        uint32_t tpn = it->first;
        if (!IsTpInUse(tpn) && !IsPeerTpInUse(tpn)) {
            tpns.push_back(tpn);
        }
    }
    return tpns;
}

bool UbTransaction::IsTpInUse(uint32_t tpn)
{
    NS_ASSERT_MSG(m_tpnMap.find(tpn) != m_tpnMap.end(), "no such tp.");
    auto it = m_tpRelatedJetties.find(tpn);
    if (it == m_tpRelatedJetties.end()) {
        // 不存在记录表示无jetty使用
        return false;
    }
    // 若存在记录且对应jetty数目为0，表示使用完毕，非0，表示正在使用
    return (it->second.size() != 0);
}

bool UbTransaction::IsPeerTpInUse(uint32_t tpn)
{
    auto localTpIt = m_tpnMap.find(tpn);
    NS_ASSERT_MSG(localTpIt != m_tpnMap.end(), "can't find local tpn");
    auto localTp = localTpIt->second;
    uint32_t dst = localTp->GetDest();
    uint32_t dstTpn = localTp->GetDstTpn();
    auto peerTa = NodeList::GetNode(dst)->GetObject<UbController>()->GetUbTransaction();
    return peerTa->IsTpInUse(dstTpn);
}

} // namespace ns3