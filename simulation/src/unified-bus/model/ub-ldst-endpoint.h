// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LDST_ENDPOINT_H
#define UB_LDST_ENDPOINT_H

#include "ns3/callback.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/ub-datatype.h"

#include <cstdint>
#include <unordered_map>

namespace ns3
{

class Node;

using UbLdstCompletionCallback = Callback<void, uint16_t, bool, Ptr<Packet>>;

struct UbLdstRequest
{
    uint16_t requestId{0};
    uint32_t destNode{0};
    uint16_t port{0};
    UbMemOperationType type{UbMemOperationType::STORE};
    uint32_t requestBytes{0};
    uint32_t responseBytes{0};
    UbPriority priority{UB_PRIORITY_DEFAULT};
    bool hasPaddr{false};
    uint64_t paddr{0};
    Ptr<Packet> payload;
    UbLdstCompletionCallback completion;
};

class UbLdstEndpoint : public Object
{
  public:
    struct ResponseContext
    {
        uint32_t srcNode{0};
        uint32_t destNode{0};
    };

    static TypeId GetTypeId();

    UbLdstEndpoint();
    ~UbLdstEndpoint() override;

    void Init(uint32_t nodeId);
    bool SendRequest(const UbLdstRequest& request);
    bool CompleteRequest(uint16_t requestId,
                         const ResponseContext& responseContext,
                         UbTransactionOpcode responseOpcode,
                         Ptr<Packet> payload);

  private:
    struct InflightRequest
    {
        UbMemOperationType type{UbMemOperationType::STORE};
        uint32_t expectedResponseBytes{0};
        ResponseContext responseContext;
        UbLdstCompletionCallback completion;
    };

    bool ValidateRequest(const UbLdstRequest& request, uint8_t& encodedLength) const;
    bool IsEncodableMemorySize(uint32_t bytes, uint8_t& encodedLength) const;
    bool BuildAndSendRequestPacket(const UbLdstRequest& request, uint8_t encodedLength);
    Ptr<Node> GetLocalNode() const;

    UbDatalinkHeaderConfig m_headerConfig{UbDatalinkHeaderConfig::PACKET_UB_MEM};
    uint32_t m_nodeId{0};
    bool m_initialized{false};
    std::unordered_map<uint16_t, InflightRequest> m_inflight;
};

} // namespace ns3

#endif /* UB_LDST_ENDPOINT_H */
