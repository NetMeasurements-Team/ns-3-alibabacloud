// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LDST_PACKET_UTILS_H
#define UB_LDST_PACKET_UTILS_H

#include "ns3/ub-header.h"

namespace ns3
{
struct UbLdstAddress
{
    uint32_t nodeId{0};
    uint32_t portId{0};
    bool hasPort{false};
};

// Decoded fields for routing and response context, not a replacement wire header.
struct UbLdstNetworkFields
{
    UbLdstAddress source;
    UbLdstAddress destination;
    uint8_t lb{0};
    uint8_t serviceLevel{0};
};

Ptr<const AttributeChecker> MakeLdstHeaderConfigChecker();

bool IsLdstAddressEncodable(uint8_t config, const UbLdstAddress& address);
bool TryEncodeLdstAddress(uint8_t config, const UbLdstAddress& address, uint32_t& cna);
UbLdstAddress DecodeLdstAddress(uint8_t config, uint32_t cna);

void AddLdstNetworkHeader(Ptr<Packet> packet,
                          const UbLdstNetworkFields& fields,
                          UbDatalinkHeaderConfig config = UbDatalinkHeaderConfig::PACKET_UB_MEM);
UbLdstNetworkFields RemoveLdstNetworkHeader(Ptr<Packet> packet, uint8_t config = 9);
} // namespace ns3
#endif
