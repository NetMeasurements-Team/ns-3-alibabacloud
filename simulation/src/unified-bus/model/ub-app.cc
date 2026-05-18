// SPDX-License-Identifier: GPL-2.0-only
#include "ub-app.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/callback.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-function.h"
#include "ub-traffic-gen.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-port.h"
#include "ns3/ub-utils.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbApp");

NS_OBJECT_ENSURE_REGISTERED(UbApp);
TypeId UbApp::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbApp")
            .SetParent<Application>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbApp>()
            .AddAttribute("EnableMultiPath",
                          "Enable multi-path transport: create TPs on multiple source ports toward the same destination.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbApp::m_multiPathEnable),
                          MakeBooleanChecker())
            .AddAttribute("UseShortestPaths",
                          "If true, only create TPs on source ports that belong to shortest paths.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&UbApp::m_useShortestPaths),
                          MakeBooleanChecker())
            .AddTraceSource("MemTaskStartsNotify",
                            "MEM Task Starts, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceMemTaskStartsNotify),
                            "ns3::UbApp::MemTaskStartsNotify")
            .AddTraceSource("MemTaskCompletesNotify",
                            "MEM Task Completes, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceMemTaskCompletesNotify),
                            "ns3::UbApp::MemTaskCompletesNotify")
            .AddTraceSource("WqeTaskStartsNotify",
                            "WQE Task Starts, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceWqeTaskStartsNotify),
                            "ns3::UbApp::WqeTaskStartsNotify")
            .AddTraceSource("WqeTaskCompletesNotify",
                            "WQE Task Completes, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceWqeTaskCompletesNotify),
                            "ns3::UbApp::WqeTaskCompletesNotify");
    return tid;
}

UbApp::UbApp()
{
    m_random = CreateObject<UniformRandomVariable>();
    m_random->SetAttribute("Min", DoubleValue(0.0));
    m_random->SetAttribute("Max", DoubleValue(1.0));
}

UbApp::~UbApp()
{
}

void UbApp::SetFinishCallback(Callback<void, uint32_t, uint32_t> cb, Ptr<UbJetty> jetty)
{
    jetty->SetClientCallback(cb);
}

void UbApp::SetFinishCallback(Callback<void, uint32_t> cb, Ptr<UbLdstInstance> ubLdstInstance)
{
    ubLdstInstance->SetClientCallback(cb);
}

void UbApp::DoDispose(void)
{
    Application::DoDispose();
}

void UbApp::SendTraffic(TrafficRecord record)
{
    if (record.priority == 0) {
        NS_LOG_DEBUG("Task uses the highest priority, not recommended.");
    }

    if (record.opType == "MEM_STORE" || record.opType == "MEM_LOAD") {
        // 内存语义发送
        UbMemOperationType type = UbMemOperationType::STORE;
        if (record.opType == "MEM_STORE") {
            type = UbMemOperationType::STORE;
        } else if (record.opType == "MEM_LOAD") {
            type = UbMemOperationType::LOAD;
        }
        auto ldstInstance = GetNode()->GetObject<UbLdstInstance>();
        Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
        SetFinishCallback(MakeCallback(&UbApp::OnMemTaskCompleted, this), ldstInstance);
        NS_LOG_INFO("MEM Task Starts, taskId: " << record.taskId);
        MemTaskStartsNotify(GetNode()->GetId(), record.taskId);
        std::vector<uint32_t> threadIds = {0, 1};
        ldstInstance->HandleLdstTask(record.sourceNode, record.destNode, record.dataSize,
                          record.taskId, record.priority, type, threadIds, 0);
    } else if (record.opType == "URMA_WRITE" || record.opType == "URMA_READ") {
        // URMA发送
        Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
        Ptr<UbTransaction> ubTa = GetNode()->GetObject<UbController>()->GetUbTransaction();
        bool jettyExist = ubFunc->IsJettyExists(m_jettyNum);
        if (jettyExist) {
            NS_LOG_ERROR("Jetty already exists");
            return;
        }
        ubFunc->CreateJetty(record.sourceNode, record.destNode, m_jettyNum);
        vector<uint32_t> tpns = GetNode()->GetObject<UbController>()->GetTpConnManager()->GetTpns(
            m_getTpnRule, m_useShortestPaths, m_multiPathEnable, record.sourceNode,
            record.destNode, UINT32_MAX, UINT32_MAX, record.priority);
        bool bindRst = ubTa->JettyBindTp(record.sourceNode, record.destNode, m_jettyNum, m_multiPathEnable, tpns);
        if (bindRst) {
            Ptr<UbJetty> curr_jetty = ubFunc->GetJetty(m_jettyNum);
            SetFinishCallback(MakeCallback(&UbApp::OnTaskCompleted, this), curr_jetty);
            NS_LOG_INFO("WQE Starts, jettyNum: " << m_jettyNum << " taskId: " << record.taskId);
            NS_LOG_INFO("Src: " << record.sourceNode << " Dst: " << record.destNode);
            WqeTaskStartsNotify(GetNode()->GetId(), m_jettyNum, record.taskId);
            NS_LOG_INFO("[APPLICATION INFO] taskId: " << record.taskId << ",start time:" <<
                Simulator::Now().GetNanoSeconds() << "ns");
            NS_ASSERT_MSG(TaOpcodeMap.find(record.opType) != TaOpcodeMap.end(), "TaOpcode Not Exist");
            TaOpcode type = TaOpcodeMap[record.opType];
            Ptr<UbWqe> wqe =
                ubFunc->CreateWqe(record.sourceNode, record.destNode, record.dataSize, record.taskId, type);
            ubFunc->PushWqeToJetty(wqe, m_jettyNum);
        }
        m_jettyNum++; // m_jettyNum 在client里是唯一的，不重复的
    } else {
            NS_ASSERT_MSG(0, "TaOpcode Not Exist");
    }
}

void UbApp::OnTaskCompleted(uint32_t taskId, uint32_t jettyNum)
{
    NS_LOG_FUNCTION(this << taskId);
    NS_LOG_INFO("WQE Completes, jettyNum: " << jettyNum << " taskId: " << taskId);
    WqeTaskCompletesNotify(GetNode()->GetId(), jettyNum, taskId);
    NS_LOG_INFO("[APPLICATION INFO] taskId: " << taskId << ",finish time:" << Simulator::Now().GetNanoSeconds() << "ns");
    // 删除无用tp
    TrafficRecord record = UbTrafficGen::Get()->GetTaskById(taskId);
    GetNode()->GetObject<UbController>()->GetTpConnManager()->RemoveUselessTps(jettyNum,
        record.sourceNode, record.destNode, record.priority);
    UbTrafficGen::Get()->OnTaskCompleted(taskId);
}

void UbApp::OnTestTaskCompleted(uint32_t taskId, uint32_t jettyNum)
{
    NS_LOG_FUNCTION(this << taskId);
    NS_LOG_INFO("WQE Completes, jettyNum:" << jettyNum << " taskId:" << taskId);
    WqeTaskCompletesNotify(GetNode()->GetId(), jettyNum, taskId);
    NS_LOG_INFO("[APPLICATION INFO] taskId:" << taskId << ",finish time:" << Simulator::Now().GetNanoSeconds() << "ns");
    Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
    UbTrafficGen::Get()->OnTaskCompleted(taskId);
}

void UbApp::OnMemTaskCompleted(uint32_t taskId)
{
    NS_LOG_FUNCTION(this << taskId);
    NS_LOG_INFO("MEM Task Completes, taskId: " << taskId);
    MemTaskCompletesNotify(GetNode()->GetId(), taskId);
    NS_LOG_INFO("[APPLICATION INFO] taskId: " << taskId << ",finish time:" << Simulator::Now().GetNanoSeconds() << "ns");
    UbTrafficGen::Get()->OnTaskCompleted(taskId);
}

void UbApp::MemTaskStartsNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceMemTaskStartsNotify(nodeId, taskId);
}

void UbApp::MemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceMemTaskCompletesNotify(nodeId, taskId);
}

void UbApp::WqeTaskStartsNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId)
{
    m_traceWqeTaskStartsNotify(nodeId, jettyNum, taskId);
}

void UbApp::WqeTaskCompletesNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId)
{
    m_traceWqeTaskCompletesNotify(nodeId, jettyNum, taskId);
}

} // namespace ns3
