// SPDX-License-Identifier: GPL-2.0-only
#include "ub-datatype.h"

#include "ub-traffic-gen.h"

namespace ns3 {

/*********************
 * GlobalValue
 ********************/
GlobalValue g_ub_priority_num = GlobalValue("UB_PRIORITY_NUM",
                                            "支持的优先级数量 (1-16)",
                                            IntegerValue(16),
                                            MakeIntegerChecker<int>(1, 16));

GlobalValue g_ub_vl_num = GlobalValue("UB_VL_NUM",
                                      "支持的虚通道数量(1-16)，目前与优先级一一对应",
                                      IntegerValue(16),
                                      MakeIntegerChecker<int>(1, 16));

/*********************
 * UbWqe
 ********************/
TypeId UbWqe::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbWqe").SetParent<Object>().SetGroupName("UnifiedBus");
    return tid;
}

/*********************
 * UbWqeSegment
 ********************/
TypeId UbWqeSegment::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbWqeSegment").SetParent<Object>().SetGroupName("UnifiedBus");
    return tid;
}

/*********************
 * UbLdstTaskSegment
 ********************/
TypeId UbLdstTaskSegment::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbLdstTaskSegment")
                            .SetParent<Object>()
                            .SetGroupName("UnifiedBus");
    return tid;
}

} // namespace ns3
