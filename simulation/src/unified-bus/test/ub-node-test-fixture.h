// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_NODE_TEST_FIXTURE_H
#define UB_NODE_TEST_FIXTURE_H
#include "ns3/ub-controller.h"
#include "ns3/ub-congestion-control.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-network-address.h"
namespace
{
using namespace ns3;
inline Ptr<UbPort>
CreatePort(Ptr<Node> node)
{
    Ptr<UbPort> port = CreateObject<UbPort>();
    port->SetAddress(Mac48Address::Allocate());
    node->AddDevice(port);
    return port;
}

inline void
InitNode(Ptr<Node> node,
         UbNodeType_t nodeType,
         uint32_t portCount,
         bool provisionDefaultCtpEntity = true)
{
    Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
    node->AggregateObject(sw);
    sw->SetNodeType(nodeType);

    if (nodeType == UB_DEVICE)
    {
        Ptr<UbController> controller = CreateObject<UbController>();
        node->AggregateObject(controller);
        controller->CreateUbFunction();
        controller->CreateUbTransaction();
    }

    for (uint32_t index = 0; index < portCount; ++index)
    {
        CreatePort(node);
    }

    if (nodeType == UB_DEVICE && provisionDefaultCtpEntity)
    {
        Ptr<UbController> controller = node->GetObject<UbController>();
        controller->CreateDefaultCtpEntity(1);
        controller->FreezeCtpEntities();
    }

    sw->Init();
    Ptr<UbCongestionControl> congestionCtrl = UbCongestionControl::Create(UB_SWITCH);
    congestionCtrl->OnSwitchAttached(sw);
}

inline void
AddShortestRoute(Ptr<Node> node, uint32_t destNodeId, uint32_t destPortId, uint16_t outPort)
{
    std::vector<uint16_t> outPorts = {outPort};
    Ptr<UbRoutingProcess> routing = node->GetObject<UbSwitch>()->GetRoutingProcess();
    routing->GetRoutingTable().AddShortestRoute(NodeIdToIp(destNodeId).Get(), outPorts);
    routing->GetRoutingTable().AddShortestRoute(NodeIdToIp(destNodeId, destPortId).Get(), outPorts);
}

}
#endif
