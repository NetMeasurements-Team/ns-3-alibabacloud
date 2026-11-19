// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LINK_FAULT_SCHEDULE_H
#define UB_LINK_FAULT_SCHEDULE_H

#include "ns3/nstime.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ns3 {

class UbLink;

/**
 * @brief One validated `link_fault.csv` row before topology resolution.
 */
struct UbLinkFaultScheduleRow
{
    uint32_t line{0};  //!< 1-based data-row line number in the file, for diagnostics.
    uint64_t nodeId{0};
    uint64_t portId{0};
    uint64_t failTimeNs{0};
};

/**
 * @brief One resolved schedule entry bound to a concrete physical link.
 */
struct UbLinkFaultTarget
{
    uint32_t line{0};
    uint32_t nodeId{0};
    uint32_t portId{0};
    Time failTime{Time(0)};
    Ptr<UbLink> link;
};

/**
 * @brief Read and strictly validate an optional case-local link fault schedule.
 *
 * Contract (design section 3.2):
 * - the header must be exactly `nodeId,portId,failTimeNs`;
 * - every data row has exactly three fields; surrounding whitespace is trimmed;
 * - blank lines are ignored;
 * - a missing, zero-length, or header-only file means "no scheduled failures";
 * - an existing unreadable file, or a malformed non-empty file, is an error;
 * - numeric fields must be complete, non-negative integers inside `uint64_t` and representable at
 *   the configured simulator time resolution.
 *
 * @param path Case-local file path.
 * @param rows Parsed rows in file order.
 * @param error Human-readable diagnostic naming file, line, field, and value.
 * @return true when the schedule is usable (possibly empty), false on any error.
 */
bool TryReadLinkFaultSchedule(const std::string& path,
                              std::vector<UbLinkFaultScheduleRow>& rows,
                              std::string& error);

/**
 * @brief Resolve every row to a physical switch-to-switch link.
 *
 * Validates node and port identity, attachment, endpoint roles, arming, and runtime support, and
 * rejects rows that resolve to a different physical link than the first row. Nothing is mutated:
 * all rows are validated before any fault action is scheduled.
 *
 * @param rows Parsed rows in file order.
 * @param targets Resolved targets, preserving file order for equal-time actions.
 * @param error Human-readable diagnostic.
 * @return true when every row resolved, false otherwise.
 */
bool TryResolveLinkFaultSchedule(const std::vector<UbLinkFaultScheduleRow>& rows,
                                 std::vector<UbLinkFaultTarget>& targets,
                                 std::string& error);

/**
 * @brief Schedule the resolved failure actions through `UbLink::Fail()`.
 *
 * Actions are absolute simulation times; duplicate and unsorted rows for the same link are allowed
 * and later actions are idempotent no-ops at runtime.
 */
void ScheduleLinkFaults(const std::vector<UbLinkFaultTarget>& targets);

} // namespace ns3

#endif // UB_LINK_FAULT_SCHEDULE_H
