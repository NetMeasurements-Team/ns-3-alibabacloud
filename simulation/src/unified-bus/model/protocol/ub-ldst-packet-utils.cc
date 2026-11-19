// SPDX-License-Identifier: GPL-2.0-only
#include "ub-ldst-packet-utils.h"
#include "ns3/ub-enum-accessor.h"

#include "ns3/ub-network-address.h"

namespace ns3
{
Ptr<const AttributeChecker>
MakeLdstHeaderConfigChecker()
{
    return MakeUbEnumChecker(UbDatalinkHeaderConfig::PACKET_UB_MEM,
                           "9",
                           UbDatalinkHeaderConfig::PACKET_CNA24,
                           "7");
}

bool
IsLdstAddressEncodable(uint8_t config, const UbLdstAddress& address)
{
    if (config != 7 && config != 9)
    {
        return false;
    }
    const uint32_t maxNode = config == 7 ? 65535 : 4095;
    const uint32_t maxPort = config == 7 ? 254 : 14;
    return address.nodeId <= maxNode && (!address.hasPort || address.portId <= maxPort);
}

bool
TryEncodeLdstAddress(uint8_t config, const UbLdstAddress& address, uint32_t& cna)
{
    if (!IsLdstAddressEncodable(config, address))
    {
        return false;
    }
    if (config == 7)
    {
        cna = address.hasPort ? utils::NodeIdToCna24(address.nodeId, address.portId)
                              : utils::NodeIdToCna24(address.nodeId);
    }
    else
    {
        cna = address.hasPort ? utils::NodeIdToCna16(address.nodeId, address.portId)
                              : utils::NodeIdToCna16(address.nodeId);
    }
    return true;
}

UbLdstAddress
DecodeLdstAddress(uint8_t config, uint32_t cna)
{
    UbLdstAddress address;
    // CFG6's historical LDST receive fallback uses CNA16 too.
    if (config == 7)
    {
        address.nodeId = utils::Cna24ToNodeId(cna);
        address.hasPort = (cna & 0xFF) != 0;
        address.portId = address.hasPort ? utils::Cna24ToPortId(cna) : 0;
    }
    else
    {
        address.nodeId = utils::Cna16ToNodeId(cna);
        address.hasPort = (cna & 0xF) != 0;
        address.portId = address.hasPort ? utils::Cna16ToPortId(cna) : 0;
    }
    return address;
}

namespace
{
template <typename Header>
void
AddNetworkHeader(Ptr<Packet> packet,
                 const UbLdstNetworkFields& fields,
                 uint32_t scna,
                 uint32_t dcna,
                 uint8_t nlp)
{
    Header header;
    header.SetScna(scna);
    header.SetDcna(dcna);
    header.SetLb(fields.lb);
    header.SetServiceLevel(fields.serviceLevel);
    header.SetNlp(nlp);
    packet->AddHeader(header);
}

template <typename Header>
UbLdstNetworkFields
RemoveNetworkHeader(Ptr<Packet> packet, uint8_t config)
{
    Header header;
    packet->RemoveHeader(header);
    UbLdstNetworkFields fields;
    fields.source = DecodeLdstAddress(config, header.GetScna());
    fields.destination = DecodeLdstAddress(config, header.GetDcna());
    fields.lb = header.GetLb();
    fields.serviceLevel = header.GetServiceLevel();
    return fields;
}
} // namespace

void
AddLdstNetworkHeader(Ptr<Packet> packet,
                     const UbLdstNetworkFields& fields,
                     UbDatalinkHeaderConfig config)
{
    uint32_t scna = 0;
    uint32_t dcna = 0;
    NS_ABORT_MSG_IF(
        !TryEncodeLdstAddress(static_cast<uint8_t>(config), fields.source, scna) ||
            !TryEncodeLdstAddress(static_cast<uint8_t>(config), fields.destination, dcna),
        "LDST address cannot be encoded for HeaderConfig " << unsigned(config));
    if (config == UbDatalinkHeaderConfig::PACKET_CNA24)
    {
        AddNetworkHeader<UbCna24NetworkHeader>(packet, fields, scna, dcna, UB_CNA_NLP_LDST_BYPASS);
    }
    else
    {
        AddNetworkHeader<UbCna16NetworkHeader>(packet, fields, scna, dcna, 0);
    }
}

UbLdstNetworkFields
RemoveLdstNetworkHeader(Ptr<Packet> packet, uint8_t config)
{
    return config == 7 ? RemoveNetworkHeader<UbCna24NetworkHeader>(packet, config)
                       : RemoveNetworkHeader<UbCna16NetworkHeader>(packet, config);
}
} // namespace ns3
