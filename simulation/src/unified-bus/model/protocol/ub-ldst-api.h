// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LDST_API_H
#define UB_LDST_API_H

#include "ns3/node-list.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-header.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-tag.h"
#include <ns3/node.h>
#include <ns3/ub-ldst-thread.h>

#include <limits>
#include <memory>

namespace ns3
{
constexpr int MAX_LB = 255;
constexpr int MIN_LB = 0;
struct RoutingKey;
class UbController;

struct UbLdstRemoteRequestContext
{
    UbDatalinkHeaderConfig headerConfig{UbDatalinkHeaderConfig::PACKET_UB_MEM};
    uint16_t requestId{0};
    uint32_t srcNode{0};
    uint32_t destNode{0};
    uint32_t srcPortId{0};
    uint32_t dstPortId{0};
    bool hasSrcPortId{false};
    bool hasDstPortId{false};
    UbMemOperationType type{UbMemOperationType::STORE};
    uint32_t requestBytes{0};
    uint32_t responseBytes{0};
    UbPriority priority{UB_PRIORITY_DEFAULT};
    bool hasPaddr{false};
    uint64_t paddr{0};
    uint32_t ingressPort{std::numeric_limits<uint32_t>::max()};
    uint8_t creditTargetVl{0};
    uint8_t packetVl{0};
    bool usePacketSpray{false};
    bool useShortestPaths{true};
    uint8_t lb{0};
    uint32_t packetUid{0};
    uint32_t packetSize{0};
    Ptr<Packet> payload;
    uint32_t flowId{0};
    UbPacketTraceTag traceTag;
};

using UbLdstResponseSender = Callback<void, UbTransactionOpcode, uint32_t, Ptr<Packet>>;
using UbLdstRemoteRequestProcessor =
    Callback<bool, const UbLdstRemoteRequestContext&, UbLdstResponseSender>;

class UbLdstApi : public Object
{
  public:
    static TypeId GetTypeId(void);
    UbLdstApi();
    virtual ~UbLdstApi();

  public:
    void SetNodeId(uint32_t nodeId);
    void RecvResponse(Ptr<Packet> packet);
    void SetRoutingType(RoutingType routingType);
    void ValidateRoutingConfiguration() const;
    void RecvDataPacket(Ptr<Packet> packet,
                        uint32_t ingressPort = std::numeric_limits<uint32_t>::max());
    void LdstProcess(Ptr<UbLdstTaskSegment> taskSegment);
    void SetRemoteRequestProcessor(UbLdstRemoteRequestProcessor processor);
    void ClearRemoteRequestProcessor();
    bool HasRemoteRequestProcessor() const;

  private:
    void ValidateRoutingType(RoutingType routingType) const;
    void SendPacket(Ptr<UbLdstTaskSegment> taskSegment, Ptr<Packet> packet);
    int SelectOutPort(RoutingKey routingKey,
                      bool hasSourcePort,
                      uint32_t sourcePort,
                      bool hasDestinationPort) const;
    Ptr<Packet> GenDataPacket(Ptr<UbLdstTaskSegment> taskSegment);
    bool BuildRemoteRequestContext(Ptr<Packet> packet,
                                   uint32_t ingressPort,
                                   UbDatalinkPacketHeader& linkPacketHeader,
                                   UbCompactTransactionHeader& cTaHeader,
                                   UbLdstRemoteRequestContext& context);
    void SendRemoteResponse(const UbLdstRemoteRequestContext& context,
                            UbTransactionOpcode responseOpcode,
                            uint32_t payloadBytes,
                            Ptr<Packet> payload);
    static void SendBoundRemoteResponse(Ptr<UbLdstApi> api,
                                        std::shared_ptr<const UbLdstRemoteRequestContext> context,
                                        UbTransactionOpcode responseOpcode,
                                        uint32_t payloadBytes,
                                        Ptr<Packet> payload);
    UbDatalinkHeaderConfig m_headerConfig{UbDatalinkHeaderConfig::PACKET_UB_MEM};
    uint32_t m_nodeId{UINT32_MAX};
    uint32_t m_lbHashSalt = 0;
    RoutingType m_routingType{RoutingType::PER_FLOW_SHORTEST_PATHS};
    bool m_pktTraceEnabled = false;
    UbLdstRemoteRequestProcessor m_remoteRequestProcessor;
    void LdstRecvNotify(uint32_t packetUid,
                        uint32_t src,
                        uint32_t dst,
                        PacketType type,
                        uint32_t size,
                        uint32_t taskId,
                        UbPacketTraceTag traceTag);
    TracedCallback<uint32_t, uint32_t, uint32_t, PacketType, uint32_t, uint32_t, UbPacketTraceTag>
        m_ldstRecvNotify;
};
} // namespace ns3

#endif /* UB_LDST_API_H */
