// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_DATALINK_H
#define UB_DATALINK_H

#include <ns3/node.h>
#include <set>
#include <unordered_map>
#include "ub-header.h"
#include "ns3/ub-port.h"

namespace ns3 {
/**
 * \brief UB Data Link Layer
 */
class UbDataLink : public Object {
public:
    static TypeId GetTypeId(void);

    UbDataLink();
    virtual ~UbDataLink();

    static UbDatalinkControlCreditHeader ConsumeCreditHeaderAndRestoreCredits(Ptr<Packet> packet,
                                                                              Ptr<UbPort> port);
    static UbDatalinkPacketHeader RemovePacketHeader(Ptr<Packet> packet);
    static Ptr<Packet> BuildControlCreditPacket(const uint8_t credits[16]);
    static void AddPacketHeader(Ptr<Packet> packet,
                                bool credit,
                                bool ack,
                                uint8_t creditTargetVl,
                                uint8_t packetVl,
                                RoutingType routingType,
                                UbDatalinkHeaderConfig config);

    [[deprecated("use ConsumeCreditHeaderAndRestoreCredits")]] static UbDatalinkControlCreditHeader
    ParseCreditHeader(Ptr<Packet> packet, Ptr<UbPort> port)
    {
        return ConsumeCreditHeaderAndRestoreCredits(packet, port);
    }

    [[deprecated("use RemovePacketHeader")]] static UbDatalinkPacketHeader ParsePacketHeader(
        Ptr<Packet> packet)
    {
        return RemovePacketHeader(packet);
    }

    [[deprecated("use BuildControlCreditPacket")]] static Ptr<Packet> GenControlCreditPacket(
        const uint8_t credits[16])
    {
        return BuildControlCreditPacket(credits);
    }

    [[deprecated("use AddPacketHeader")]] static void GenPacketHeader(Ptr<Packet> packet,
                                                                      bool credit,
                                                                      bool ack,
                                                                      uint8_t creditTargetVl,
                                                                      uint8_t packetVl,
                                                                      RoutingType routingType,
                                                                      UbDatalinkHeaderConfig config)
    {
        AddPacketHeader(packet, credit, ack, creditTargetVl, packetVl, routingType, config);
    }
};

} // namespace ns3

#endif /* UB_DATALINK_H */
