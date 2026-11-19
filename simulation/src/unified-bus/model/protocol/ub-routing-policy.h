// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_ROUTING_POLICY_H
#define UB_ROUTING_POLICY_H

#include <cstdint>
#include "ns3/ub-span.h"

namespace ns3 {

struct RoutingKey;

struct UbRoutingCandidateView
{
    UbSpan<const uint16_t> shortest;
    UbSpan<const uint16_t> nonShortest;
    uint16_t excludedPort{UINT16_MAX};
};

class UbRoutingPolicy
{
  public:
    static size_t Count(const UbRoutingCandidateView& candidates);
    static int SelectHash(const RoutingKey& key,
                          const UbRoutingCandidateView& candidates,
                          uint32_t nodeId,
                          bool& selectedShortestPath);
    static int SelectAdaptive(const RoutingKey& key,
                              const UbRoutingCandidateView& candidates,
                              uint32_t nodeId,
                              bool& selectedShortestPath);

  private:
    static uint64_t CalcHash(uint32_t sip,
                             uint32_t dip,
                             uint16_t sport,
                             uint16_t dport,
                             uint8_t priority,
                             uint32_t salt);
    static int CandidateAt(const UbRoutingCandidateView& candidates,
                           size_t index,
                           bool& selectedShortestPath);
};

} // namespace ns3

#endif // UB_ROUTING_POLICY_H
