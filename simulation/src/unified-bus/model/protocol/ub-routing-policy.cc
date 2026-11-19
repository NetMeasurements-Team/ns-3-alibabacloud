// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-routing-policy.h"

#include "ns3/hash.h"
#include "ns3/node-list.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-switch.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ns3 {

namespace
{
size_t
CountPorts(UbSpan<const uint16_t> ports, uint16_t excludedPort)
{
    size_t count = 0;
    for (uint16_t port : ports)
    {
        count += port == excludedPort ? 0 : 1;
    }
    return count;
}
} // namespace

size_t
UbRoutingPolicy::Count(const UbRoutingCandidateView& candidates)
{
    return CountPorts(candidates.shortest, candidates.excludedPort) +
           CountPorts(candidates.nonShortest, candidates.excludedPort);
}

uint64_t
UbRoutingPolicy::CalcHash(uint32_t sip,
                          uint32_t dip,
                          uint16_t sport,
                          uint16_t dport,
                          uint8_t priority,
                          uint32_t salt)
{
    uint8_t buf[17];
    buf[0] = (sip >> 24) & 0xff;
    buf[1] = (sip >> 16) & 0xff;
    buf[2] = (sip >> 8) & 0xff;
    buf[3] = sip & 0xff;
    buf[4] = (dip >> 24) & 0xff;
    buf[5] = (dip >> 16) & 0xff;
    buf[6] = (dip >> 8) & 0xff;
    buf[7] = dip & 0xff;
    buf[8] = (sport >> 8) & 0xff;
    buf[9] = sport & 0xff;
    buf[10] = (dport >> 8) & 0xff;
    buf[11] = dport & 0xff;
    buf[12] = priority;
    buf[13] = (salt >> 24) & 0xff;
    buf[14] = (salt >> 16) & 0xff;
    buf[15] = (salt >> 8) & 0xff;
    buf[16] = salt & 0xff;
    return Hash64(reinterpret_cast<const char*>(buf), sizeof(buf));
}

int
UbRoutingPolicy::CandidateAt(const UbRoutingCandidateView& candidates,
                             size_t index,
                             bool& selectedShortestPath)
{
    for (uint16_t port : candidates.shortest)
    {
        if (port == candidates.excludedPort)
        {
            continue;
        }
        if (index-- == 0)
        {
            selectedShortestPath = true;
            return port;
        }
    }
    for (uint16_t port : candidates.nonShortest)
    {
        if (port == candidates.excludedPort)
        {
            continue;
        }
        if (index-- == 0)
        {
            selectedShortestPath = false;
            return port;
        }
    }
    return -1;
}

int
UbRoutingPolicy::SelectHash(const RoutingKey& key,
                            const UbRoutingCandidateView& candidates,
                            uint32_t nodeId,
                            bool& selectedShortestPath)
{
    const size_t candidateCount = Count(candidates);
    if (candidateCount == 0)
    {
        return -1;
    }

    const uint32_t salt = utils::NodeIdToIp(nodeId).Get();
    const uint64_t selection =
        CalcHash(key.sip, key.dip, key.sport, key.dport, key.priority, salt);

    return CandidateAt(candidates,
                       static_cast<size_t>(selection % candidateCount),
                       selectedShortestPath);
}

int
UbRoutingPolicy::SelectAdaptive(const RoutingKey& key,
                                const UbRoutingCandidateView& candidates,
                                uint32_t nodeId,
                                bool& selectedShortestPath)
{
    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
    Ptr<UbQueueManager> queueManager = sw->GetQueueManager();

    auto loadScore = [&](uint16_t outPort) -> uint64_t {
        if (queueManager == nullptr)
        {
            return 0;
        }
        const uint64_t voqLoad =
            queueManager->GetOutPortBufferUsed(outPort, static_cast<uint32_t>(key.priority));
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(outPort));
        return voqLoad + port->GetUbQueue()->GetCurrentBytes();
    };

    uint64_t bestScore = std::numeric_limits<uint64_t>::max();
    std::vector<std::pair<uint16_t, bool>> bestCandidates;
    auto consider = [&](UbSpan<const uint16_t> ports, bool isShortest) {
        for (uint16_t port : ports)
        {
            if (port == candidates.excludedPort)
            {
                continue;
            }
            const uint64_t score = loadScore(port);
            if (score < bestScore)
            {
                bestScore = score;
                bestCandidates.clear();
                bestCandidates.emplace_back(port, isShortest);
            }
            else if (score == bestScore)
            {
                bestCandidates.emplace_back(port, isShortest);
            }
        }
    };

    consider(candidates.shortest, true);
    consider(candidates.nonShortest, false);
    if (bestCandidates.empty())
    {
        return -1;
    }

    const uint32_t salt = utils::NodeIdToIp(nodeId).Get();
    const uint64_t selection =
        CalcHash(key.sip, key.dip, key.sport, key.dport, key.priority, salt);
    const auto [selectedPort, isShortest] =
        bestCandidates[static_cast<size_t>(selection % bestCandidates.size())];
    selectedShortestPath = isShortest;
    return selectedPort;
}

} // namespace ns3
