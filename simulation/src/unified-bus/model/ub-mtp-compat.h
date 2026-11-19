// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_MTP_COMPAT_H
#define UB_MTP_COMPAT_H

#ifdef NS3_MTP

#include "ns3/channel.h"
#include "ns3/mtp-interface.h"
#include "ns3/net-device.h"
#include "ns3/node-container.h"
#include "ns3/node-list.h"
#include "ns3/node.h"
#include "ns3/nstime.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <vector>

namespace ns3 {

/**
 * The upstream ns-3-ub MTP exposes FindNodeLocalLpId() and
 * GetCausalDeliveryDelay(). The ns-3.36 MTP fork used by SimAI does not, so
 * the same semantics are implemented here from public ns-3 state. Keeping this
 * in the unified-bus module avoids diverging src/mtp from the master branch.
 */
inline std::optional<uint32_t>
UbFindNodeLocalLpId(uint32_t nodeId)
{
    Ptr<Node> node = NodeList::GetNode(nodeId);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const uint32_t systemId = node->GetSystemId();
    if (systemId >= MtpInterface::GetSize())
    {
        return std::nullopt;
    }
    return systemId;
}

/**
 * Per-LP lookahead, mirroring LogicalProcess::CalculateLookAhead().
 *
 * The topology is static for the lifetime of a SimAI run, so the table is
 * computed once (matching MTP's own one-shot calculation in RunBefore()).
 */
inline const std::vector<Time>&
UbLookAheadTable()
{
    static std::mutex mutex;
    static std::vector<Time> table;
    static bool initialized = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (initialized)
    {
        return table;
    }

    const uint32_t size = MtpInterface::GetSize();
    table.assign(size, Time::Max() / 2 - TimeStep(1));
    if (size > 0)
    {
        table[0] = TimeStep(0);
    }

    NodeContainer nodes = NodeContainer::GetGlobal();
    for (auto iter = nodes.Begin(); iter != nodes.End(); ++iter)
    {
#ifdef NS3_MPI
        const uint32_t localLp = (*iter)->GetSystemId() >> 16;
#else
        const uint32_t localLp = (*iter)->GetSystemId();
#endif
        if (localLp >= table.size())
        {
            continue;
        }
        for (uint32_t i = 0; i < (*iter)->GetNDevices(); ++i)
        {
            Ptr<NetDevice> localDevice = (*iter)->GetDevice(i);
            if (!localDevice->IsPointToPoint())
            {
                continue;
            }
            Ptr<Channel> channel = localDevice->GetChannel();
            if (channel == nullptr)
            {
                continue;
            }
            Ptr<Node> remoteNode;
            if (channel->GetDevice(0) == localDevice)
            {
                remoteNode = channel->GetDevice(1)->GetNode();
            }
            else
            {
                remoteNode = channel->GetDevice(0)->GetNode();
            }
            if (remoteNode == nullptr)
            {
                continue;
            }
#ifdef NS3_MPI
            const uint32_t remoteLp = remoteNode->GetSystemId() >> 16;
#else
            const uint32_t remoteLp = remoteNode->GetSystemId();
#endif
            if (remoteLp == localLp)
            {
                continue;
            }
            TimeValue delay;
            channel->GetAttribute("Delay", delay);
            if (delay.Get() < table[localLp])
            {
                table[localLp] = delay.Get();
            }
        }
    }

    initialized = true;
    return table;
}

inline Time
UbGetCausalDeliveryDelay(uint32_t targetNodeId)
{
    const std::optional<uint32_t> targetLpId = UbFindNodeLocalLpId(targetNodeId);
    NS_ABORT_MSG_IF(!targetLpId.has_value(),
                    "MTP causal delivery could not find the worker partition for target node "
                        << targetNodeId);

    const std::vector<Time>& lookAheads = UbLookAheadTable();
    NS_ABORT_MSG_IF(*targetLpId >= lookAheads.size(),
                    "MTP causal delivery has no lookahead for worker partition " << *targetLpId);
    const Time lookAhead = lookAheads[*targetLpId];
    const Time unconstrainedLookAhead = Time::Max() / 2 - TimeStep(1);
    NS_ABORT_MSG_IF(!lookAhead.IsStrictlyPositive() || lookAhead >= unconstrainedLookAhead,
                    "MTP causal delivery requires a finite positive lookahead for target node "
                        << targetNodeId << " on worker partition " << *targetLpId
                        << "; bind a positive lookahead or register a causal topology edge before "
                           "Run()");
    return lookAhead;
}

} // namespace ns3

#endif // NS3_MTP

#endif // UB_MTP_COMPAT_H
