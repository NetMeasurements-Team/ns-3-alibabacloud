// SPDX-License-Identifier: GPL-2.0-only
#include "ub-utils.h"

namespace utils {

void UbUtils::PrintTimestamp(const std::string &message)
{
    // 获取当前系统时间点
    auto now = std::chrono::system_clock::now();

    // 转换为time_t类型以便格式化
    std::time_t nowTime = std::chrono::system_clock::to_time_t(now);

    // 转换为本地时间结构
    std::tm localTime = *std::localtime(&nowTime);

    NS_LOG_UNCOND("[" << std::put_time(&localTime, "%H:%M:%S") << "] " << message);
}

void UbUtils::ParseTrace(bool isTest)
{
    BooleanValue val;
    g_parse_enable.GetValue(val);
    bool ParseEnable = val.Get();
    if (ParseEnable) {
        PrintTimestamp("Start Parse Trace File.");

        // 从GlobalValue获取路径
        StringValue scriptPathValue;
        g_python_script_path.GetValue(scriptPathValue);
        string python_script_path = scriptPathValue.Get();

        string cmd = "python3 " + python_script_path + " " + trace_path;
        if (isTest) {
            cmd += " true";
        } else {
            cmd += " false";
        }
        int ret = system(cmd.c_str());
        if (ret == -1) {
            NS_ASSERT_MSG(0, "parse trace failed :" << cmd);
        }
    }
}

void UbUtils::Destroy()
{
    for (auto &pair : files) {
        if (pair.second->is_open()) {
            pair.second->close();
        }
        delete pair.second;  // 释放资源
    }
    files.clear();
}

void UbUtils::CreateTraceDir()
{
    size_t last_slash_pos = g_config_path.find_last_of('/');
    string dir_path;
    if (last_slash_pos != std::string::npos)
        dir_path = g_config_path.substr(0, last_slash_pos + 1);
    else
        NS_ASSERT_MSG(0, "Not find testcase dir");
    trace_path = std::string(dir_path);
    std::string command = "rm -rf " + dir_path + "runlog && mkdir " + dir_path + "runlog";
    // 执行系统命令
    if (system(command.c_str()) != 0)
        NS_ASSERT_MSG(0, "Failed to execute command: " << command);
}

inline void UbUtils::PrintTraceInfo(string fileName, string info)
{
    // 检查文件是否已经打开
    if (files.find(fileName) == files.end()) {
        // 尝试打开文件
        std::ofstream *file = new std::ofstream(fileName.c_str(), std::ios::out | std::ios::app);
        if (!file->is_open()) {
            delete file;  // 清理资源
            NS_ASSERT_MSG(0, "Can not open File: " << fileName);
        }
        // 将文件句柄存储在map中
        files[fileName] = file;
    }

    *files[fileName] << "[" << Simulator::Now().GetSeconds() * 1e6 << "us] " << info << "\n";
}

inline void UbUtils::PrintTraceInfoNoTs(string fileName, string info)
{
    // 检查文件是否已经打开
    if (files.find(fileName) == files.end()) {
        // 尝试打开文件
        std::ofstream *file = new std::ofstream(fileName.c_str(), std::ios::out | std::ios::app);
        if (!file->is_open()) {
            delete file;  // 清理资源
            NS_ASSERT_MSG(0, "Can not open File: " << fileName);
        }
        // 将文件句柄存储在map中
        files[fileName] = file;
    }

    *files[fileName] << info << "\n";
}

inline void UbUtils::TpFirstPacketSendsNotify(
    uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn, uint32_t tpMsn, uint32_t psnSndNxt, uint32_t sPort)
{
    // 使用 std::ostringstream 来减少字符串构造的次数
    std::ostringstream oss;
    oss << "First Packet Sends, taskId: " << taskId << " srcTpn: " << tpn << " destTpn: " << dstTpn
        << " tpMsn: " << tpMsn << " psn: " << psnSndNxt << " portId: " << sPort << " lastPacket: 0";
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::TpLastPacketSendsNotify(
    uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn, uint32_t tpMsn, uint32_t psnSndNxt, uint32_t sPort)
{
    std::ostringstream oss;
    oss << "Last Packet Sends,taskId: " << taskId << " srcTpn: " << tpn << " destTpn: " << dstTpn << " tpMsn: " << tpMsn
        << " psn: " << psnSndNxt << " portId: " << sPort << " lastPacket: 1";
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::TpLastPacketACKsNotify(
    uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn, uint32_t tpMsn, uint32_t psn, uint32_t sPort)
{
    std::ostringstream oss;
    oss << "Last Packet ACKs,taskId: " << taskId << " srcTpn: " << tpn << " destTpn: " << dstTpn << " tpMsn: " << tpMsn
        << " psn: " << psn << " portId: " << sPort << " lastPacket: 1";
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::TpLastPacketReceivesNotify(
    uint32_t nodeId, uint32_t srcTpn, uint32_t dstTpn, uint32_t tpMsn, uint32_t psn, uint32_t dPort)
{
    std::ostringstream oss;
    oss << "Last Packet Receives,srcTpn: " << srcTpn << " destTpn: " << dstTpn << " tpMsn: " << tpMsn
        << " tpMsn: " << tpMsn << " psn: " << psn << " inportId: " << dPort << " lastPacket: 1";
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::TpWqeSegmentSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn)
{
    std::ostringstream oss;
    oss << "WQE Segment Sends,taskId: " << taskId << " TASSN: " << taSsn;
    string info = oss.str();
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::TpWqeSegmentCompletesNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn)
{
    std::ostringstream oss;
    oss << "WQE Segment Completes,taskId: " << taskId << " TASSN: " << taSsn;
    string info = oss.str();
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline string UbUtils::Among(string s, string ts)
{
    string res = s;
    // 添加空格使字符串和时间戳对齐
    if (s.size() >= ts.size()) {
        res.insert(0, 1, ' ');
        res.insert(res.end(), 1, ' ');
    } else {
        res.insert(0, (ts.size() - s.size()) / 2 + 1, ' ');
        res.insert(res.end(), ts.size() - s.size() - (ts.size() - s.size()) / 2 + 1, ' ');
    }
    return res;
}

void UbUtils::TpRecvNotify(UbTpRecvTraceData data)
{
    std::map<PacketType, std::string> typeMap = {
    {PacketType::PACKET, "PKT"}, {PacketType::ACK, "ACK"}, {PacketType::CONTROL_FRAME, "CONTROL"}};

    std::ostringstream oss;
    oss << "Uid:" << data.packetUid << " Psn:" << data.psn << " Src:" << data.src << " Dst:" << data.dst << " SrcTpn:" << data.srcTpn
        << " DstTpn:" << data.dstTpn << " Type:" << typeMap[data.type] << " Size:" << data.size << " TaskId:" << data.taskId;
    oss << std::endl;
    for (uint32_t i = 0; i < data.traceTag.GetTraceLenth(); i++) {
        uint32_t node = data.traceTag.GetNodeTrace(i);
        PortTrace trace = data.traceTag.GetPortTrace(node);
        if (i == 0) {
            oss << "[" << node << "][" << Among(std::to_string(trace.sendPort), std::to_string(trace.sendTime)) << "]"
                << "--->";
        } else if (i == data.traceTag.GetTraceLenth() - 1) {
            oss << "[" << Among(std::to_string(trace.recvPort), std::to_string(trace.recvTime)) << "][" << node << "]"
                << std::endl;
        } else {
            oss << "[" << Among(std::to_string(trace.recvPort), std::to_string(trace.recvTime)) << "]"
                << "[" << node << "]"
                << "[" << Among(std::to_string(trace.sendPort), std::to_string(trace.sendTime)) << "]"
                << "--->";
        }
    }
    for (uint32_t i = 0; i < data.traceTag.GetTraceLenth(); i++) {
        uint32_t node = data.traceTag.GetNodeTrace(i);
        PortTrace trace = data.traceTag.GetPortTrace(node);
        if (i == 0) {
            oss << std::string(std::to_string(node).size() + 2, ' ') << "["
                << Among(std::to_string(trace.sendTime), std::to_string(trace.sendTime)) << "]" << std::string(4, ' ');
        } else if (i == data.traceTag.GetTraceLenth() - 1) {
            oss << "[" << Among(std::to_string(trace.recvTime), std::to_string(trace.recvTime)) << "]" << std::endl;
        } else {
            oss << "[" << Among(std::to_string(trace.recvTime), std::to_string(trace.recvTime)) << "]"
                << std::string(std::to_string(node).size() + 2, ' ') << "["
                << Among(std::to_string(trace.sendTime), std::to_string(trace.sendTime)) << "]" << std::string(4, ' ');
        }
    }
    string info = oss.str();
    string pktType = typeMap[data.type];
    string fileName = trace_path + "runlog/AllPacketTrace_" + pktType + "_node_" + to_string(data.src) + ".tr";
    PrintTraceInfoNoTs(fileName, info);
}

void UbUtils::LdstRecvNotify(uint32_t packetUid, uint32_t src, uint32_t dst, PacketType type,
                             uint32_t size, uint32_t taskId, UbPacketTraceTag traceTag)
{
    std::map<PacketType, std::string> typeMap = {
    {PacketType::PACKET, "PKT"}, {PacketType::ACK, "ACK"}, {PacketType::CONTROL_FRAME, "CONTROL"}};

    std::ostringstream oss;
    oss << "Uid:" << packetUid << " Src:" << src << " Dst:" << dst
        << " Type:" << typeMap[type] << " Size:" << size << " TaskId:" << taskId;
    oss << std::endl;
    for (uint32_t i = 0; i < traceTag.GetTraceLenth(); i++) {
        uint32_t node = traceTag.GetNodeTrace(i);
        PortTrace trace = traceTag.GetPortTrace(node);
        if (i == 0) {
            oss << "[" << node << "][" << Among(std::to_string(trace.sendPort), std::to_string(trace.sendTime)) << "]"
                << "--->";
        } else if (i == traceTag.GetTraceLenth() - 1) {
            oss << "[" << Among(std::to_string(trace.recvPort), std::to_string(trace.recvTime)) << "][" << node << "]"
                << std::endl;
        } else {
            oss << "[" << Among(std::to_string(trace.recvPort), std::to_string(trace.recvTime)) << "]"
                << "[" << node << "]"
                << "[" << Among(std::to_string(trace.sendPort), std::to_string(trace.sendTime)) << "]"
                << "--->";
        }
    }
    for (uint32_t i = 0; i < traceTag.GetTraceLenth(); i++) {
        uint32_t node = traceTag.GetNodeTrace(i);
        PortTrace trace = traceTag.GetPortTrace(node);
        if (i == 0) {
            oss << std::string(std::to_string(node).size() + 2, ' ') << "["
                << Among(std::to_string(trace.sendTime), std::to_string(trace.sendTime)) << "]" << std::string(4, ' ');
        } else if (i == traceTag.GetTraceLenth() - 1) {
            oss << "[" << Among(std::to_string(trace.recvTime), std::to_string(trace.recvTime)) << "]" << std::endl;
        } else {
            oss << "[" << Among(std::to_string(trace.recvTime), std::to_string(trace.recvTime)) << "]"
                << std::string(std::to_string(node).size() + 2, ' ') << "["
                << Among(std::to_string(trace.sendTime), std::to_string(trace.sendTime)) << "]" << std::string(4, ' ');
        }
    }
    string info = oss.str();
    string pktType = typeMap[type];
    string fileName = trace_path + "runlog/AllPacketTrace_" + pktType + "_node_" + to_string(src) + ".tr";
    PrintTraceInfoNoTs(fileName, info);
}

inline void UbUtils::LdstFirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId)
{
    string info = "First Packet Sends,taskId: " + std::to_string(taskId);
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::DagMemTaskStartsNotify(uint32_t nodeId, uint32_t taskId)
{
    string info = "MEM Task Starts, taskId: " + std::to_string(taskId);
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::DagMemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId)
{
    string info = "MEM Task Completes, taskId: " + std::to_string(taskId);
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::DagWqeTaskStartsNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId)
{
    std::ostringstream oss;
    oss << "WQE Starts, jettyNum: " << jettyNum << " taskId: " << taskId;
    string info = oss.str();
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::DagWqeTaskCompletesNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId)
{
    std::ostringstream oss;
    oss << "WQE Completes, jettyNum: " << jettyNum << " taskId: " << taskId;
    string info = oss.str();
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::PortTxNotify(uint32_t nodeId, uint32_t portId, uint32_t size)
{
    std::ostringstream oss;
    oss << "Port Tx, port ID: " << portId << " PacketSize: " << size;
    string info = oss.str();
    string fileName = trace_path + "runlog/PortTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::PortRxNotify(uint32_t nodeId, uint32_t portId, uint32_t size)
{
    std::ostringstream oss;
    oss << "Port Rx, port ID: " << portId << " PacketSize: " << size;
    string info = oss.str();
    string fileName = trace_path + "runlog/PortTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstThreadMemTaskStartsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    string info = "Mem Task Starts,taskId: " + std::to_string(memTaskId);
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstMemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId)
{
    string info = "Mem Task Completes,taskId: " + std::to_string(taskId);
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstThreadFirstPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    string info = "First Packet Sends, taskId: " + std::to_string(memTaskId);
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstThreadLastPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    string info = "Last Packet Sends, taskId: " + std::to_string(memTaskId);
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstLastPacketACKsNotify(uint32_t nodeId, uint32_t taskId)
{
    string info = "Last Packet ACKs,taskId: " + std::to_string(taskId);
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstPeerSendFirstPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t type)
{
    std::ostringstream oss;
    oss << "Peer Send First Packet ACKs, taskId: " << taskId << " type: " << type;
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::SwitchLastPacketTraversesNotify(uint32_t nodeId, UbTransportHeader ubTpHeader)
{
    if (ubTpHeader.GetLastPacket()) {
        std::ostringstream oss;
        oss << "Last Packet Traverses ,NodeId: " << nodeId << " srcTpn: " << ubTpHeader.GetSrcTpn()
            << " destTpn: " << ubTpHeader.GetDestTpn() << " tpMsn: " << ubTpHeader.GetTpMsn()
            << " psn:" << ubTpHeader.GetPsn();
        string info = oss.str();
        string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
        PrintTraceInfo(fileName, info);
    }
}

// 读取拓扑文件
void UbUtils::CreateTopo(const string &filename)
{
    ifstream file(filename);
    if (!file.is_open())
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
    string line;
    getline(file, line);
    // node1,port1,node2,port2,bandwidth,delay 0,0,2,0,400Gbps,10ns
    while (getline(file, line)) {
        vector<string> row;
        stringstream ss(line);
        // 跳过空行、#开头行、纯空格行
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        string cell;
        uint32_t node1;
        uint32_t port1;
        uint32_t node2;
        uint32_t port2;
        string delay;
        string bandwidth;
        getline(ss, cell, ',');
        node1 = static_cast<uint32_t>(stoi(cell));
        getline(ss, cell, ',');
        port1 = static_cast<uint32_t>(stoi(cell));
        getline(ss, cell, ',');
        node2 = static_cast<uint32_t>(stoi(cell));
        getline(ss, cell, ',');
        port2 = static_cast<uint32_t>(stoi(cell));
        getline(ss, cell, ',');
        bandwidth = cell;
        getline(ss, cell, ',');
        delay = cell;
        Ptr<Node> n1 = NodeList::GetNode(node1);
        Ptr<Node> n2 = NodeList::GetNode(node2);

        Ptr<UbPort> p1 = DynamicCast<UbPort>(n1->GetDevice(port1));
        Ptr<UbPort> p2 = DynamicCast<UbPort>(n2->GetDevice(port2));
        p1->SetDataRate(DataRate(bandwidth));
        p2->SetDataRate(DataRate(bandwidth));
        Ptr<UbLink> channel = CreateObject<UbLink>();
        channel->SetAttribute("Delay", StringValue(delay));
        p1->Attach(channel);
        p2->Attach(channel);
    }

    for (auto it = NodeList::Begin(); it != NodeList::End(); ++it) {
        Ptr<Node> node = *it;
        Ptr<UbCongestionControl> congestionCtrl = node->GetObject<ns3::UbSwitch>()->GetCongestionCtrl();
        if (congestionCtrl->GetCongestionAlgo() == CAQM) {
            Ptr<UbSwitchCaqm> swCaqm = DynamicCast<UbSwitchCaqm>(congestionCtrl);
            swCaqm->ResetLocalCc();
        }
    }
    file.close();
}

// 解析节点范围（如 "1..4"）
inline void UbUtils::ParseNodeRange(const string &rangeStr, NodeEle nodeEle)
{
    size_t dotPos = rangeStr.find("..");
    if (dotPos != string::npos) {
        // 处理范围格式
        uint32_t start = stoi(rangeStr.substr(0, dotPos));
        uint32_t end = stoi(rangeStr.substr(dotPos + 2));
        for (auto i = start; i <= end; ++i) {
            nodeEle_map[i]=nodeEle;
        }
    } else {
        // 处理单个节点
        nodeEle_map[stoi(rangeStr)] = nodeEle;
    }
}

// 创建node
void UbUtils::CreateNode(const string &filename)
{
    PrintTimestamp("Create node.");
    ifstream file(filename);
    if (!file.is_open()) {
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
    }
    string line;
    // 跳过标题行
    getline(file, line);
    while (getline(file, line)) {
        // 跳过空行、#开头行、纯空格行
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        stringstream ss(line);
        string nodeIdStr;
        string nodeTypeStr;
        string portNumStr;
        string forwardDelay;
        // 解析CSV行
        getline(ss, nodeIdStr, ',');
        getline(ss, nodeTypeStr, ',');
        getline(ss, portNumStr, ',');
        getline(ss, forwardDelay);

        NodeEle nodeEle = {};
        nodeEle.nodeIdStr = nodeIdStr;
        nodeEle.nodeTypeStr = nodeTypeStr;
        nodeEle.portNumStr = portNumStr;
        nodeEle.forwardDelay = forwardDelay;

        // 解析节点ID（范围 or 单个节点）
        ParseNodeRange(nodeIdStr, nodeEle);
    }
    file.close();
    // 创建节点
    for (auto it: nodeEle_map) {
        string nodeIdStr = it.second.nodeIdStr;
        string nodeTypeStr = it.second.nodeTypeStr;
        string portNumStr = it.second.portNumStr;
        string forwardDelay = it.second.forwardDelay;
        int portNum = stoi(portNumStr);
        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<ns3::UbLdstInstance> ldst = CreateObject<UbLdstInstance>();
        node->AggregateObject(ldst);
        ldst->Init(node->GetId());
        if (nodeTypeStr == "DEVICE") {
            Ptr<UbController> ubCtrl = CreateObject<UbController>();
            node->AggregateObject(ubCtrl);
            ubCtrl->CreateUbFunction();
            ubCtrl->CreateUbTransaction();
            sw->SetNodeType(UB_DEVICE);
        } else if (nodeTypeStr == "SWITCH") {
            sw->SetNodeType(UB_SWITCH);
        } else {
            NS_ASSERT_MSG(0, "node type not support");
        }
        for (int i = 0; i < portNum; i++) {
            Ptr<UbPort> port = CreateObject<UbPort>();
            port->SetAddress(Mac48Address::Allocate());
            node->AddDevice(port);
        }
        sw->Init();
        auto cc = UbCongestionControl::Create(UB_SWITCH);
        cc->SwitchInit(sw);
        if (!forwardDelay.empty()) {
            auto allocator = sw->GetAllocator();
            allocator->SetAttribute("AllocationTime", StringValue(forwardDelay));
        }
    }
}

// 读取路由
void UbUtils::AddRoutingTable(const string &filename)
{
    // node_id,dest,outport,metric
    std::ifstream file(filename);
    if (!file.is_open()) {
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
    }

    uint32_t node_id;
    uint32_t destip;
    uint32_t destport;
    uint32_t outport;
    uint32_t metric;
    std::string line;
    std::vector<uint16_t> outports;
    std::vector<uint16_t> metrics;

    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::map<uint32_t, std::vector<uint16_t>>>> rtTable;
    getline(file, line);
    while (std::getline(file, line)) {
        std::vector<std::string> row;
        std::stringstream ss(line);

        // 跳过空行、#开头行、纯空格行
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        std::string cell;
        std::getline(ss, cell, ',');
        node_id = static_cast<uint32_t>(std::stoi(cell));
        std::getline(ss, cell, ',');
        destip = static_cast<uint32_t>(std::stoi(cell));
        std::getline(ss, cell, ',');
        destport = static_cast<uint32_t>(std::stoi(cell));
        std::getline(ss, cell, ',');
        // read outports
        std::stringstream sOutports(cell);
        outports.clear();
        while (sOutports >> outport) {
            outports.push_back(outport);
        }
        std::getline(ss, cell, ',');
        std::stringstream sMetrics(cell);
        metrics.clear();
        while (sMetrics >> metric) {
            metrics.push_back(metric);
        }
        if (outports.size() != metrics.size()) {
            NS_ASSERT_MSG(0, "outports size not equal metrics size!" << filename);
        }
        Ipv4Address ip_node = NodeIdToIp(destip);
        Ipv4Address ip_nodePort = NodeIdToIp(destip, destport);
        for (auto i = 0u; i < outports.size(); i++) {
            rtTable[node_id][ip_node.Get()][metrics[i]].push_back(outports[i]);
            rtTable[node_id][ip_nodePort.Get()][metrics[i]].push_back(outports[i]);
        }
    }
    for (auto &nodert : rtTable) {
        auto rt = NodeList::GetNode(nodert.first)->GetObject<ns3::UbSwitch>()->GetRoutingProcess();
        for (auto &destiprow : nodert.second) {
            auto ip = destiprow.first;
            int i = 0;
            for (auto &metricrow : destiprow.second) {
                if (i == 0) {
                    rt->AddShortestRoute(ip, metricrow.second);
                } else {
                    rt->AddOtherRoute(ip, metricrow.second);
                }
                i++;
            }
        }
    }
    file.close();
}

// 读取TP配置文件
void UbUtils::ParseLine(const std::string &line, Connection &conn)
{
    std::stringstream ss(line);
    std::string item;

    // 读取node1
    getline(ss, item, ',');
    conn.node1 = stoi(item);

    // 读取port1
    getline(ss, item, ',');
    conn.port1 = stoi(item);

    // 读取tpn1
    getline(ss, item, ',');
    conn.tpn1 = stoi(item);

    // 读取node2
    getline(ss, item, ',');
    conn.node2 = stoi(item);

    // 读取port2
    getline(ss, item, ',');
    conn.port2 = stoi(item);

    // 读取tpn2
    getline(ss, item, ',');
    conn.tpn2 = stoi(item);

    // 读取priority
    getline(ss, item, ',');
    conn.priority = stoi(item);

    // 读取metrics
    getline(ss, item, ',');
    if (!item.empty()) {
        conn.metrics = stoi(item);
    } else {
        conn.metrics = UINT32_MAX;
    }
}

void UbUtils::CreateTp(const string &filename)
{
    std::unordered_map<uint32_t, std::vector<uint32_t>> NodeTpns;
    // key1:node1 key2:node2 value:Connection
    ifstream file(filename);
    if (!file.is_open()) { // 没有TP文件则使用实时创建TP模式
        PrintTimestamp("File transport_channel.csv not found."
                       " Unable to preload TP channels. TP channels will be created on demand.");
        return ;
    }
    string line;
    // 跳过标题行
    getline(file, line);

    while (getline(file, line)) {
        // 跳过空行、#开头行、纯空格行
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        Connection conn;
        ParseLine(line, conn);
        auto sendCtrl = NodeList::GetNode(conn.node1)->GetObject<UbController>();
        auto recvCtrl = NodeList::GetNode(conn.node2)->GetObject<UbController>();
        sendCtrl->GetTpConnManager()->AddUnilateralConnection(conn, conn.node1);
        recvCtrl->GetTpConnManager()->AddUnilateralConnection(conn, conn.node2);
    }
    file.close();
    return ;
}

void UbUtils::SetRecord(int fieldCount, string field, TrafficRecord &record)
{
    switch (static_cast<FIELDCOUNT>(fieldCount)) {
        case FIELDCOUNT::TASKID:
            record.taskId = stoi(field);
            break;
        case FIELDCOUNT::SOURCENODE:
            record.sourceNode = stoi(field);
            break;
        case FIELDCOUNT::DESTNODE:
            record.destNode = stoi(field);
            break;
        case FIELDCOUNT::DATASIZE:
            record.dataSize = stoi(field);
            break;
        case FIELDCOUNT::OPTYPE:
            record.opType = field;
            break;
        case FIELDCOUNT::PRIORITY:
            record.priority = stoi(field);
            break;
        case FIELDCOUNT::DELAY:
            record.delay = field;
            break;
        case FIELDCOUNT::PHASEID:
            record.phaseId = stoi(field);
            break;
        case FIELDCOUNT::DEPENDONPHASES: {
            if (!field.empty()) {
                stringstream depStream(field);
                string dep;
                while (depStream >> dep) {
                    record.dependOnPhases.push_back(stoi(dep));
                }
            }
            break;
        }
    }
}

vector<TrafficRecord> UbUtils::ReadTrafficCSV(const string &filename)
{
    vector<TrafficRecord> records;
    ifstream file(filename);
    if (!file.is_open()) {
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
        return records;
    }
    string line;
    getline(file, line);  // 跳过标题行
    while (getline(file, line)) {
        stringstream ss(line);
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        string field;
        TrafficRecord record;
        int fieldCount = 0;
        while (getline(ss, field, ',')) {
            // 去除字段前后的空格
            field.erase(0, field.find_first_not_of(" \t"));
            field.erase(field.find_last_not_of(" \t") + 1);
            SetRecord(fieldCount, field, record);
            fieldCount++;
        }
        UbTrafficGen::Get()->SetPhaseDepend(record.phaseId, record.taskId);
        records.push_back(record);
    }
    file.close();
    return records;
}

// 从TXT文件加载配置
void UbUtils::SetComponentsAttribute(const string &filename)
{
    PrintTimestamp("Set component attributes");
    g_config_path = std::string(filename);
    std::ifstream file(filename.c_str());
    if (!file.good()) {
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
    }
    Config::SetDefault("ns3::ConfigStore::Filename", StringValue(filename));
    Config::SetDefault("ns3::ConfigStore::FileFormat", StringValue("RawText"));
    Config::SetDefault("ns3::ConfigStore::Mode", StringValue("Load"));
    ConfigStore config;
    config.ConfigureDefaults();
}

void UbUtils::TopoTraceConnect()
{
    BooleanValue val;
    g_trace_enable.GetValue(val);
    TraceEnable = val.Get();

    if (!TraceEnable) {
        return; // 若不开启总开关则直接返回
    }

    g_task_trace_enable.GetValue(val);
    TaskTraceEnable = val.Get();

    g_packet_trace_enable.GetValue(val);
    PacketTraceEnable = val.Get();

    g_port_trace_enable.GetValue(val);
    PortTraceEnable = val.Get();

    g_record_pkt_trace_enable.GetValue(val);
    RecordTraceEnabled = val.Get();

    NS_LOG_UNCOND("--- UnifiedBus Trace System Configuration ---");
    NS_LOG_UNCOND("UB_TRACE_ENABLE: " << (TraceEnable ? "ON" : "OFF"));
    if (TraceEnable) {
        NS_LOG_UNCOND("  UB_TASK_TRACE_ENABLE:   " << (TaskTraceEnable ? "ON" : "OFF") << "  (Task level events)");
        NS_LOG_UNCOND("  UB_PACKET_TRACE_ENABLE: " << (PacketTraceEnable ? "ON" : "OFF") << "  (Packet Send/ACK timestamps, essential for detailed task latency breakdown)");
        NS_LOG_UNCOND("  UB_PORT_TRACE_ENABLE:   " << (PortTraceEnable ? "ON" : "OFF") << "  (All port traffic, high volume, for throughput)");
        NS_LOG_UNCOND("  UB_RECORD_PKT_TRACE:    " << (RecordTraceEnabled ? "ON" : "OFF") << "  (Per-hop packet path tracking)");
    }
    NS_LOG_UNCOND("-------------------------------------------");

    for (uint32_t i = 0; i < NodeList::GetNNodes(); ++i) {
        Ptr<Node> node = NodeList::GetNode(i);
        Ptr<UbController> ubCtrl = node->GetObject<ns3::UbController>();
        Ptr<UbSwitch> sw = node->GetObject<ns3::UbSwitch>();
        
        if (PacketTraceEnable) {
            sw->TraceConnectWithoutContext("LastPacketTraversesNotify", MakeCallback(SwitchLastPacketTraversesNotify));
        }

        std::map<uint32_t, Ptr<UbTransportChannel>> tpnMap;
        if (ubCtrl) {
            tpnMap = ubCtrl->GetTpnMap();
            for (const auto &pair : tpnMap) { // 设置 TP的trace callback
                auto tp = pair.second;
                if (tp) {
                    if (PacketTraceEnable) {
                        tp->TraceConnectWithoutContext("FirstPacketSendsNotify", MakeCallback(TpFirstPacketSendsNotify));
                        tp->TraceConnectWithoutContext("LastPacketSendsNotify", MakeCallback(TpLastPacketSendsNotify));
                        tp->TraceConnectWithoutContext("LastPacketACKsNotify", MakeCallback(TpLastPacketACKsNotify));
                        tp->TraceConnectWithoutContext("LastPacketReceivesNotify", MakeCallback(TpLastPacketReceivesNotify));
                    }
                    if (TaskTraceEnable) {
                        tp->TraceConnectWithoutContext("WqeSegmentSendsNotify", MakeCallback(TpWqeSegmentSendsNotify));
                        tp->TraceConnectWithoutContext("WqeSegmentCompletesNotify", MakeCallback(TpWqeSegmentCompletesNotify));
                    }
                    if (RecordTraceEnabled) {
                        tp->TraceConnectWithoutContext("TpRecvNotify", MakeCallback(TpRecvNotify));
                    }
                } else {
                    NS_ASSERT_MSG(0, "TP is null");
                }
            }
            Ptr<UbLdstInstance> ldstInstance = node->GetObject<UbLdstInstance>();
            if (ldstInstance != nullptr && TaskTraceEnable) {
                ldstInstance->TraceConnectWithoutContext("MemTaskCompletesNotify", MakeCallback(LdstMemTaskCompletesNotify));
                ldstInstance->TraceConnectWithoutContext("LastPacketACKsNotify", MakeCallback(LdstLastPacketACKsNotify));
                ldstInstance->TraceConnectWithoutContext("MemTaskStartsNotify", MakeCallback(LdstThreadMemTaskStartsNotify));
                ldstInstance->TraceConnectWithoutContext("FirstPacketSendsNotify", MakeCallback(LdstThreadFirstPacketSendsNotify));
                ldstInstance->TraceConnectWithoutContext("LastPacketSendsNotify", MakeCallback(LdstThreadLastPacketSendsNotify));
            }
            if (RecordTraceEnabled) {
                auto ldstApi = ubCtrl->GetUbFunction()->GetUbLdstApi();
                ldstApi->TraceConnectWithoutContext("LdstRecvNotify", MakeCallback(LdstRecvNotify));
            }
        }

        if (PortTraceEnable) {
            uint32_t DevicesNum = node->GetNDevices();
            for (uint32_t i = 0; i < DevicesNum; i++) { // 设置 port的trace callback
                Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(i));
                if (port) {
                    port->TraceConnectWithoutContext("PortTxNotify", MakeCallback(PortTxNotify));
                    port->TraceConnectWithoutContext("PortRxNotify", MakeCallback(PortRxNotify));
                } else {
                    NS_ASSERT_MSG(0, "port is null");
                }
            }
        }
    }
}

void UbUtils::SingleTpTraceConnect(uint32_t nodeId, uint32_t tpn)
{
    BooleanValue val;
    g_trace_enable.GetValue(val);
    TraceEnable = val.Get();

    if (!TraceEnable) {
        return; // 若不开启总开关则直接返回
    }

    g_task_trace_enable.GetValue(val);
    TaskTraceEnable = val.Get();

    g_packet_trace_enable.GetValue(val);
    PacketTraceEnable = val.Get();

    g_record_pkt_trace_enable.GetValue(val);
    RecordTraceEnabled = val.Get();

    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<UbController> ubCtrl = node->GetObject<ns3::UbController>();
    Ptr<UbTransportChannel> tp = ubCtrl->GetTpByTpn(tpn);
    if (tp) {
        if (PacketTraceEnable) {
            tp->TraceConnectWithoutContext("FirstPacketSendsNotify", MakeCallback(TpFirstPacketSendsNotify));
            tp->TraceConnectWithoutContext("LastPacketSendsNotify", MakeCallback(TpLastPacketSendsNotify));
            tp->TraceConnectWithoutContext("LastPacketACKsNotify", MakeCallback(TpLastPacketACKsNotify));
            tp->TraceConnectWithoutContext("LastPacketReceivesNotify", MakeCallback(TpLastPacketReceivesNotify));
        }
        if (TaskTraceEnable) {
            tp->TraceConnectWithoutContext("WqeSegmentSendsNotify", MakeCallback(TpWqeSegmentSendsNotify));
            tp->TraceConnectWithoutContext("WqeSegmentCompletesNotify", MakeCallback(TpWqeSegmentCompletesNotify));
        }
        if (RecordTraceEnabled) {
            tp->TraceConnectWithoutContext("TpRecvNotify", MakeCallback(TpRecvNotify));
        }
    }
}

void UbUtils::ClientTraceConnect(int srcNode)
{
    if (!TraceEnable || !TaskTraceEnable) {
        return; // 若不开启trace或不开启task trace则直接返回
    }
    Ptr<ns3::UbApp> client = DynamicCast<ns3::UbApp>(NodeList::GetNode(srcNode)->GetApplication(0));
    client->TraceConnectWithoutContext("MemTaskStartsNotify", MakeCallback(DagMemTaskStartsNotify));
    client->TraceConnectWithoutContext("MemTaskCompletesNotify", MakeCallback(DagMemTaskCompletesNotify));
    client->TraceConnectWithoutContext("WqeTaskStartsNotify", MakeCallback(DagWqeTaskStartsNotify));
    client->TraceConnectWithoutContext("WqeTaskCompletesNotify", MakeCallback(DagWqeTaskCompletesNotify));
}

bool UbUtils::QueryAttributeInfo(int argc, char *argv[])
{
    std::string className;
    std::string attrName;

    CommandLine cmd;
    cmd.AddValue("ClassName", "Target class name", className);
    cmd.AddValue("AttributeName", "Target attribute name (optional)", attrName);
    cmd.Parse(argc, argv);

    if (className == "" || className.empty()) {
        return false;
    }

    TypeId tid = TypeId::LookupByName(className);  // 获取TypeId

    if (!attrName.empty()) {  // attrName not empty
        struct TypeId::AttributeInformation info;
        if (tid.LookupAttributeByName(attrName, &info)) {  // 输出单个属性值
            NS_LOG_UNCOND("Attribute: " << info.name << "\n"
                                        << "Description: " << info.help << "\n"
                                        << "DataType: " << info.checker->GetValueTypeName() << "\n"
                                        << "Default: " << info.initialValue->SerializeToString(info.checker));
        } else {
            NS_LOG_UNCOND("Attribute not found!");
        }
    } else {  // 输出所有属性
        for (uint32_t i = 0; i < tid.GetAttributeN(); ++i) {
            TypeId::AttributeInformation info = tid.GetAttribute(i);
            NS_LOG_UNCOND(
                "Attribute: " << info.name << "\n"
                              << "Description: " << info.help << "\n"
                              << "DataType: " << info.checker->GetValueTypeName() << "\n"
                              << "Default: " << info.initialValue->SerializeToString(info.checker));  // 输出属性信息
        }
    }
    return true;  // 执行完后退出程序
}

void UbUtils::InitFaultMoudle(const string &FaultConfigFile)
{
    Ptr<UbFault> ubFault = CreateObject<UbFault>();
    for (auto it = NodeList::Begin(); it != NodeList::End(); ++it) {
        Ptr<Node> node = *it;
        uint16_t portNum = node->GetNDevices();
        for (int i = 0; i < portNum; i++) {
            Ptr<UbPort> port = DynamicCast<ns3::UbPort>(node->GetDevice(i));
            port->SetFaultCallBack(MakeCallback(&UbFault::FaultCallback, ubFault));
        }
    }

    ubFault->InitFault(FaultConfigFile);
}

} // namespace utils