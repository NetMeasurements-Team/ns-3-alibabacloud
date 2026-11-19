// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-flow-control.h"

#include "ns3/log.h"

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbFlowControl);

LogComponent&
GetUbFlowControlLogComponent()
{
    static LogComponent component("UbFlowControl", __FILE__);
    return component;
}

TypeId
UbFlowControl::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbFlowControl").SetParent<Object>().AddConstructor<UbFlowControl>();
    return tid;
}

} // namespace ns3
