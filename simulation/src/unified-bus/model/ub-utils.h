// SPDX-License-Identifier: GPL-2.0-only
#ifndef UTILS_H
#define UTILS_H

#include "ub-tp-connection-manager.h"

#include "ns3/config-store.h"
#include "ns3/core-module.h"
#include "ns3/enum.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/ub-reliable-rerouting.h"
#include "ns3/network-module.h"
#include "ns3/object-factory.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ptr.h"
#include "ns3/random-variable-stream.h"
#include "ns3/singleton.h"
#include "ns3/ub-app.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-fault.h"
#include "ns3/ub-link.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/ub-rule-routing.h"
#include "ns3/ub-lifecycle-trace.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-traffic-gen.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-transport.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <tuple>
#include <vector>

class UbLinkDelayOffsetTest;

namespace ns3
{
class UbNextHopIndex;
}

namespace utils {
class UbTraceFileConcurrencyTest;
class UbTraceWriterBoundedTest;
class UbTraceWriterDescriptorLruTest;
class UbQueueSamplerEventRetentionTest;

struct UbStructuredNodeConfig
{
    uint32_t nodeId;
    std::string nodeType;
    uint32_t portCount;
    std::string allocationDelay;
    std::string forwardDelay;
    uint32_t systemId{0};
};

struct UbStructuredLinkConfig
{
    uint32_t node1;
    uint32_t port1;
    uint32_t node2;
    uint32_t port2;
    std::string bandwidth;
    std::string delay;
};

struct UbStructuredTableRoute
{
    uint32_t currentNodeId;
    uint32_t destinationNodeId;
    uint32_t destinationPortId;
    std::vector<uint16_t> shortestPorts;
    std::vector<uint16_t> nonShortestPorts;
};

struct UbStructuredTopologyConfig
{
    std::vector<UbStructuredNodeConfig> nodes;
    std::vector<UbStructuredLinkConfig> links;
    std::vector<UbStructuredTableRoute> tableRoutes;
    uint64_t tableRouteCount{0};
    std::shared_ptr<const ns3::UbNextHopIndex> tableNextHopIndex;
    std::vector<ns3::UbPortGroup> portGroups;
    std::vector<ns3::UbRoutingRule> routingRules;
    std::vector<ns3::UbRoutingRangeRule> routingRangeRules;
    std::vector<ns3::UbTopologyRule> topologyRules;
    std::string routingFingerprint;
};

struct UbTraceWriterStorageSnapshot
{
    uint64_t ownerCount{0};
    uint64_t fileCount{0};
    uint64_t openFileCount{0};
    uint64_t currentBufferedBytes{0};
    uint64_t maxOwnerBufferedBytes{0};
    uint64_t openCount{0};
    uint64_t writeCount{0};
    uint64_t flushCount{0};
    uint64_t closeCount{0};
    uint64_t lruCloseCount{0};
    uint64_t pressureFlushCount{0};
    uint64_t peakOpenFiles{0};
    uint64_t peakBufferedBytes{0};
};

struct UbRuntimeStorageSnapshot
{
    static constexpr uint32_t SCHEMA_VERSION = 1;

    uint64_t nodeCount{0};
    uint64_t canonicalConnections{0};
    uint64_t authoritativeLocalConnectionViews{0};
    uint64_t secondaryConnectionIndexEntries{0};
    uint64_t residentTpEndpoints{0};
    uint64_t reservedEndpointSlots{0};
    uint64_t taskScopedJetties{0};
    uint64_t reusableJetties{0};
    uint64_t simulatorEventCount{0};
    uint64_t residentWqes{0};
    uint64_t residentOperations{0};
    uint64_t residentSegments{0};
    uint64_t activeLdstTasks{0};
    uint64_t activeLdstSegments{0};
    uint64_t storedTrafficTasks{0};
    uint64_t completedTrafficTasks{0};
    uint64_t trafficDependencyReferences{0};
    ns3::UbLifecycleStorageSnapshot lifecycle;
    UbTraceWriterStorageSnapshot traceWriter;
};
/**
 *  @brief UbUtils单例类
 */
class UbUtils : public ns3::Singleton<UbUtils> {
public:
  struct TraceSinkHandle
  {
      bool IsValid() const
      {
          return state != nullptr;
      }

    private:
      friend class UbUtils;
      void* state{nullptr};
      uint64_t epoch{0};
  };

    // Runtime / trace lifecycle helpers used by examples and tests.
    void PrintTimestamp(const std::string &message);

    void ParseTrace(bool isTest = false);

    void Destroy();

    void CreateTraceDir();

    static std::string PrepareTraceDir(const std::string &configPath);

    static void SetTracePathForTest(const std::string& tracePath);

    static TraceSinkHandle ResolveTraceSink(const std::string& fileName);

    static void AppendTraceInfoNoTs(const TraceSinkHandle& sink, const std::string& info);

    static UbTraceWriterStorageSnapshot GetTraceWriterStorageSnapshot();

    static UbRuntimeStorageSnapshot CaptureRuntimeStorageSnapshot();

    static void WriteRuntimeStorageSnapshot(const std::string& outputPath);

    // MPI locality helpers used by config-driven builder/tests.
    static uint32_t ExtractMpiRank(uint32_t systemId);

    static bool IsSameMpiRank(uint32_t lhsSystemId, uint32_t rhsSystemId);

    static bool IsSystemOwnedByRank(uint32_t systemId, uint32_t currentRank);

    // Config-driven builder surface used by ub-quick-example and white-box tests.
    void CreateNode(const std::string &filename);

    void CreateNode(const std::vector<UbStructuredNodeConfig>& nodes);

    // Loads traffic records and initializes phase-dependency state in UbTrafficGen.
    std::vector<TrafficRecord> LoadTrafficConfig(const std::string &filename);

    struct TrafficLoadStats
    {
        uint64_t recordCount{0};
        uint32_t maxTaskId{0};
    };

    // Streams traffic records without retaining a full vector in memory.
    void ForEachTrafficRecord(const std::string& filename,
                              const std::function<void(const TrafficRecord&)>& callback);

    // Callback-only row view; string_view fields must not be stored after callback return.
    void ForEachTrafficRecordView(const std::string& filename,
                                  const std::function<void(const TrafficRecordView&)>& callback);

    // Pre-registers phase membership before streaming AddTask calls.
    void RegisterTrafficPhaseDependencies(const std::string& filename);

    TrafficLoadStats RegisterTrafficPhaseDependenciesAndGetStats(const std::string& filename);

    struct LinkDelayOffsetStats
    {
        uint64_t positiveLinkCount{0};
        uint64_t zeroDelayLinkCount{0};
        uint64_t distinctOffsetCount{0};
        uint64_t offsetReuseCount{0};
        uint64_t maxLinksPerOffset{0};
    };

    void CreateTopo(const std::string& filename);

    LinkDelayOffsetStats CreateTopo(const std::string& filename,
                                    ns3::Time offsetWindow,
                                    uint32_t offsetSeed);

    LinkDelayOffsetStats CreateTopo(const std::vector<UbStructuredLinkConfig>& links,
                                    ns3::Time offsetWindow,
                                    uint32_t offsetSeed);

    void AddRoutingTable(const std::string &filename);

    void AddRoutingTable(const std::vector<UbStructuredTableRoute>& routes);

    void AddRuleRoutingTable(const std::string& portGroupFilename,
                             const std::string& ruleFilename,
                             const std::string& rangeRuleFilename = "");

    void AddRuleRoutingTable(const std::vector<ns3::UbPortGroup>& portGroups,
                             const std::vector<ns3::UbRoutingRule>& routingRules,
                             const std::vector<ns3::UbRoutingRangeRule>& routingRangeRules = {},
                             const std::vector<ns3::UbTopologyLink>& topologyLinks = {},
                             const std::vector<ns3::UbTopologyRule>& topologyRules = {});

    static UbStructuredTopologyConfig ReadStructuredTopologyConfig(std::istream& input);

    LinkDelayOffsetStats InstallStructuredTopology(const UbStructuredTopologyConfig& config,
                                                   ns3::Time offsetWindow,
                                                   uint32_t offsetSeed);

    void CreateTp(const std::string& filename);

    /**
     * @brief Provision and freeze the standard Host CTP Entity registries.
     *
     * The runner-only setup creates Entity 1 on each UB_DEVICE host and binds every local
     * CNA16-encodable UB port. Additional Entity definitions remain a controller API concern;
     * there is no case-file input for them.
     */
    void CreateDefaultCtpEntities();

    void SetComponentsAttribute(const std::string& filename);

    // Runtime trace wiring / attribute query / fault helpers.
    void TopoTraceConnect();

    void SingleTpTraceConnect(uint32_t nodeId, uint32_t tpn);

    void ClientTraceConnect(int srcNode);

    static void PfcStateNotify(uint32_t nodeId,
                               uint32_t portId,
                               const std::string& action,
                               uint32_t priority,
                               uint64_t ingressBytes);

    static void PfcDynamicStateNotify(uint32_t nodeId,
                                      uint32_t portId,
                                      uint32_t priority,
                                      uint64_t ingressTotalBytes,
                                      uint64_t sharedUsedBytes,
                                      uint64_t headroomUsedBytes,
                                      uint64_t xoffBytes,
                                      uint64_t xonBytes,
                                      bool pause);

    static void CbfcStateNotify(uint32_t nodeId,
                                uint32_t portId,
                                const std::string& action,
                                uint32_t vl,
                                int32_t availableCredits,
                                uint32_t nextPacketBytes,
                                uint32_t creditCellBytes);

    static void CbfcCreditRestoreTraceNotify(uint32_t nodeId,
                                             uint32_t portId,
                                             const std::vector<int32_t>& creditCells,
                                             uint32_t creditCellBytes);

    static void CbfcCreditLevelNotify(uint32_t nodeId,
                                      uint32_t portId,
                                      const std::string& reason,
                                      uint32_t vl,
                                      int32_t availableCredits,
                                      int32_t deltaCredits,
                                      uint32_t creditCellBytes);

    static void CbfcControlSendNotify(uint32_t nodeId,
                                      uint32_t portId,
                                      const std::string& reason,
                                      int32_t triggerThresholdCells,
                                      int32_t emitMinPendingCells,
                                      const std::vector<int32_t>& pendingCreditCells,
                                      const std::vector<int32_t>& sendCreditCells,
                                      uint32_t creditCellBytes);

    static void DcqcnMarkNotify(uint32_t nodeId,
                                uint32_t outPort,
                                uint64_t totalQueueBytes,
                                double markProbability);

    static void DcqcnCnpNotify(uint32_t nodeId,
                               uint32_t tpn,
                               const std::string& action,
                               uint8_t ecn,
                               bool location);

    static void DcqcnSenderStateNotify(uint32_t nodeId,
                                       uint32_t tpn,
                                       const std::string& reason,
                                       double alpha,
                                       uint64_t currentRateBps,
                                       uint64_t targetRateBps,
                                       uint64_t bytesSinceLastIncrease,
                                       uint32_t timeRecoveryEvents,
                                       uint32_t byteRecoveryEvents);

    static void CaqmAckNotify(uint32_t nodeId,
                              uint32_t tpn,
                              uint32_t psnStart,
                              uint32_t psnEnd,
                              uint8_t cE,
                              uint8_t iE,
                              uint16_t hintE);

    static void CaqmSenderStateNotify(uint32_t nodeId,
                                      uint32_t tpn,
                                      uint32_t psn,
                                      uint32_t sequence,
                                      uint32_t inFlight,
                                      uint32_t cwnd,
                                      uint8_t cE,
                                      bool iE,
                                      uint16_t hint);

    static bool IsTraceEnabled();

    static bool IsFlowControlTraceEnabled();

    static bool IsCongestionControlTraceEnabled();

    static bool IsQueueTraceEnabled();

    static bool IsTpDebugEnabledFor(uint32_t nodeId, uint32_t tpn);

    static void TpgPhaseCreateNotify(uint32_t srcNode,
                                     uint32_t dstNode,
                                     uint32_t taskId,
                                     uint32_t memberCount,
                                     uint32_t initialIndex,
                                     uint16_t initialPort,
                                     const std::vector<uint16_t>& memberPorts,
                                     const std::string& strategy);

    /**
     * @brief Trace sink for reliable-rerouting events.
     *
     * Writes `output/reliable_rerouting.csv` through the existing buffered trace writer when the
     * packet-trace category is enabled. Correctness and counters never depend on this sink.
     */
    static void ReliableReroutingNotify(const UbReliableReroutingEvent& record);
    static void ReliableReroutingOccupancyNotify(const UbRecoveryOccupancyEvent& record, bool summary);

    static void TpDebugStateNotify(uint32_t nodeId,
                                   uint32_t tpn,
                                   const std::string& reason,
                                   uint64_t psnSndNxt,
                                   uint64_t psnSndUna,
                                   uint64_t inflightPackets,
                                   uint64_t maxInflightPackets,
                                   bool inflightLimited,
                                   bool ccLimited,
                                   bool sendWindowLimited,
                                   uint32_t activeSegments,
                                   uint32_t totalSegments,
                                   uint32_t ackQueueLen,
                                   uint32_t cnpQueueLen);

    void StartQueueSampler();

    bool QueryAttributeInfo(int argc, char *argv[]);

    bool IsFaultEnabled() const;

    void InitFaultMoudle(const std::string &FaultConfigFile);

    static void ResetRuntimeDropDiagnostics();

    static void RecordRuntimePacketDrop(const std::string& reason);

    static uint64_t GetRuntimePacketDropCount();

    static std::string GetRuntimePacketDropReason();

private:
    friend class ::utils::UbTraceFileConcurrencyTest;
    friend class ::utils::UbTraceWriterBoundedTest;
    friend class ::utils::UbTraceWriterDescriptorLruTest;
    friend class ::utils::UbQueueSamplerEventRetentionTest;
    friend class ::UbLinkDelayOffsetTest;

    static ns3::Time ResolveLinkDelayWithOffset(ns3::Time baseDelay,
                                                ns3::Time offsetWindow,
                                                uint32_t offsetSeed,
                                                uint32_t node1,
                                                uint32_t port1,
                                                uint32_t node2,
                                                uint32_t port2);

    struct TraceFileState
    {
        std::string fileName;
        std::ofstream stream;
        std::mutex mutex;
        uint64_t lastUse{0};
        uint32_t activeWrites{0};
        bool headerWritten{false};
        bool headerCommitted{false};
        std::string csvHeader;
        bool open{false};
    };

    struct TraceOwnerState
    {
        std::mutex mutex;
        std::map<TraceFileState*, std::string> chunks;
        std::size_t bufferedBytes{0};
    };

    struct TraceWriterConfig
    {
        std::size_t chunkBytes;
        std::size_t highWaterBytes;
        std::size_t lowWaterBytes;
        std::size_t maxOpenFiles;
        std::size_t effectiveMaxOpenFiles;
    };

    struct TraceWriterStats
    {
        uint64_t openCount;
        uint64_t writeCount;
        uint64_t flushCount;
        uint64_t closeCount;
        uint64_t lruCloseCount;
        uint64_t pressureFlushCount;
        std::size_t peakOpenFiles;
        std::size_t peakBufferedBytes;
    };

    // Runtime trace state shared by current process.
    inline static std::string trace_path;

    inline static std::map<std::string, TraceFileState> files;
    inline static std::map<uint32_t, TraceOwnerState> trace_owners;
    inline static std::mutex files_mutex;
    inline static TraceWriterConfig trace_writer_config{64 * 1024,
                                                        1024 * 1024,
                                                        512 * 1024,
                                                        1024,
                                                        256};
    inline static TraceWriterStats trace_writer_stats{};
    inline static bool trace_writer_configured = false;
    inline static std::size_t trace_open_files = 0;
    inline static uint64_t trace_lru_clock = 0;
    inline static std::atomic<uint64_t> trace_writer_epoch{1};
    inline static std::mutex runtime_drop_mutex;
    inline static uint64_t runtime_packet_drop_count = 0;
    inline static std::string runtime_packet_drop_reason;
    inline static bool queue_sampler_started = false;
    inline static std::map<std::pair<uint32_t, uint32_t>, ns3::EventId> queue_sampler_events;

    ns3::GlobalValue g_fault_enable =
    ns3::GlobalValue("UB_FAULT_ENABLE", "Enable the fault injection module.", ns3::BooleanValue(false), ns3::MakeBooleanChecker());

    // 读取Traffic配置文件
    enum class FIELDCOUNT : int {
       TASKID = 0,
       SOURCENODE = 1,
       DESTNODE = 2,
       DATASIZE,
       OPTYPE,
       PRIORITY,
       DELAY,
       PHASEID,
       DEPENDONPHASES,
       SRCPORTID,
       DSTPORTID
    };

    struct NodeEle {
        std::string nodeIdStr;

        std::string nodeTypeStr;

        std::string portNumStr;

        std::string forwardDelay;

        std::string allocationDelay;

        std::string systemIdStr;
    };

    std::map<uint32_t, NodeEle> nodeEle_map;

    std::string g_config_path;

    bool TraceEnable = false;
    bool TaskTraceEnable = true;
    bool PacketTraceEnable = true;
    bool PortTraceEnable = true;
    bool RecordTraceEnabled = false;

    // 设置Trace全局变量
    ns3::GlobalValue g_trace_enable = ns3::GlobalValue("UB_TRACE_ENABLE",
                                                       "Master switch for all traces",
                                                       ns3::BooleanValue(true),
                                                       ns3::MakeBooleanChecker());

    ns3::GlobalValue g_tpg_phase_hash_enable =
        ns3::GlobalValue("UB_TPG_PHASE_HASH_ENABLE",
                         "Use a seeded hash for the initial RTP TPG TP cursor.",
                         ns3::BooleanValue(false),
                         ns3::MakeBooleanChecker());

    ns3::GlobalValue g_tpg_phase_seed =
        ns3::GlobalValue("UB_TPG_PHASE_SEED",
                         "Seed for the initial RTP TPG TP cursor hash.",
                         ns3::UintegerValue(0),
                         ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_tpg_phase_trace_enable =
        ns3::GlobalValue("UB_TPG_PHASE_TRACE_ENABLE",
                         "Enable initialization-only RTP TPG phase traces.",
                         ns3::BooleanValue(false),
                         ns3::MakeBooleanChecker());

    ns3::GlobalValue g_routing_rr_phase_hash_enable =
        ns3::GlobalValue("UB_ROUTING_RR_PHASE_HASH_ENABLE",
                         "Use a seeded hash for the initial routing RR candidate index.",
                         ns3::BooleanValue(false),
                         ns3::MakeBooleanChecker());

    ns3::GlobalValue g_routing_rr_phase_seed =
        ns3::GlobalValue("UB_ROUTING_RR_PHASE_SEED",
                         "Seed for the initial routing RR candidate index hash.",
                         ns3::UintegerValue(0),
                         ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_allocator_rr_phase_hash_enable =
        ns3::GlobalValue("UB_ALLOCATOR_RR_PHASE_HASH_ENABLE",
                         "Use a seeded hash for the initial allocator RR ingress index.",
                         ns3::BooleanValue(false),
                         ns3::MakeBooleanChecker());

    ns3::GlobalValue g_allocator_rr_phase_seed =
        ns3::GlobalValue("UB_ALLOCATOR_RR_PHASE_SEED",
                         "Seed for the initial allocator RR ingress index hash.",
                         ns3::UintegerValue(0),
                         ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_task_trace_enable = ns3::GlobalValue("UB_TASK_TRACE_ENABLE",
                                                            "Enable task and WQE level traces",
                                                            ns3::BooleanValue(true),
                                                            ns3::MakeBooleanChecker());

    ns3::GlobalValue g_packet_trace_enable = ns3::GlobalValue("UB_PACKET_TRACE_ENABLE",
                                                              "Enable packet send/ack/receive traces",
                                                              ns3::BooleanValue(true),
                                                              ns3::MakeBooleanChecker());

    ns3::GlobalValue g_port_trace_enable = ns3::GlobalValue("UB_PORT_TRACE_ENABLE",
                                                            "Enable port-level Tx/Rx traces (very noisy)",
                                                            ns3::BooleanValue(true),
                                                            ns3::MakeBooleanChecker());

    ns3::GlobalValue g_parse_enable = ns3::GlobalValue("UB_PARSE_TRACE_ENABLE",
                                                       "Run the Python trace-parsing script after simulation ends.",
                                                       ns3::BooleanValue(true),
                                                       ns3::MakeBooleanChecker());

    ns3::GlobalValue g_record_pkt_trace_enable = ns3::GlobalValue("UB_RECORD_PKT_TRACE",
                                                                  "Record per-hop packet path traces (high overhead).",
                                                                  ns3::BooleanValue(false),
                                                                  ns3::MakeBooleanChecker());

    ns3::GlobalValue g_flow_control_trace_enable =
    ns3::GlobalValue("UB_FLOW_CONTROL_TRACE_ENABLE",
                     "Enable algorithm-emitted flow-control traces such as PFC/CBFC trace files.",
                     ns3::BooleanValue(false),
                     ns3::MakeBooleanChecker());

    ns3::GlobalValue g_congestion_control_trace_enable =
    ns3::GlobalValue("UB_CONGESTION_CONTROL_TRACE_ENABLE",
                     "Enable algorithm-emitted congestion-control traces such as DCQCN/CAQM trace files.",
                     ns3::BooleanValue(false),
                     ns3::MakeBooleanChecker());

    ns3::GlobalValue g_queue_trace_enable =
    ns3::GlobalValue("UB_QUEUE_TRACE_ENABLE",
                     "Enable queue trace files such as QueueTrace_* from event-driven updates and sampling.",
                     ns3::BooleanValue(false),
                     ns3::MakeBooleanChecker());

    ns3::GlobalValue g_trace_preset =
    ns3::GlobalValue("UB_TRACE_PRESET",
                     "Trace category preset: minimal, balanced, detailed, or custom.",
                     ns3::StringValue("minimal"),
                     ns3::MakeStringChecker());

    ns3::GlobalValue g_lifecycle_trace_level =
    ns3::GlobalValue("UB_LIFECYCLE_TRACE_LEVEL",
                     "Lifecycle trace level override: OFF, SUMMARY, EVENT, or DETAIL.",
                     ns3::StringValue(""),
                     ns3::MakeStringChecker());

    ns3::GlobalValue g_lifecycle_detail_sample_stride =
    ns3::GlobalValue("UB_LIFECYCLE_DETAIL_SAMPLE_STRIDE",
                     "Retain one complete lifecycle identity chain per deterministic stride.",
                     ns3::UintegerValue(1),
                     ns3::MakeUintegerChecker<uint64_t>(1));

    ns3::GlobalValue g_lifecycle_detail_sample_offset =
    ns3::GlobalValue("UB_LIFECYCLE_DETAIL_SAMPLE_OFFSET",
                     "Selected identity remainder within the lifecycle detail sample stride.",
                     ns3::UintegerValue(0),
                     ns3::MakeUintegerChecker<uint64_t>());

    ns3::GlobalValue g_trace_chunk_bytes =
        ns3::GlobalValue("UB_TRACE_CHUNK_BYTES",
                         "Maximum unfinished trace bytes per execution owner and sink.",
                         ns3::UintegerValue(64 * 1024),
                         ns3::MakeUintegerChecker<uint64_t>(1024));

    ns3::GlobalValue g_trace_owner_high_water_bytes =
        ns3::GlobalValue("UB_TRACE_OWNER_HIGH_WATER_BYTES",
                         "Execution-owner trace buffer high-water mark.",
                         ns3::UintegerValue(1024 * 1024),
                         ns3::MakeUintegerChecker<uint64_t>(1024));

    ns3::GlobalValue g_trace_owner_low_water_bytes =
        ns3::GlobalValue("UB_TRACE_OWNER_LOW_WATER_BYTES",
                         "Execution-owner trace buffer pressure-flush target.",
                         ns3::UintegerValue(512 * 1024),
                         ns3::MakeUintegerChecker<uint64_t>());

    ns3::GlobalValue g_trace_max_open_files =
        ns3::GlobalValue("UB_TRACE_MAX_OPEN_FILES",
                         "Configured process-wide trace file descriptor maximum.",
                         ns3::UintegerValue(1024),
                         ns3::MakeUintegerChecker<uint64_t>(1));

    ns3::GlobalValue g_python_script_path =
    ns3::GlobalValue("UB_PYTHON_SCRIPT_PATH",
                     "Path to parse_trace.py script (REQUIRED - must be set by user)",
                     ns3::StringValue("/path/to/ns-3-ub-tools/trace_analysis/parse_trace.py"),
                     ns3::MakeStringChecker());

    ns3::GlobalValue g_queue_sample_interval =
    ns3::GlobalValue("UB_QUEUE_SAMPLE_INTERVAL_NS",
                     "Periodic queue sampling interval in ns. 0 disables periodic queue samples.",
                     ns3::UintegerValue(0),
                     ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_tp_debug_enable =
    ns3::GlobalValue("UB_TP_DEBUG_ENABLE",
                     "Enable focused TP sender-state debug trace.",
                     ns3::BooleanValue(false),
                     ns3::MakeBooleanChecker());

    ns3::GlobalValue g_tp_debug_node_id =
    ns3::GlobalValue("UB_TP_DEBUG_NODE_ID",
                     "Focused TP debug trace source node id. UINT32_MAX matches none.",
                     ns3::UintegerValue(std::numeric_limits<uint32_t>::max()),
                     ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_tp_debug_tpn =
    ns3::GlobalValue("UB_TP_DEBUG_TPN",
                     "Focused TP debug trace TP number. UINT32_MAX matches none.",
                     ns3::UintegerValue(std::numeric_limits<uint32_t>::max()),
                     ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_tp_debug_start_ns =
    ns3::GlobalValue("UB_TP_DEBUG_START_NS",
                     "Focused TP debug trace start time in ns.",
                     ns3::UintegerValue(0),
                     ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_tp_debug_end_ns =
    ns3::GlobalValue("UB_TP_DEBUG_END_NS",
                     "Focused TP debug trace end time in ns. 0 means no upper bound.",
                     ns3::UintegerValue(0),
                     ns3::MakeUintegerChecker<uint32_t>());

    static std::string Among(std::string s, std::string ts);

    void SetRecord(int fieldCount, std::string field, TrafficRecord &record);

    void ForEachTrafficRecordInternal(const std::string& filename,
                                      const std::string& action,
                                      const std::function<void(const TrafficRecord&)>& callback);

    void ForEachTrafficRecordViewInternal(
        const std::string& filename,
        const std::string& action,
        const std::function<void(const TrafficRecordView&)>& callback);

    void ForEachTrafficPhaseIndexRecord(const std::string& filename,
                                        const std::function<void(uint32_t, uint32_t)>& callback);

    static void PrintTraceInfo(const std::string& fileName, const std::string& info);

    static void PrintTraceInfoNoTs(const std::string& fileName, const std::string& info);

    static TraceFileState& GetTraceFile(const std::string& fileName);

    static TraceSinkHandle GetCachedTraceSink(const std::string& fileName);

    static TraceOwnerState& GetTraceOwner(uint32_t ownerId);

    static void AppendTraceLine(const TraceSinkHandle& sink, std::string line);

    static void AppendCsvRow(const std::string& fileName,
                             const std::string& header,
                             const std::string& row);

    static void CommitTraceChunk(TraceFileState& fileState, std::string& chunk);

    static void FlushTraceOwner(TraceOwnerState& owner);

    static void FlushTraceFile(TraceFileState& fileState);

    static void ConfigureTraceWriter();

    static void CloseTraceFile(TraceFileState& fileState, bool lruClose);

    static void QueueSampleNotify(uint32_t nodeId, uint32_t portId);
    static void QueueSampleTick(uint32_t nodeId, uint32_t portId, uint32_t intervalNs);

    static void TpFirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn,
                                         uint32_t tpMsn, uint32_t psnSndNxt, uint32_t sPort);

    static void TpLastPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn,
                                        uint32_t tpMsn, uint32_t psnSndNxt, uint32_t sPort);

    static void TpLastPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn,
                                       uint32_t tpMsn, uint32_t psn, uint32_t sPort);

    static void TpLastPacketReceivesNotify(uint32_t nodeId, uint32_t srcTpn, uint32_t dstTpn,
                                           uint32_t tpMsn, uint32_t psn, uint32_t dPort);

    static void TpWqeSegmentSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn);

    static void TpWqeSegmentCompletesNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn);

    static void TpRecvNotify(UbTpRecvTraceData data);

    static void CtpFirstPacketSendsNotify(UbCtpPacketTimingTraceData data);

    static void CtpLastPacketACKsNotify(UbCtpPacketTimingTraceData data);

    static void LdstRecvNotify(uint32_t packetUid,
                               uint32_t src,
                               uint32_t dst,
                               ns3::PacketType type,
                               uint32_t size,
                               uint32_t taskId,
                               ns3::UbPacketTraceTag traceTag);

    static void LdstFirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId);

    static void DagMemTaskStartsNotify(uint32_t nodeId, uint32_t taskId);

    static void DagMemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);

    static void DagComputeTaskQueuedNotify(uint32_t nodeId, uint32_t taskId);

    static void DagComputeTaskStartsNotify(uint32_t nodeId, uint32_t taskId);

    static void DagComputeTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);

    static void DagWqeTaskStartsNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId);

    static void DagWqeTaskCompletesNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId);

    static void PortTxNotify(uint32_t nodeId, uint32_t portId, uint32_t size);

    static void PortRxNotify(uint32_t nodeId, uint32_t portId, uint32_t size);

    static void QueueVoqNotify(uint32_t nodeId, uint32_t portId, uint64_t voqBytes);

    static void QueueIngressOccupancyNotify(uint32_t nodeId,
                                            uint32_t inPort,
                                            uint32_t priority,
                                            uint64_t bytes);

    static void QueueEgressEnqueueNotify(uint32_t nodeId,
                                         uint32_t portId,
                                         ns3::Ptr<const ns3::Packet> packet,
                                         uint32_t egressBytes);

    static void QueueEgressDequeueNotify(uint32_t nodeId,
                                         uint32_t portId,
                                         ns3::Ptr<const ns3::Packet> packet,
                                         uint32_t egressBytes);

    static void LdstThreadMemTaskStartsNotify(uint32_t nodeId, uint32_t memTaskId);

    static void LdstMemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);

    static void LdstThreadFirstPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId);

    static void LdstThreadLastPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId);

    static void LdstLastPacketACKsNotify(uint32_t nodeId, uint32_t taskId);

    static void LdstPeerSendFirstPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t type);

    static void SwitchLastPacketTraversesNotify(uint32_t nodeId, ns3::UbTransportHeader ubTpHeader);

    // 解析节点范围（如 "1..4"）
    inline void ParseNodeRange(const std::string &rangeStr, NodeEle nodeEle);

    // 读取TP配置文件
    void ParseLine(const std::string &line, Connection &conn);
};

}  // namespace utils

#endif  // UTILS_H
