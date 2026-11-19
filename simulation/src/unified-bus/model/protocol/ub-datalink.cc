// SPDX-License-Identifier: GPL-2.0-only
#include "ub-datalink.h"

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("UbDataLink");
NS_OBJECT_ENSURE_REGISTERED(UbDataLink);

UbDataLink::UbDataLink() {}

UbDataLink::~UbDataLink() {}

TypeId UbDataLink::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbDataLink")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbDataLink>();
    return tid;
}

UbDatalinkControlCreditHeader
UbDataLink::ConsumeCreditHeaderAndRestoreCredits(Ptr<Packet> packet, Ptr<UbPort> port)
{
    UbDatalinkControlCreditHeader header;
    packet->PeekHeader(header);
    uint8_t credits[16];
    header.GetAllCreditsVL(credits);
    port->ResetCredits();
    for (int i = 0; i < 16; i++) {
        port->SetCredits(i, credits[i]);
    }
    packet->RemoveHeader(header);
    return header;
}

UbDatalinkPacketHeader
UbDataLink::RemovePacketHeader(Ptr<Packet> packet)
{
    UbDatalinkPacketHeader header;
    packet->RemoveHeader(header);
    return header;
}

Ptr<Packet>
UbDataLink::BuildControlCreditPacket(const uint8_t credits[16])
{
    Ptr<Packet> p = Create<Packet>(0);
    UbDatalinkControlCreditHeader controlCreditHeader;
    controlCreditHeader.SetAllCreditsVL(credits);
    controlCreditHeader.SetSD(1);
    controlCreditHeader.SetType(1);
    p->AddHeader(controlCreditHeader);
    return p;
}

void
UbDataLink::AddPacketHeader(Ptr<Packet> packet,
                            bool credit,
                            bool ack,
                            uint8_t creditTargetVl,
                            uint8_t packetVl,
                            RoutingType routingType,
                            UbDatalinkHeaderConfig config)
{
    NS_ABORT_MSG_IF(config != UbDatalinkHeaderConfig::CONTROL && packetVl == 0,
                    "Unified-bus reserves priority 0 for locally generated control frames in the "
                    "simulator model. Data packets must use priority 1..15.");

    UbDatalinkPacketHeader linkPacketHeader;
    linkPacketHeader.SetCredit(credit);             // 报文是否返回信用证
    linkPacketHeader.SetACK(ack);                   // 报文是否释放retry buffer空间
    linkPacketHeader.SetCreditTargetVL(creditTargetVl); // 4 bits: 指定接收credit的VL
    linkPacketHeader.SetPacketVL(packetVl);             // 4 bits: 数据包的VL
    linkPacketHeader.SetRoutingType(routingType);
    linkPacketHeader.SetConfig(static_cast<uint8_t>(config));
    packet->AddHeader(linkPacketHeader);
}

} // namespace ns3
