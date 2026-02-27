// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-fault.h"
#include "ns3/node-list.h"
#include "ns3/node.h"
using namespace std;
using namespace utils;
namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(UbFault);
NS_LOG_COMPONENT_DEFINE("UbFault");

/*********************
 * UbFault
 ********************/

// UbFault
UbFault::UbFault()
{
    isInitFault = false;
}

UbFault::~UbFault()
{
    NS_LOG_FUNCTION(this);
}
TypeId UbFault::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbFault")
                            .SetParent<Object>()
                            .AddConstructor<UbFault>()
                            .AddAttribute("UbFaultUsePacketSpray", "is Packet Follow or not.", BooleanValue(false),
                                          MakeBooleanAccessor(&UbFault::isPacketFlow), MakeBooleanChecker());
    return tid;
}
vector<string> UbFault::Split(const string &s, char delim)
{
    vector<string> tokens;
    string token;
    istringstream tokenStream(s);
    while (getline(tokenStream, token, delim)) {
        if (!token.empty())
            tokens.push_back(token);
    }
    return tokens;
}

void UbFault::ReadCongestionOrLowerDataRateParams(map<uint32_t, FaultInfo> &faultMap, LowerDataRate &lowerDataRate,
                                                  const string &cell,uint32_t taskId)
{
    uint8_t congestionAndLowerDataRateParamsCount = 3;
    if (faultMap[taskId].faultType == FaultType::CONGESTION || faultMap[taskId].faultType == FaultType::LOWERDATARATE) {
        if (cell.find(' ') != string::npos) {
            vector<string> spaceParts = Split(cell, ' ');
            if (spaceParts.size() == congestionAndLowerDataRateParamsCount) {
                lowerDataRate.nodeId = static_cast<uint32_t>(stoi(spaceParts[0]));
                lowerDataRate.portId = static_cast<uint32_t>(stoi(spaceParts[1]));
                lowerDataRate.dataRate = DataRate(spaceParts[2] + "Mbps");
                // faultType==2
                // 先跑一遍流量，查看端口平均带宽信息。将最大带宽的端口在跑流量之前将带宽设置为最大带宽的一半
                // faultType==4
                // 在开始跑仿真前将拓扑的带宽减为一半（可以提前跑一遍流量看下端口级带宽，选择有流量的端口进行降lane）
                SetPortCongestion(lowerDataRate);
            } else {
                NS_LOG_WARN("Invalid fault injection config (LOWERDATARATE): incorrect number of parameters. Line ignored.");
            }
        }
    }
}

void UbFault::ReadShutDownParams(map<uint32_t, FaultInfo> &faultMap, const string &cell, uint32_t taskId)
{
    uint8_t shutDownParamsCount = 2;
    if (faultMap[taskId].faultType == FaultType::SHUTDOWNUP) {
        if (cell.find(' ') != string::npos) {
            vector<string> spaceParts = Split(cell, ' ');
            if (spaceParts.size() == shutDownParamsCount) {
                faultMap[taskId].shutDownPacketDrop = {static_cast<uint32_t>(stoi(spaceParts[0])),
                                                    static_cast<uint32_t>(stoi(spaceParts[1]))};
            } else {
                NS_LOG_WARN("Invalid fault injection config (SHUTDOWNUP): incorrect number of parameters. Line ignored.");
            }
        }
    }
}

void UbFault::InitFault(const string &filename)
{
    if (isInitFault)
        return;
    isInitFault = true;
    isPacketFlowValue = isPacketFlow.Get();
    NS_LOG_INFO("Init Fault moudle.");
    ifstream file(filename);
    if (!file.is_open()) {
        NS_LOG_DEBUG("Can not open File: " << filename);
    }

    string line;
    // 跳过标题行
    uint8_t percentSign = 100;
    getline(file, line);
    while (getline(file, line)) {
        // 跳过空行、#开头行、纯空格行
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        stringstream ss(line);
        string cell;

        getline(ss, cell, ',');
        uint32_t taskId = static_cast<uint32_t>(stoi(cell));
        faultMap[taskId];

        getline(ss, cell, ',');
        faultMap[taskId].faultType = static_cast<FaultType>(stoi(cell));

        getline(ss, cell, ',');
        faultMap[taskId].dropRate = static_cast<double>(stoi(cell)) / percentSign;

        getline(ss, cell, ',');
        faultMap[taskId].delay = static_cast<uint64_t>(stoi(cell));

        getline(ss, cell, ',');
        LowerDataRate lowerDataRate;
        ReadCongestionOrLowerDataRateParams(faultMap, lowerDataRate, cell, taskId);

        getline(ss, cell, ',');

        getline(ss, cell, ',');
        faultMap[taskId].erorDropRate = static_cast<double>(stoi(cell)) / percentSign;

        NS_LOG_DEBUG("taskId:" << taskId << ",faultType:" <<  static_cast<uint16_t>(faultMap[taskId].faultType)
                               << ",dropRate:" << faultMap[taskId].dropRate << ",delay:" << faultMap[taskId].delay
                               << ",lowerDataRate nodeId:" << lowerDataRate.nodeId << ",lowerDataRate portId:"
                               << lowerDataRate.portId << ",lowerDataRate dataRate:" << lowerDataRate.dataRate
                               << ",shutDownPacketDrop begin:" << faultMap[taskId].shutDownPacketDrop.begin
                               << ",shutDownPacketDrop end:" << faultMap[taskId].shutDownPacketDrop.end
                               << ",erorDropRate:" << faultMap[taskId].erorDropRate);
    }
    file.close();
}
// compute packetSize
uint32_t UbFault::GetPacketSize(Ptr<Packet> packet)
{
    UbMAExtTah MAExtTaHeader;
    UbTransactionHeader TransactionHeader;
    UbTransportHeader TransportHeader;
    UdpHeader UHeader;
    Ipv4Header I4Header;
    UbDatalinkPacketHeader DataLinkPacketHeader;
    UbNetworkHeader networkHeader;
    uint32_t MAExtTaHeaderSize = MAExtTaHeader.GetSerializedSize();
    uint32_t UbTransactionHeaderSize = TransactionHeader.GetSerializedSize();
    uint32_t UbTransportHeaderSize = TransportHeader.GetSerializedSize();
    uint32_t UdpHeaderSize = UHeader.GetSerializedSize();
    uint32_t Ipv4HeaderSize = I4Header.GetSerializedSize();
    uint32_t UbDataLinkPktSize = DataLinkPacketHeader.GetSerializedSize();
    uint32_t networkHeaderSize = networkHeader.GetSerializedSize();
    uint32_t headerSize = MAExtTaHeaderSize + UbTransactionHeaderSize + UbTransportHeaderSize + UdpHeaderSize +
                          Ipv4HeaderSize + UbDataLinkPktSize + networkHeaderSize;

    // cout<<"packetsize:"<<packet->GetSize()<<","<<headerSize<<endl;
    if (packet->GetSize() < headerSize)
        return 0;
    return packet->GetSize() - headerSize;
}

// Flow Following对第一跳交换机首个包设置时延 &&Packet Following 对第一跳交换机首个端口设置时延
int UbFault::SetPacketDelay(uint32_t taskId, uint32_t nodeId, uint32_t portId)
{
    if (!MapTaskFind(delayMap, taskId)) {
        delayMap[taskId] = {nodeId, portId};
        NS_LOG_DEBUG("nodeId" << nodeId << ",taskId:" << taskId << ",delay(ns):" << faultMap[taskId].delay);
        return faultMap[taskId].delay;
    } else if (isPacketFlowValue && delayMap[taskId].nodeId == nodeId && delayMap[taskId].portId == portId) {  // 逐包
        NS_LOG_DEBUG("nodeId" << nodeId << ",taskId:" << taskId << ",portId" << portId
                              << ",delay(ns):" << faultMap[taskId].delay);
        return faultMap[taskId].delay;
    }
    return 0;
}

// 按照丢包率丢包
int UbFault::SetPacketDrop(uint64_t packetSize, double dropRate, uint32_t taskId, uint32_t nodeId, uint32_t portId)
{
    int ret = 0;
    if (!MapTaskFind(dropMap, taskId)) {
        dropMap[taskId] = {nodeId, portId, packetSize, 0};
        ret = 0;
    } else if (dropMap[taskId].nodeId == nodeId && dropMap[taskId].portId == portId) {
        double curPacketDroprate =
            1.0 * (dropMap[taskId].dropSize) / (dropMap[taskId].dropSize + dropMap[taskId].sendSize);
        if (curPacketDroprate < dropRate) {
            dropMap[taskId].dropSize += packetSize;
            ret = -1;
        } else {
            dropMap[taskId].sendSize += packetSize;
            ret = 0;
        }
        curPacketDroprate = 1.0 * (dropMap[taskId].dropSize) / (dropMap[taskId].dropSize + dropMap[taskId].sendSize);
        if (isPacketFlowValue) {
            NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",portId:" << portId << ",dropsize:"
                                   << dropMap[taskId].dropSize << ",sendSize:" << dropMap[taskId].sendSize
                                   << ",curPacketDroprate:" << curPacketDroprate);
        } else {
            NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",dropsize:" << dropMap[taskId].dropSize
                                   << ",sendSize:" << dropMap[taskId].sendSize
                                   << ",curPacketDroprate:" << curPacketDroprate);
        }
    }

    return ret;
}

// 错包实现 随机丢包
int UbFault::GetRandomToDeterminePacketDrop(uint64_t packetSize, double lossProbability, uint32_t taskId,
                                            uint32_t nodeId, uint32_t portId)
{
    Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
    uv->SetAttribute("Min", DoubleValue(0.0));
    uv->SetAttribute("Max", DoubleValue(1.0));
    int ret = 0;
    if (uv->GetValue() < lossProbability) {
        errorDropMap[taskId].dropSize += packetSize;
        ret = -1;
    } else {
        errorDropMap[taskId].sendSize += packetSize;
    }
    double curPacketDroprate =
        1.0 * (errorDropMap[taskId].dropSize) / (errorDropMap[taskId].dropSize + errorDropMap[taskId].sendSize);
    if (isPacketFlowValue) {
        NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",portId" << portId << ",dropsize:"
                               << errorDropMap[taskId].dropSize << ",sendSize:" << errorDropMap[taskId].sendSize
                               << ",curPacketDroprate:" << curPacketDroprate);
    } else {
        NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",dropsize:" << errorDropMap[taskId].dropSize
                               << ",sendSize:" << errorDropMap[taskId].sendSize
                               << ",curPacketDroprate:" << curPacketDroprate);
    }

    return ret;
}

int UbFault::SetErrorPacket(uint64_t packetSize, double lossProbability, uint32_t taskId, uint32_t nodeId,
                            uint32_t portId)
{
    int ret = 0;
    if (!MapTaskFind(errorDropMap, taskId)) {
        errorDropMap[taskId] = {nodeId, portId, 0, 0};
        ret = GetRandomToDeterminePacketDrop(packetSize, lossProbability, taskId, nodeId, portId);
    } else if (errorDropMap[taskId].nodeId == nodeId && errorDropMap[taskId].portId == portId) {
        ret = GetRandomToDeterminePacketDrop(packetSize, lossProbability, taskId, nodeId, portId);
    }
    return ret;
}

void UbFault::SetPortCongestion(LowerDataRate lowerDataRate)
{
    Ptr<Node> node = NodeList::GetNode(lowerDataRate.nodeId);
    Ptr<UbPort> port = DynamicCast<ns3::UbPort>(node->GetDevice(lowerDataRate.portId));
    port->SetDataRate(lowerDataRate.dataRate);
}
int UbFault::JudgeShutdownRangeToDeterminePacketDrop(uint64_t packetSize, uint32_t taskId, uint32_t nodeId,
                                                     uint32_t portId)
{
    int ret = 0;
    shutdownDropMap[taskId].sendNum += 1;
    if ((shutdownDropMap[taskId].sendNum >= faultMap[taskId].shutDownPacketDrop.begin) &&
        (shutdownDropMap[taskId].sendNum <= faultMap[taskId].shutDownPacketDrop.end)) {
        shutdownDropMap[taskId].dropSize += packetSize;
        ret = -1;
    } else {
        shutdownDropMap[taskId].sendSize += packetSize;
        ret = 0;
    }
    double shutdownPacketDroprate = 1.0 * (shutdownDropMap[taskId].dropSize) /
                                    (shutdownDropMap[taskId].dropSize + shutdownDropMap[taskId].sendSize);

    if (isPacketFlowValue) {
        NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",portId:" << portId << ",dropsize:"
                               << shutdownDropMap[taskId].dropSize << ",sendSize:" << shutdownDropMap[taskId].sendSize
                               << ",shutdownPacketDroprate:" << shutdownPacketDroprate
                               << ",shutdownDropMap[taskId].sendNum:" << shutdownDropMap[taskId].sendNum);
    } else {
        NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",dropsize:" << shutdownDropMap[taskId].dropSize
                               << ",sendSize:" << shutdownDropMap[taskId].sendSize
                               << ",shutdownPacketDroprate:" << shutdownPacketDroprate
                               << ",shutdownDropMap[taskId].sendNum:" << shutdownDropMap[taskId].sendNum);
    }

    return ret;
}
int UbFault::SetPortShutdownAndUp(uint64_t packetSize, uint32_t taskId, uint32_t nodeId, uint32_t portId)
{
    int ret = 0;
    if (!MapTaskFind(shutdownDropMap, taskId)) {
        shutdownDropMap[taskId] = {nodeId, portId, 0, 0, 0};
        ret = JudgeShutdownRangeToDeterminePacketDrop(packetSize, taskId, nodeId, portId);
    } else if (shutdownDropMap[taskId].nodeId == nodeId && shutdownDropMap[taskId].portId == portId) {
        ret = JudgeShutdownRangeToDeterminePacketDrop(packetSize, taskId, nodeId, portId);
    }

    return ret;
}
int UbFault::FaultDiagnosis(Ptr<Packet> packet, uint32_t nodeId, uint32_t portId, Ptr<UbPort> ubPort)
{
    uint64_t packetSize = GetPacketSize(packet);
    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<UbSwitch> retrieved_sw = node->GetObject<UbSwitch>();
    UbFlowTag flowTag;
    packet->PeekPacketTag(flowTag);
    uint32_t taskId = flowTag.GetFlowId();
    int ret = 0;
    if (retrieved_sw->GetNodeType() == UB_SWITCH && MapTaskFind(faultMap, taskId) && packetSize > 0) {
        switch (faultMap[taskId].faultType) {
            case FaultType::DROPPACKET:
                ret = SetPacketDrop(packetSize, faultMap[taskId].dropRate, taskId, nodeId, portId);
                break;
            case FaultType::ADDPACKETDELAY:
                ret = SetPacketDelay(taskId, nodeId, portId);
                break;
            case FaultType::SHUTDOWNUP:
                ret = SetPortShutdownAndUp(packetSize, taskId, nodeId, portId);
                break;
            case FaultType::ERRORPACKET:
                ret = SetErrorPacket(packetSize, faultMap[taskId].erorDropRate, taskId, nodeId, portId);
                break;
            default:
                break;
        }
    }
    return ret;
}

int UbFault::FaultCallback(Ptr<Packet> packet, uint32_t nodeId, uint32_t portId, Ptr<UbPort> ubPort)
{
    int ret = FaultDiagnosis(packet, nodeId, portId, ubPort);
    if (ret >= 0) {
        Simulator::ScheduleNow(&UbPort::TransmitPacket, ubPort, packet, Time(ret));
    } else {
        Simulator::Schedule(Time(0), &UbPort::TransmitComplete, ubPort);
    }
    return 0;
}
}  // namespace ns3
