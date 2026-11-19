// SPDX-License-Identifier: GPL-2.0-only
#include "ub-link-fault-schedule.h"

#include "ns3/log.h"
#include "ns3/node-list.h"
#include "ns3/simulator.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbLinkFaultSchedule");

namespace {

constexpr const char* kLinkFaultHeader = "nodeId,portId,failTimeNs";

std::string
Trim(std::string value)
{
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool
TryParseUnsigned(const std::string& text,
                 const std::string& path,
                 uint32_t line,
                 const char* field,
                 uint64_t& value,
                 std::string& error)
{
    if (text.empty())
    {
        std::ostringstream message;
        message << path << ":" << line << ": field '" << field << "' is empty";
        error = message.str();
        return false;
    }
    for (char c : text)
    {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0)
        {
            std::ostringstream message;
            message << path << ":" << line << ": field '" << field << "' must be a non-negative "
                    << "integer without a sign, fraction, or exponent but is '" << text << "'";
            error = message.str();
            return false;
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0')
    {
        std::ostringstream message;
        message << path << ":" << line << ": field '" << field << "' value '" << text
                << "' does not fit an unsigned 64-bit integer";
        error = message.str();
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

} // namespace

bool
TryReadLinkFaultSchedule(const std::string& path,
                         std::vector<UbLinkFaultScheduleRow>& rows,
                         std::string& error)
{
    rows.clear();
    error.clear();

    {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
        {
            error = path + ": is a directory, not a link fault schedule file";
            return false;
        }
    }

    std::ifstream input(path);
    if (!input.is_open())
    {
        std::ifstream probe(path, std::ios::binary);
        if (!probe.is_open())
        {
            // Missing file means "no scheduled failures".
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
            {
                return true;
            }
            error = path + ": exists but cannot be opened for reading";
            return false;
        }
    }

    std::string line;
    if (!std::getline(input, line))
    {
        // Zero-length file: no scheduled failures.
        return true;
    }

    const std::string header = Trim(line);
    if (header != kLinkFaultHeader)
    {
        error = path + ":1: header must be exactly '" + std::string(kLinkFaultHeader) +
                "' but is '" + header + "'";
        return false;
    }

    uint32_t lineNumber = 1;
    while (std::getline(input, line))
    {
        ++lineNumber;
        const std::string trimmed = Trim(line);
        if (trimmed.empty())
        {
            continue;
        }

        // Split on commas while preserving an empty trailing field, so `1,1,` still reports the
        // empty numeric field instead of a column-count error.
        std::vector<std::string> fields;
        std::string current;
        for (char c : trimmed)
        {
            if (c == ',')
            {
                fields.push_back(Trim(current));
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        fields.push_back(Trim(current));
        if (fields.size() != 3)
        {
            std::ostringstream message;
            message << path << ":" << lineNumber << ": expected exactly 3 fields but found "
                    << fields.size() << " in '" << trimmed << "'";
            error = message.str();
            return false;
        }

        UbLinkFaultScheduleRow row;
        row.line = lineNumber;
        if (!TryParseUnsigned(fields[0], path, lineNumber, "nodeId", row.nodeId, error) ||
            !TryParseUnsigned(fields[1], path, lineNumber, "portId", row.portId, error) ||
            !TryParseUnsigned(fields[2], path, lineNumber, "failTimeNs", row.failTimeNs, error))
        {
            return false;
        }
        if (row.failTimeNs > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            std::ostringstream message;
            message << path << ":" << lineNumber << ": field 'failTimeNs' value '" << fields[2]
                    << "' exceeds the simulator time range";
            error = message.str();
            return false;
        }
        rows.push_back(row);
    }

    return true;
}

bool
TryResolveLinkFaultSchedule(const std::vector<UbLinkFaultScheduleRow>& rows,
                            std::vector<UbLinkFaultTarget>& targets,
                            std::string& error)
{
    targets.clear();
    error.clear();

    // Every rejection clears the resolved output so a partially validated schedule can never be
    // mistaken for a usable one.
    const auto reject = [&targets, &error](const std::string& message) {
        targets.clear();
        error = message;
        return false;
    };

    for (const auto& row : rows)
    {
        if (row.nodeId >= NodeList::GetNNodes())
        {
            std::ostringstream message;
            message << "link_fault.csv:" << row.line << ": nodeId " << row.nodeId
                    << " does not exist in topology.csv";
            return reject(message.str());
        }
        Ptr<Node> node = NodeList::GetNode(static_cast<uint32_t>(row.nodeId));
        if (row.portId >= node->GetNDevices())
        {
            std::ostringstream message;
            message << "link_fault.csv:" << row.line << ": nodeId " << row.nodeId << " portId "
                    << row.portId << " does not exist in topology.csv";
            return reject(message.str());
        }
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(static_cast<uint32_t>(row.portId)));
        if (port == nullptr)
        {
            std::ostringstream message;
            message << "link_fault.csv:" << row.line << ": nodeId " << row.nodeId << " portId "
                    << row.portId << " is not a UB port";
            return reject(message.str());
        }
        Ptr<UbLink> link = DynamicCast<UbLink>(port->GetChannel());
        if (link == nullptr)
        {
            std::ostringstream message;
            message << "link_fault.csv:" << row.line << ": nodeId " << row.nodeId << " portId "
                    << row.portId << " has no attached UB link";
            return reject(message.str());
        }
        if (!link->IsReliableReroutingArmed())
        {
            std::ostringstream message;
            message << "link_fault.csv:" << row.line << ": the link at nodeId " << row.nodeId
                    << " portId " << row.portId
                    << " is not an eligible armed switch-to-switch link; enable "
                       "ns3::UbLink::EnableReliableRerouting for switch-to-switch links and check "
                       "the runtime support (MPI/MTP is not supported)";
            return reject(message.str());
        }

        UbLinkFaultTarget target;
        target.line = row.line;
        target.nodeId = static_cast<uint32_t>(row.nodeId);
        target.portId = static_cast<uint32_t>(row.portId);
        target.failTime = NanoSeconds(row.failTimeNs);
        target.link = link;
        if (!targets.empty() && PeekPointer(targets.front().link) != PeekPointer(link))
        {
            std::ostringstream message;
            message << "link_fault.csv:" << row.line << ": nodeId " << row.nodeId << " portId "
                    << row.portId
                    << " resolves to a different physical link than the earlier schedule rows; "
                       "version one supports one distinct failed link per simulation";
            return reject(message.str());
        }
        targets.push_back(target);
    }

    if (!targets.empty())
    {
        // Earliest event causes failure; equal times keep file order.
        std::stable_sort(targets.begin(),
                         targets.end(),
                         [](const UbLinkFaultTarget& lhs, const UbLinkFaultTarget& rhs) {
                             return lhs.failTime < rhs.failTime;
                         });
    }
    return true;
}

void
ScheduleLinkFaults(const std::vector<UbLinkFaultTarget>& targets)
{
    for (const auto& target : targets)
    {
        Simulator::Schedule(target.failTime, &UbLink::Fail, target.link);
    }
}

} // namespace ns3
