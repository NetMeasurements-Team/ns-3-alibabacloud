// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_DATATYPE_H
#define UB_DATATYPE_H

#include "ns3/callback.h"
#include "ns3/ipv4-address.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/core-module.h"
#include "ns3/ub-lifecycle.h"

#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ns3 {


extern GlobalValue g_ub_priority_num;
extern GlobalValue g_ub_vl_num;

// ============================================================================
// 常量定义
// ============================================================================

// 优先级相关常量
const uint8_t UB_PRIORITY_NUM_DEFAULT = 16;         // 支持的优先级数量默认值 (0-15)
const uint8_t UB_PRIORITY_HIGH = 0;                 // 最高优先级
const uint8_t UB_PRIORITY_LOW = 15;                 // 最低优先级
const uint8_t UB_PRIORITY_DEFAULT = 8;              // 默认优先级
const uint8_t UB_PRIORITY_MAX = UB_PRIORITY_LOW;    // 最大优先级值

// Jetty相关常量
const uint32_t UB_JETTY_TASSN_OOO_THRESHOLD = 2048; // Jetty分段乱序阈值（用于乱序缓存等）

// WQE相关常量
const uint32_t UB_WQE_TA_SEGMENT_BYTE = 64 * 1024;  // TA层分段大小 (64KB)
const uint32_t UB_MTU_BYTE = 4 * 1024;              // 最大传输单元（TP层）

// Credit相关常量
const uint8_t UB_CREDIT_MAX_VALUE = 63;             // CREDIT最大值

// 操作类型枚举
enum class UbOperationType : uint8_t {
    WRITE = 0, // 写操作
    READ = 1   // 读操作
};

enum class UbTransactionSegmentKind : uint8_t {
    REQUEST = 0,
    RESPONSE = 1
};

enum class UbTransactionType : uint8_t
{
    MEMORY = 0,
    MESSAGE = 1,
    MAINTENANCE = 2,
    MANAGEMENT = 3,
};

enum class UbTransactionHeaderKind : uint8_t
{
    BTAH = 0,
    ATAH,
    MAETAH,
    UDETAH,
    MTETAH,
    TVETAH,
    TAIDETAH,
    OFSTETAH,
    IMMETAH,
    NTFETAH,
    BEETAH,
    MGMTETAH,
    INVALID = 0xff,
};

struct UbMemoryAccessFields
{
    /** Target virtual address of the described memory segment, in bytes. */
    uint64_t address{0};
    /** UB memory descriptor token that authorizes the access. */
    uint32_t tokenId{0};
    /** Transaction length described by the header, in bytes. */
    uint32_t length{0};
};
/** Target hint and context carried by MTETAH for message operations. */
struct UbMessageTargetFields
{
    /** Target context type selector. */
    uint8_t targetContextType{0};
    /** Target context identifier. */
    uint32_t targetContextId{0};
    /** Optional target hint value. */
    uint8_t hint{0};
};
/** Immediate data carried by IMMETAH for immediate message operations. */
struct UbImmediateFields
{
    /** Immediate value, up to eight bytes. */
    uint64_t data{0};
    /** Token that authorizes the immediate operation. */
    uint32_t tokenValue{0};
    /** Message length this immediate value belongs to, in bytes. */
    uint32_t messageLength{0};
};
/** Notify address, token, and payload carried by NTFETAH for notify writes. */
struct UbNotifyFields
{
    /** Notification address the target writes to, in bytes. */
    uint64_t address{0};
    /** Token that authorizes the notification. */
    uint32_t tokenId{0};
    /** Notification token value. */
    uint32_t tokenValue{0};
    /** Notification payload. */
    uint64_t data{0};
};
/** Byte-enable bitmap carried by BEETAH; this phase has no supported request using it. */
struct UbByteEnableFields
{
    /** Raw byte-enable bitmap as it appears on the wire. */
    std::vector<uint8_t> bytes;
};
/** UDETAH fields used by the 2.1 remote-sync operations. */
struct UbUserDataFields
{
    uint8_t type{0};
    uint8_t opcode{0};
    uint8_t offset{0};
    uint8_t reserved{0};
};
/** Management command fields carried by MGMTETAH. */
struct UbManagementFields
{
    /** Management command code. */
    uint8_t code{0};
    /** Management command sub-code. */
    uint8_t subCode{0};
    /** Whether this unit is a management response. */
    bool response{false};
    /** Management status code. */
    uint8_t status{0};
    /**
     * Management PDU length in bytes.
     *
     * A locally submitted request defaults this to its transaction payload length and both sides
     * validate that the two agree.
     */
    uint16_t pduLength{0};
};
/**
 * Normalized transaction field values of one request or response.
 *
 * Each member is present only when the wire unit carries the corresponding extension header. Local
 * submissions receive the rule defaults of their required headers in the single request
 * validation entry; decoded requests carry exactly the fields their headers provided.
 */
struct UbTransactionFields
{
    /** MAETAH: memory address and token. */
    std::optional<UbMemoryAccessFields> memoryAccess;
    /** TVETAH / UDETAH token value when the header is present. */
    std::optional<uint32_t> tokenValue;
    /** MTETAH: message target context. */
    std::optional<UbMessageTargetFields> messageTarget;
    /** Optional wire task identifier. */
    std::optional<uint16_t> taskId;
    /** Optional byte offset. */
    std::optional<uint32_t> offset;
    /** IMMETAH: immediate value, token, and message length. */
    std::optional<UbImmediateFields> immediate;
    /** NTFETAH: notify address, token, and payload. */
    std::optional<UbNotifyFields> notify;
    /** BEETAH: byte-enable bitmap; unsupported in this phase. */
    std::optional<UbByteEnableFields> byteEnable;
    /** UDETAH type/opcode/offset fields. */
    std::optional<UbUserDataFields> userData;
    /** MGMTETAH: management command fields. */
    std::optional<UbManagementFields> management;
};

// 内存语义操作类型枚举
enum class UbMemOperationType : uint8_t {
    STORE = 0,
    LOAD = 1,
    MEM_STORE_ACK = 2,
    MEM_LOAD_RESP
};

// ============================================================================
// 类型定义
// ============================================================================

using UbPriority = uint8_t;

// ============================================================================
// 前向声明
// ============================================================================
struct TpgTag;

// ============================================================================
// UB Header相关字段定义
// ============================================================================
// 定义UbTransactionOpcode枚举，便于使用
enum class UbTransactionOpcode : uint8_t {
    SEND = 0x00,             // send
    SEND_WITH_IMMEDIATE = 0x01,   // send with immediate (8B)
    RESERVED_02 = 0x02,  // send with invalidate
    WRITE = 0x03,            // write （最常用）
    WRITE_WITH_IMMEDIATE = 0x04,  // write with immediate (8B)
    WRITE_WITH_NOTIFY = 0x05,     // write with notify (8B)
    READ = 0x06,             // read （最常用）
    ATOMIC_COMPARE_SWAP = 0x07,  // atomic compare swap
    ATOMIC_SWAP = 0x08,      // atomic swap
    ATOMIC_STORE = 0x09,     // atomic store
    ATOMIC_LOAD = 0x0A,      // atomic load
    ATOMIC_FETCH_ADD = 0x0B, // atomic fetch add
    ATOMIC_FETCH_SUB = 0x0C, // atomic fetch sub
    ATOMIC_FETCH_AND = 0x0D, // atomic fetch and
    ATOMIC_FETCH_OR = 0x0E,  // atomic fetch or
    ATOMIC_FETCH_XOR = 0x0F, // atomic fetch xor
    MANAGEMENT = 0x10,      // management
    TAACK = 0x11,  // transaction ACK
    READ_RESPONSE = 0x12,    // read response
    ATOMIC_RESPONSE = 0x13,  // atomic response
    WRITE_WITH_BE = 0x14,         // write with BE
    PREFETCH_TGT = 0x15,     // PrefetchTgt
    RESERVED_16 = 0x16,  // Disconnect SCID
    WRITEBACK = 0x17,   // WriteBackFull
    WRITEBACK_WITH_BE = 0x18,    // WriteBackPtl
    WRITE_WITH_ATOMIC_STORE_ADD = 0x19,
    WRITE_WITH_REMOTE_SYNC = 0x1A,
    WRITE_WITH_REMOTE_SYNC_WITH_BE = 0x1B,
    ATOMIC_STORE_WITH_RETURN_STATUS = 0x1C,
    ATOMIC_SYNC = 0x1D,
    MAX_OPCODE = 0x1E               // 无效的TA OPCODE
};

/**
 * Atomic sub-operation selected by a local Atomic_load or Atomic_store submission.
 *
 * The sub-operation is local-only input: it is not encoded in a transaction header, so a decoded
 * request never carries one.
 */
enum class UbAtomicSubOperation : uint8_t
{
    ADD = 0,
    CLR = 1,
    EOR = 2,
    SET = 3,
    SMAX = 4,
    SMIN = 5,
    UMAX = 6,
    UMIN = 7,
};

struct UbAtomicOptions
{
    /** Local Atomic_load/Atomic_store sub-operation; required only for those two opcodes. */
    std::optional<UbAtomicSubOperation> subOperation;
    /** Explicit request payload override in bytes; the rule default applies when absent. */
    std::optional<uint32_t> requestPayloadBytes;
};

/**
 * Normalized transaction admission result cached on a WQE and on its segments.
 *
 * It is produced exactly once for a request: by the local submission path or by the inbound decode
 * path. Every later stage (segmentation, transport, response matching, response construction)
 * consumes this cached value instead of recomputing payload metadata, and `error` is empty only for
 * the cached, admissible result.
 */
struct UbTransactionResolved
{
    /** Transaction type of the operation, taken from its static rule. */
    UbTransactionType type{UbTransactionType::MEMORY};
    /** Request opcode this result was produced for. */
    UbTransactionOpcode requestOpcode{UbTransactionOpcode::MAX_OPCODE};
    /** Opcode of the protocol response the target answers with. */
    UbTransactionOpcode responseOpcode{UbTransactionOpcode::TAACK};
    /**
     * Logical transaction length in bytes.
     *
     * For Atomic requests this is the operand width; for Read requests it is the requested length
     * that the response must carry.
     */
    uint32_t logicalBytes{0};
    /** Request payload bytes the initiator must send for this request. */
    uint32_t requestPayloadBytes{0};
    /** Response payload bytes the target must return; zero when the response carries none. */
    uint32_t responsePayloadBytes{0};
    /**
     * Whether successful completion of this request requires its transaction response.
     *
     * This is the static protocol requirement of the operation. It stays true after the response
     * has been accepted and is never used as the dynamic completion progress of a WQE; TP ACKs
     * advance transport confirmation while the matching response advances segment completion.
     */
    bool requiresResponse{false};
    /** Rejection reason when this value was not produced by a successful validation; empty when
     * the result is admissible. */
    std::string error;
};


enum class UbDatalinkHeaderConfig : uint8_t {
    CONTROL = 0x00,         // Config value for control credit header
    PACKET_IPV4 = 0x03,     // Config value for ipv4 packet header
    PACKET_IPV6 = 0x04,     // Config value for ipv6 packet header
    PACKET_CNA16 = 0x06,    // Config value for ipv6 packet header
    PACKET_CNA24 = 0x07,    // Config value for ipv6 packet header
    PACKET_UB_MEM = 0x09    // Config value for UB memory packet header
};

enum class RoutingType : uint8_t {
    PER_FLOW_ALL_PATHS = 0b00,
    PER_PACKET_ALL_PATHS = 0b01,
    PER_FLOW_SHORTEST_PATHS = 0b10,
    PER_PACKET_SHORTEST_PATHS = 0b11,
};

enum class MultipathSelector : uint8_t {
    HASH64 = 0,
    CRC32 = 1,
    TOEPLITZ = 2,
    ROUND_ROBIN = 3,
    ADAPTIVE = 4,
    INGRESS_PORT_STRIPE = 5,
};

constexpr const char*
RoutingTypeToString(RoutingType routingType)
{
    switch (routingType)
    {
    case RoutingType::PER_FLOW_ALL_PATHS:
        return "PER_FLOW_ALL_PATHS";
    case RoutingType::PER_PACKET_ALL_PATHS:
        return "PER_PACKET_ALL_PATHS";
    case RoutingType::PER_FLOW_SHORTEST_PATHS:
        return "PER_FLOW_SHORTEST_PATHS";
    case RoutingType::PER_PACKET_SHORTEST_PATHS:
        return "PER_PACKET_SHORTEST_PATHS";
    }
    return "UNKNOWN";
}

constexpr const char*
MultipathSelectorToString(MultipathSelector selector)
{
    switch (selector)
    {
    case MultipathSelector::HASH64:
        return "HASH64";
    case MultipathSelector::CRC32:
        return "CRC32";
    case MultipathSelector::TOEPLITZ:
        return "TOEPLITZ";
    case MultipathSelector::ROUND_ROBIN:
        return "ROUND_ROBIN";
    case MultipathSelector::ADAPTIVE:
        return "ADAPTIVE";
    case MultipathSelector::INGRESS_PORT_STRIPE:
        return "INGRESS_PORT_STRIPE";
    }
    return "UNKNOWN";
}

constexpr bool
RoutingTypeIsPerPacket(RoutingType routingType)
{
    return (static_cast<uint8_t>(routingType) & 0b01) != 0;
}

constexpr bool
RoutingTypeUsesShortestPaths(RoutingType routingType)
{
    return (static_cast<uint8_t>(routingType) & 0b10) != 0;
}

constexpr RoutingType
MakeRoutingType(bool perPacket, bool shortestPaths)
{
    return static_cast<RoutingType>((shortestPaths ? 0b10 : 0) | (perPacket ? 0b01 : 0));
}

constexpr bool
MultipathSelectorIsValidForRoutingType(MultipathSelector selector, RoutingType routingType)
{
    switch (selector)
    {
    case MultipathSelector::HASH64:
    case MultipathSelector::CRC32:
    case MultipathSelector::TOEPLITZ:
        return true;
    case MultipathSelector::ROUND_ROBIN:
    case MultipathSelector::ADAPTIVE:
        return RoutingTypeIsPerPacket(routingType);
    case MultipathSelector::INGRESS_PORT_STRIPE:
        return !RoutingTypeIsPerPacket(routingType);
    }
    return false;
}

// 定义Order枚举
enum class OrderType : uint8_t {
    ORDER_NO = 0x00,      // No Order: 与其它报文无保序要求
    ORDER_RELAX = 0x01,   // Relax Order: SO报文不能超越前面带RO的报文
    ORDER_STRONG = 0x02,  // Strong Order: SO报文不能超越前面带RO的报文
    ORDER_RESERVED = 0x03 // reserved
};

// 定义Initiator Requester Context类型枚举
enum class IniRcType : uint8_t {
    REQUESTER_CONTEXT = 0x01,                  // Initiator Context
    DESTINATION_SEQUENCE_CONTEXT = 0x02,       // Requester Context
    RESERVED = 0x03                             // Destination Sequence Context
};

// 定义TPOpcode常量，便于使用
enum class TpOpcode : uint8_t {
    TP_OPCODE_UNRELIABLE_TA = 0x0,      // 不可靠TA数据包
    TP_OPCODE_RELIABLE_TA = 0x1,        // 可靠TA数据包（典型数据包）
    TP_OPCODE_ACK_WITHOUT_CETPH = 0x2,  // 不带CETPH的TP ACK（典型ACK）
    TP_OPCODE_ACK_WITH_CETPH = 0x3,     // 带CETPH的TP ACK
    TP_OPCODE_RESERVED1 = 0x4,          // 保留
    TP_OPCODE_SACK_WITHOUT_CETPH = 0x5, // 不带CETPH的TP SACK
    TP_OPCODE_SACK_WITH_CETPH = 0x6,    // 带CETPH的TP SACK
    TP_OPCODE_RESERVED2 = 0x7,          // 保留
    TP_OPCODE_CNP = 0x8                 // CNP拥塞通知包
};

enum class UbRetransmissionMode : uint8_t {
    GBN = 0,
    SELECTIVE = 1,
};

enum class UbRetransTimeoutMode : uint8_t {
    STATIC = 0,
    DYNAMIC = 1,
};

enum class TransportMode : uint8_t {
    RTP = 0,
    CTP = 1,
};

enum class CtpOpcode : uint8_t {
    CTP_DATA = 0x0,
    CTP_CNP = 0x1,
};

constexpr uint8_t UB_CNA_NLP_CTPH = 0x0;
constexpr uint8_t UB_CNA_NLP_RTPH = 0x2;
// Simulator-local CFG7 compact LDST TP bypass extension (not a standard NLP).
constexpr uint8_t UB_CNA_NLP_LDST_BYPASS = 0x4;
constexpr uint8_t UB_CTPH_NLP_COMPACT_TAH = 0x0;
constexpr uint8_t UB_CTPH_NLP_UPI32_EID128_TAH = 0x1;
constexpr uint8_t UB_CTPH_NLP_UPI16_EID40_TAH = 0x2;
constexpr uint32_t UB_COMPACT_EID_MAX = 0xFFFFF;
constexpr uint16_t UB_COMPACT_UPI_MAX = 0x7FFF;

// 定义NLP常量，便于使用
enum class NextLayerProtocol : uint8_t {
    NLP_TAH = 0x0,      // TAH (事务头)
    NLP_UPI_UEID = 0x1, // UPI + UEID (虚拟化相关，忽略)
    NLP_RESERVED = 0x2, // 保留
    NLP_CIP = 0x3       // CIP (保密性和完整性保护，忽略)
};

/**
 * @brief 验证优先级是否有效
 * @param priority 要验证的优先级值
 * @return true 如果有效，false 否则
 */
inline bool IsValidPriority(UbPriority priority)
{
    return priority <= UB_PRIORITY_MAX;
}

/**
 * @brief
 *
 * 表示一个完整的内存语义任务，包含任务描述信息、网络信息。
 */
class UbLdstTaskSegment : public Object {
public:
    static TypeId GetTypeId(void);

    // ========== 构造函数 ==========
    UbLdstTaskSegment()
        : m_src(0),
          m_dest(0),
          m_type(UbMemOperationType::STORE),
          m_size(0),
          m_bytesLeft(0)
    {
    }

    UbLdstTaskSegment(uint32_t src,
              uint32_t dest,
              UbMemOperationType type,
              uint32_t size,
              UbPriority priority)
        : m_src(src),
          m_dest(dest),
          m_type(type),
          m_size(size),
          m_priority(priority),
          m_bytesLeft(size)
    {
    }

    ~UbLdstTaskSegment() = default;

    // ========== 仿真全局信息 ==========
    uint32_t GetTaskId() const
    {
        return m_taskId;
    }

    void SetTaskId(uint32_t taskId)
    {
        m_taskId = taskId;
    }

    uint32_t GetTaskSegmentId() const
    {
        return m_taskSegmentId;
    }

    void SetTaskSegmentId(uint32_t taskSegmentId)
    {
        m_taskSegmentId = taskSegmentId;
    }

    uint32_t GetThreadId() const
    {
        return m_threadId;
    }

    void SetThreadId(uint32_t threadId)
    {
        m_threadId = threadId;
    }

    // ========== 任务描述信息 ==========
    uint32_t GetSrc() const
    {
        return m_src;
    }

    uint32_t GetDest() const
    {
        return m_dest;
    }

    bool HasSrcPortId() const
    {
        return m_hasSrcPortId;
    }

    bool HasDstPortId() const
    {
        return m_hasDstPortId;
    }

    uint32_t GetSrcPortId() const
    {
        return m_srcPortId;
    }

    uint32_t GetDstPortId() const
    {
        return m_dstPortId;
    }

    UbMemOperationType GetType() const
    {
        return m_type;
    }

    uint32_t GetSize() const
    {
        return m_size;
    }

    UbPriority GetPriority() const
    {
        return m_priority;
    }

    uint32_t GetPsnSize() const
    {
        return m_psnCnt;
    }

    void SetSrc(uint32_t src)
    {
        m_src = src;
    }

    void SetDest(uint32_t dest)
    {
        m_dest = dest;
    }

    void SetSrcPortId(uint32_t portId)
    {
        m_srcPortId = portId;
        m_hasSrcPortId = true;
    }

    void SetDstPortId(uint32_t portId)
    {
        m_dstPortId = portId;
        m_hasDstPortId = true;
    }

    void SetType(UbMemOperationType type)
    {
        m_type = type;
    }

    void SetSize(uint32_t size)
    {
        m_size = size;
        m_bytesLeft = size;
    }

    void SetPacketInfo(uint32_t packetSize, uint32_t length)
    {
        m_length = length;
        m_dataSize = 64 * (1 << length);
        m_packetSize = packetSize;
        // 计算要发的packet数目
        m_psnCnt = (m_size + m_dataSize - 1) / m_dataSize;
    }

    uint32_t GetLength()
    {
        return m_length;
    }

    uint32_t GetDataSize()
    {
        return m_dataSize;
    }

    uint32_t GetPacketSize()
    {
        return m_packetSize;
    }

    void SetPriority(UbPriority priority)
    {
        m_priority = priority;
    }

    // ========== 动态信息 ==========
    uint64_t GetBytesLeft() const
    {
        return m_bytesLeft;
    }

    // ========== 业务逻辑方法 ==========
    // 判断MEM是否已经发送完成（即剩余字节为0）
    bool IsSentCompleted() const
    {
        // 已发送完成：所有数据都已发送，剩余字节为0
        return m_bytesLeft == 0;
    }

    /**
     * @brief 重置MEM发送状态，可用于重传
     */
    void Reset()
    {
        m_bytesLeft = m_size;
    }

    /**
     * @brief 查看下一个packet的大小
     * @return 对应分段的大小
     */
    uint32_t PeekNextDataSize() const
    {
        return std::min(m_dataSize, m_bytesLeft);
    }

    /**
     * @brief 检查MEM配置是否有效
     * @return true 如果配置有效，false 否则
     */
    bool IsValid() const
    {
        return m_size > 0 && IsValidPriority(m_priority) && m_src != m_dest;
    }

    /**
     * @brief 更新已发送的字节数
     * @param sentBytes 本次发送的字节数
     * @return 实际发送的字节数（不会超过剩余字节数）
     */
    uint64_t UpdateSentBytes(uint32_t sentBytes)
    {
        uint64_t actualSent = std::min(sentBytes, m_bytesLeft);
        m_bytesLeft -= actualSent;
        return actualSent;
    }

private:
    // ========== 全局信息 ==========
    uint32_t m_taskId;
    uint32_t m_threadId;
    uint32_t m_taskSegmentId;       // 用于识别tasksegment
    // ========== 任务描述信息 ==========
    uint32_t m_src;                 // 源节点标识符
    uint32_t m_dest;                // 目的节点标识符
    uint32_t m_srcPortId{0};
    uint32_t m_dstPortId{0};
    bool m_hasSrcPortId{false};
    bool m_hasDstPortId{false};
    UbMemOperationType m_type = UbMemOperationType::STORE; // 操作类型
    uint32_t m_size = 0;                                     // MemTask数据大小 (字节)
    UbPriority m_priority = UB_PRIORITY_DEFAULT;             // MemTask优先级 (0-15, 0最高)

    uint64_t m_address = 0;    // 待访问的内存起始地址
    uint32_t m_length = 0;     // 单个切片要访问的内存段数据长度， 64 * (2 ^ length)
    uint32_t m_dataSize = 0;   // 单个切片要访问的内存段数据长度
    uint32_t m_psnCnt = 0;          // 总计获取的数据包个数计数
    uint32_t m_bytesLeft = 0;       // 剩余的字节数
    uint32_t m_msn = 0;
    uint32_t m_packetSize = 0; // 请求包的payload size
};

// ============================================================================
// 数据结构定义
//
//  仿真各层次数据结构关系：
//  数据结构：     WQE(message)         WQE->WQESegment          WQESegment -> Packet
//  角色功能：  维护WQE完成信息        保证WQESegment可靠完成       保证Packet可靠完成，
//                                       上报WQE完成信息          上报WQESegment完成信息
//  角色：         Client --------------> TA(jetty) --------------> TP
// ============================================================================

/**
 * @brief Work Queue Entry (WQE) 类
 *
 * 表示一个完整的工作队列项（消息），包含任务描述信息、网络信息和TA层调度信息。
 * WQE 会被分解为多个 WQE Segment 进行细粒度调度。
 */
class UbWqe : public Object {
public:
    static TypeId GetTypeId(void);

    // ========== 构造函数 ==========
    UbWqe()
        : m_src(0),
          m_dest(0),
          m_sport(0),
          m_dport(0),
          m_type(UbTransactionOpcode::WRITE),
          m_size(0),
          m_priority(UB_PRIORITY_DEFAULT),
          m_taMsn(0),
          m_taSsnStart(0),
          m_taSsnSize(0),
          m_bytesLeft(0)
    {
    }

    UbWqe(uint32_t src,
          uint32_t dest,
          uint8_t sport,
          uint8_t dport,
          uint32_t win,
          uint32_t baseRtt,
          UbTransactionOpcode type,
          uint32_t size,
          UbPriority priority)
        : m_src(src),
          m_dest(dest),
          m_sport(sport),
          m_dport(dport),
          m_type(type),
          m_size(size),
          m_priority(priority),
          m_taMsn(0),
          m_taSsnStart(0),
          m_taSsnSize((m_size + UB_WQE_TA_SEGMENT_BYTE - 1) / UB_WQE_TA_SEGMENT_BYTE),
          m_bytesLeft(size)
    {
    }

    ~UbWqe() = default;

    // ========== 仿真全局信息 ==========
    uint32_t GetWqeId() const
    {
        return m_wqeId;
    }

    void SetWqeId(uint32_t wqeId)
    {
        m_wqeId = wqeId;
    }

    void SetAtomicOptions(const UbAtomicOptions& options)
    {
        m_atomicOptions = options;
    }

    const std::optional<UbAtomicOptions>& GetAtomicOptions() const
    {
        return m_atomicOptions;
    }

    void SetTransactionFields(const UbTransactionFields& fields) { m_transactionFields = fields; }
    const UbTransactionFields& GetTransactionFields() const { return m_transactionFields; }
    void SetTransactionResolved(const UbTransactionResolved& resolved) { m_transactionResolved = resolved; }
    bool HasTransactionResolved() const { return m_transactionResolved.has_value(); }
    const UbTransactionResolved& GetTransactionResolved() const { return *m_transactionResolved; }

    bool IsLifecycleManaged() const
    {
        return m_lifecycleManaged;
    }

    void MarkLifecycleManaged()
    {
        m_lifecycleManaged = true;
    }

    // ========== 任务描述信息 ==========
    uint32_t GetSrc() const
    {
        return m_src;
    }

    uint32_t GetDest() const
    {
        return m_dest;
    }

    bool HasSrcEntityId() const
    {
        return m_hasSrcEntityId;
    }

    bool HasDstEntityId() const
    {
        return m_hasDstEntityId;
    }

    uint32_t GetSrcEntityId() const
    {
        return m_srcEntityId;
    }

    uint32_t GetDstEntityId() const
    {
        return m_dstEntityId;
    }

    uint8_t GetSport() const
    {
        return m_sport;
    }

    uint8_t GetDport() const
    {
        return m_dport;
    }

    UbTransactionOpcode GetType() const
    {
        return m_type;
    }

    uint32_t GetSize() const
    {
        return m_size;
    }

    UbPriority GetPriority() const
    {
        return m_priority;
    }

    void SetSrc(uint32_t src)
    {
        m_src = src;
    }

    void SetDest(uint32_t dest)
    {
        m_dest = dest;
    }

    void SetSrcEntityId(uint32_t srcEntityId)
    {
        m_srcEntityId = srcEntityId;
        m_hasSrcEntityId = true;
    }

    void SetDstEntityId(uint32_t dstEntityId)
    {
        m_dstEntityId = dstEntityId;
        m_hasDstEntityId = true;
    }

    void SetSport(uint8_t sport)
    {
        m_sport = sport;
    }

    void SetDport(uint8_t dport)
    {
        m_dport = dport;
    }

    void SetType(UbTransactionOpcode type)
    {
        m_type = type;
    }

    void SetSize(uint32_t size)
    {
        m_size = size;
        m_bytesLeft = size;
        // 计算TA层分段数
        m_taSsnSize = (m_size + UB_WQE_TA_SEGMENT_BYTE - 1) / UB_WQE_TA_SEGMENT_BYTE;
    }

    void SetPriority(UbPriority priority)
    {
        m_priority = priority;
    }

    void SetExplicitPayload(Ptr<const Packet> payload)
    {
        m_explicitPayload = payload;
    }

    Ptr<const Packet> GetExplicitPayload() const
    {
        return m_explicitPayload;
    }

    bool HasExplicitPayload() const
    {
        return m_explicitPayload != nullptr;
    }

    // ========== 网络层信息 ==========
    Ipv4Address GetSip() const
    {
        return m_sip;
    }

    Ipv4Address GetDip() const
    {
        return m_dip;
    }

    void SetSip(const Ipv4Address& sip)
    {
        m_sip = sip;
    }

    void SetDip(const Ipv4Address& dip)
    {
        m_dip = dip;
    }

    uint32_t GetJettyNum() const
    {
        return m_jettyNum;
    }

    uint64_t GetTaMsn() const
    {
        return m_taMsn;
    }

    uint64_t GetTaSsnStart() const
    {
        return m_taSsnStart;
    }

    uint64_t GetTaSsnSize() const
    {
        return m_taSsnSize;
    }

    void SetJettyNum(uint32_t jettyNum)
    {
        m_jettyNum = jettyNum;
    }

    void SetTaMsn(uint64_t msn)
    {
        m_taMsn = msn;
    }

    void SetTaSsnStart(uint64_t start)
    {
        m_taSsnStart = start;
    }

    void SetTaSsnSize(uint64_t size)
    {
        m_taSsnSize = size;
    }

    // ========== 动态信息 ==========
    uint64_t GetBytesLeft() const
    {
        return m_bytesLeft;
    }

    // ========== 业务逻辑方法 ==========
    /**
     * @brief 检查WQE是否已发送完成
     * @return true 如果所有数据都已发送，false 否则
     */
    // 判断WQE是否已经发送完成（即剩余字节为0）
    bool IsSentCompleted() const
    {
        // 已发送完成：所有数据都已发送，剩余字节为0
        return m_bytesLeft == 0;
    }

    /**
     * @brief 重置WQE发送状态，可用于重传
     */
    void Reset()
    {
        m_bytesLeft = m_carrierBytes == 0 ? m_size : m_carrierBytes;
    }

    /**
     * @brief 获取下一个TA分段大小，并更新WQE剩余字节数
     * @return 对应分段的大小
     */
    uint32_t GetNextSegmentSize()
    {
        uint32_t actualSent = PeekNextSegmentSize();
        m_bytesLeft -= actualSent;
        return actualSent;
    }

    /**
     * @brief 查看下一个segment的大小
     * @return 对应分段的大小
     */
    uint32_t PeekNextSegmentSize() const
    {
        return std::min(UB_WQE_TA_SEGMENT_BYTE, m_bytesLeft);
    }

    /**
     * @brief 检查WQE配置是否有效
     * @return true 如果配置有效，false 否则
     */
    bool IsValid() const
    {
        return m_size > 0 && IsValidPriority(m_priority) && m_src != m_dest;
    }

    bool CanSend() const
    {
        return m_canSend;
    }

    void SetCanSend(bool status)
    {
        m_canSend = status;
    }

    /**
     * @brief 更新已发送的字节数
     * @param sentBytes 本次发送的字节数
     * @return 实际发送的字节数（不会超过剩余字节数）
     */
    uint64_t UpdateSentBytes(uint32_t sentBytes)
    {
        uint64_t actualSent = std::min(sentBytes, m_bytesLeft);
        m_bytesLeft -= actualSent;
        return actualSent;
    }

    void SetOrderType(OrderType type) { m_order = type; }
    OrderType GetOrderType() { return m_order; }

    void SetSegmentKind(UbTransactionSegmentKind segmentKind)
    {
        m_segmentKind = segmentKind;
    }

    UbTransactionSegmentKind GetSegmentKind() const
    {
        return m_segmentKind;
    }

    void SetOriginJettyNum(uint32_t originJettyNum)
    {
        m_originJettyNum = originJettyNum;
    }

    uint32_t GetOriginJettyNum() const
    {
        return m_originJettyNum;
    }

    void SetRequestTassn(uint32_t requestTassn)
    {
        m_requestTassn = requestTassn;
    }

    uint32_t GetRequestTassn() const
    {
        return m_requestTassn;
    }

    void SetRequestOpcode(UbTransactionOpcode requestOpcode)
    {
        m_requestOpcode = requestOpcode;
    }

    UbTransactionOpcode GetRequestOpcode() const
    {
        return m_requestOpcode;
    }

    void SetResponseBytes(uint32_t responseBytes)
    {
        m_responseBytes = responseBytes;
    }

    uint32_t GetResponseBytes() const
    {
        return m_responseBytes;
    }

    void SetRemoteAddress(uint64_t remoteAddress)
    {
        m_remoteAddress = remoteAddress;
    }

    uint64_t GetRemoteAddress() const
    {
        return m_remoteAddress;
    }

    void SetRemoteTokenId(uint32_t remoteTokenId)
    {
        m_remoteTokenId = remoteTokenId;
    }

    uint32_t GetRemoteTokenId() const
    {
        return m_remoteTokenId;
    }

    void SetNeedsTransactionResponse(bool needsTransactionResponse)
    {
        m_needsTransactionResponse = needsTransactionResponse;
    }

    bool NeedsTransactionResponse() const
    {
        return m_needsTransactionResponse;
    }

    void SetResLenBytes(uint32_t resLenBytes)
    {
        // TODO: Rename ResLen/resLenBytes to ResponseLen/responseLenBytes in a dedicated
        // mechanical cleanup. Keep the current API stable in the retransmission MR.
        m_resLenBytes = resLenBytes;
    }

    uint32_t GetResLenBytes() const
    {
        return m_resLenBytes;
    }

    void SetLogicalBytes(uint32_t logicalBytes)
    {
        SetResLenBytes(logicalBytes);
    }

    uint32_t GetLogicalBytes() const
    {
        return GetResLenBytes();
    }

    void SetPayloadBytes(uint32_t payloadBytes)
    {
        m_payloadBytes = payloadBytes;
    }

    uint32_t GetPayloadBytes() const
    {
        return m_payloadBytes;
    }

    void SetCarrierBytes(uint32_t carrierBytes)
    {
        m_carrierBytes = carrierBytes;
    }

    uint32_t GetCarrierBytes() const
    {
        return m_carrierBytes;
    }
private:
    // ========== 全局信息 ==========
    uint32_t m_wqeId;   // WQE标识符（Node范围内唯一）。仅用于数据收集。
    // ========== 任务描述信息 ==========
    uint32_t m_src;     // 源节点标识符
    uint32_t m_dest;    // 目的节点标识符
    bool m_hasSrcEntityId{false};
    bool m_hasDstEntityId{false};
    uint32_t m_srcEntityId{0};
    uint32_t m_dstEntityId{0};
    uint8_t m_sport;    // 源端口号
    uint8_t m_dport;    //< 目的端口号
    UbTransactionOpcode m_type = UbTransactionOpcode::WRITE;           // 操作类型 (READ/WRITE)
    uint32_t m_size = 0;                         // WQE数据大小 (字节)
    UbPriority m_priority = UB_PRIORITY_DEFAULT; // WQE优先级 (0-15, 0最高)

    // ========== 网络层信息 ==========
    Ipv4Address m_sip = Ipv4Address("0.0.0.0"); // 源IP地址
    Ipv4Address m_dip = Ipv4Address("0.0.0.0"); // 目的IP地址

    // ========== TA层静态信息 (调度时设置，之后不变) ==========
    bool m_canSend = false;
    uint32_t m_jettyNum;
    uint64_t m_taMsn;      // TA层消息序号 (Message Sequence Number)
    uint32_t m_taSsnStart; // TA层起始分段序号 (Segment Sequence Number)
    uint32_t m_taSsnSize;  // TA层分段数量
    OrderType m_order = OrderType::ORDER_NO;
    UbTransactionSegmentKind m_segmentKind = UbTransactionSegmentKind::REQUEST;
    uint32_t m_originJettyNum = UINT32_MAX;
    uint32_t m_requestTassn = UINT32_MAX;
    UbTransactionOpcode m_requestOpcode = UbTransactionOpcode::WRITE;
    uint32_t m_responseBytes = 0;
    uint64_t m_remoteAddress = 0;
    uint32_t m_remoteTokenId = 0;
    bool m_needsTransactionResponse = true;
    uint32_t m_resLenBytes = 0;
    uint32_t m_payloadBytes = 0;
    uint32_t m_carrierBytes = 0;
    bool m_lifecycleManaged = false;
    Ptr<const Packet> m_explicitPayload;
    std::optional<UbAtomicOptions> m_atomicOptions;
    UbTransactionFields m_transactionFields;
    std::optional<UbTransactionResolved> m_transactionResolved;

    // ========== TA层动态信息 (追踪WQE segment的完成) ==========
    uint32_t m_bytesLeft = 0; // 剩余的字节数
};

/**
 * @brief Work Queue Entry Segment 类
 *
 * 表示WQE被分段后的单个分段，用于更细粒度的调度和流控。
 * 包含完整的任务信息、网络信息、TA层信息和TP层信息。
 *
 * 一个WQE Segment会进一步被分解为多个网络包进行传输。
 */
class UbWqeSegment : public Object {
public:
    static TypeId GetTypeId(void);

    // ========== 构造函数 ==========
    UbWqeSegment()
    {
    }

    UbWqeSegment(uint32_t src,
                 uint32_t dest,
                 uint8_t sport,
                 uint8_t dport,
                 uint32_t jettyNum,
                 UbTransactionOpcode type,
                 uint32_t size,
                 UbPriority priority,
                 uint32_t taskId)
        : m_src(src),
          m_dest(dest),
          m_sport(sport),
          m_dport(dport),
          m_type(type),
          m_size(size),
          m_priority(priority),
          m_taskId(taskId),
          m_jettyNum(jettyNum),
          m_bytesLeft(size)
    {
    }

    ~UbWqeSegment() = default;

    // ========== 任务描述信息 ==========
    uint32_t GetSrc() const
    {
        return m_src;
    }

    uint32_t GetDest() const
    {
        return m_dest;
    }

    bool HasSrcEntityId() const
    {
        return m_hasSrcEntityId;
    }

    bool HasDstEntityId() const
    {
        return m_hasDstEntityId;
    }

    uint32_t GetSrcEntityId() const
    {
        return m_srcEntityId;
    }

    uint32_t GetDstEntityId() const
    {
        return m_dstEntityId;
    }

    uint8_t GetSport() const
    {
        return m_sport;
    }

    uint8_t GetDport() const
    {
        return m_dport;
    }

    void SetTransactionFields(const UbTransactionFields& fields) { m_transactionFields = fields; }
    const UbTransactionFields& GetTransactionFields() const { return m_transactionFields; }
    void SetTransactionResolved(const UbTransactionResolved& resolved) { m_transactionResolved = resolved; }
    bool HasTransactionResolved() const { return m_transactionResolved.has_value(); }
    const UbTransactionResolved& GetTransactionResolved() const { return *m_transactionResolved; }

    UbTransactionOpcode GetType() const
    {
        return m_type;
    }

    uint32_t GetSize() const
    {
        return m_size;
    }

    UbPriority GetPriority() const
    {
        return m_priority;
    }

    uint32_t GetTaskId() const
    {
        return m_taskId;
    }

    uint32_t GetWqeSize() const
    {
        return m_wqeSize;
    }

    void SetSrc(uint32_t src)
    {
        m_src = src;
    }

    void SetDest(uint32_t dest)
    {
        m_dest = dest;
    }

    void SetSrcEntityId(uint32_t srcEntityId)
    {
        m_srcEntityId = srcEntityId;
        m_hasSrcEntityId = true;
    }

    void SetDstEntityId(uint32_t dstEntityId)
    {
        m_dstEntityId = dstEntityId;
        m_hasDstEntityId = true;
    }

    void SetSport(uint8_t sport)
    {
        m_sport = sport;
    }

    void SetDport(uint8_t dport)
    {
        m_dport = dport;
    }

    void SetType(UbTransactionOpcode type)
    {
        m_type = type;
    }

    void SetSize(uint32_t size)
    {
        m_size = size;
        m_bytesLeft = size;
        // 计算TA层分段数
        m_psnSize = (m_size + UB_MTU_BYTE - 1) / UB_MTU_BYTE;
    }

    void SetPriority(UbPriority priority)
    {
        m_priority = priority;
    }

    void SetExplicitPayload(Ptr<const Packet> payload)
    {
        m_explicitPayload = payload;
    }

    Ptr<const Packet> GetExplicitPayload() const
    {
        return m_explicitPayload;
    }

    bool HasExplicitPayload() const
    {
        return m_explicitPayload != nullptr;
    }

    void SetTaskId(uint32_t taskId)
    {
        m_taskId = taskId;
    }

    void SetWqeSize(uint32_t size)
    {
        m_wqeSize = size;
    }

    // ========== 网络层信息 ==========
    Ipv4Address GetSip() const
    {
        return m_sip;
    }

    Ipv4Address GetDip() const
    {
        return m_dip;
    }

    void SetSip(const Ipv4Address& sip)
    {
        m_sip = sip;
    }

    void SetDip(const Ipv4Address& dip)
    {
        m_dip = dip;
    }

    // ========== TA层信息 ==========
    uint32_t GetJettyNum() const
    {
        return m_jettyNum;
    }

    uint64_t GetTaMsn() const
    {
        return m_taMsn;
    }

    uint32_t GetTaSsn() const
    {
        return m_taSsn;
    }

    void SetJettyNum(const uint32_t n)
    {
        m_jettyNum = n;
    }

    void SetTaMsn(uint64_t msn)
    {
        m_taMsn = msn;
    }

    void SetTaSsn(uint32_t ssn)
    {
        m_taSsn = ssn;
    }

    // ========== TP层静态信息 ==========
    uint64_t GetTpMsn() const
    {
        return m_tpMsn;
    }

    uint64_t GetPsnStart() const
    {
        return m_psnStart;
    }

    uint32_t GetPsnSize() const
    {
        return m_psnSize;
    }

    void SetTpMsn(uint64_t msn)
    {
        m_tpMsn = msn;
    }

    void SetPsnStart(uint64_t start)
    {
        m_psnStart = start;
    }

    // ========== TP层动态信息 ==========
    uint64_t GetBytesLeft() const
    {
        return m_bytesLeft;
    }

    // ========== 业务逻辑方法 ==========
    /**
     * @brief 检查分段是否完全发送完成
     * @return true 如果所有字节都已发送，false 否则
     */
    bool IsSentCompleted() const
    {
        return m_bytesLeft == 0;
    }

    bool HasStartedSending() const
    {
        return m_hasStartedSending;
    }

    /**
     * @brief 更新已发送的字节数
     * @param sentBytes 本次发送的字节数
     * @return 实际发送的字节数（不会超过剩余字节数）
     */
    uint64_t UpdateSentBytes(uint32_t sentBytes)
    {
        uint64_t actualSent = std::min(sentBytes, m_bytesLeft);
        m_bytesLeft -= actualSent;
        m_hasStartedSending = m_hasStartedSending || actualSent != 0;
        return actualSent;
    }

    /**
     * @brief 重设已发送的字节数
     * @param
     * @return
     */
    void ResetSentBytes()
    {
        m_bytesLeft = m_carrierBytes == 0 ? m_size : m_carrierBytes;
    }

    /**
     * @brief 重设已发送的字节数
     * @param sentBytes 不需要重置的字节数
     * @return
     */
    void ResetSentBytes(uint32_t sentBytes)
    {
        const uint32_t progressBytes = m_carrierBytes == 0 ? m_size : m_carrierBytes;
        m_bytesLeft = sentBytes >= progressBytes ? 0 : progressBytes - sentBytes;
    }

    /**
     * @brief 获取下一个Packet分段大小，并更新WQE Segment剩余字节数
     * @return 对应Packet的大小
     */
    uint32_t GetNextPacketSize() // 移除const修饰符，因为函数修改了成员变量
    {
        uint64_t actualSent = std::min(UB_MTU_BYTE, m_bytesLeft); // 修正为使用UB_MTU_BYTE
        m_bytesLeft -= actualSent;
        return actualSent; // 添加显式类型转换
    }

    /**
     * @brief 查看下一个Packet的大小
     * @return 对应Packet的大小
     */
    uint32_t PeekNextPacketSize() const
    {
        return std::min(UB_MTU_BYTE, m_bytesLeft); // 添加显式类型转换
    }

    /**
     * @brief 重置分段状态（用于重传）
     */
    void Reset()
    {
        m_bytesLeft = m_size;
    }

    /**
     * @brief 检查分段是否有效
     * @return true 如果分段配置有效，false 否则
     */
    bool IsValid() const
    {
        return m_size > 0 && IsValidPriority(m_priority) && m_src != m_dest;
    }

    void SetOrderType(OrderType type) { m_orderType = type; }

    OrderType GetOrderType() { return m_orderType; }

    void SetTpn(uint32_t tpn) { m_tpn = tpn; }

    uint32_t GetTpn() { return m_tpn; }

    void SetResolvedTpIdentity(const UbLifecycleResourceIdentity& identity)
    {
        m_resolvedTpIdentity = identity;
        m_hasResolvedTpIdentity = true;
    }

    bool HasResolvedTpIdentity() const
    {
        return m_hasResolvedTpIdentity;
    }

    const UbLifecycleResourceIdentity& GetResolvedTpIdentity() const
    {
        NS_ABORT_MSG_IF(!m_hasResolvedTpIdentity, "WQE segment has no resolved TP generation");
        return m_resolvedTpIdentity;
    }

    void SetTransactionCorrelation(const UbLifecycleResourceIdentity& operationIdentity,
                                   const UbLifecycleResourceIdentity& segmentIdentity)
    {
        m_operationIdentity = operationIdentity;
        m_segmentIdentity = segmentIdentity;
        m_hasTransactionCorrelation = true;
    }

    void SetTransactionCorrelation(const UbTransactionCorrelationIdentity& identity)
    {
        SetTransactionCorrelation(identity.operation, identity.segment);
    }

    bool HasTransactionCorrelation() const
    {
        return m_hasTransactionCorrelation;
    }

    const UbLifecycleResourceIdentity& GetTransactionOperationIdentity() const
    {
        NS_ABORT_MSG_IF(!m_hasTransactionCorrelation,
                        "WQE segment has no Transaction lifecycle correlation");
        return m_operationIdentity;
    }

    const UbLifecycleResourceIdentity& GetTransactionSegmentIdentity() const
    {
        NS_ABORT_MSG_IF(!m_hasTransactionCorrelation,
                        "WQE segment has no Transaction lifecycle correlation");
        return m_segmentIdentity;
    }

    UbTransactionCorrelationIdentity GetTransactionCorrelation() const
    {
        return {GetTransactionOperationIdentity(), GetTransactionSegmentIdentity()};
    }

    void SetSegmentKind(UbTransactionSegmentKind segmentKind)
    {
        m_segmentKind = segmentKind;
    }

    UbTransactionSegmentKind GetSegmentKind() const
    {
        return m_segmentKind;
    }

    void SetOriginJettyNum(uint32_t originJettyNum)
    {
        m_originJettyNum = originJettyNum;
    }

    uint32_t GetOriginJettyNum() const
    {
        return m_originJettyNum;
    }

    void SetRequestTassn(uint32_t requestTassn)
    {
        m_requestTassn = requestTassn;
    }

    uint32_t GetRequestTassn() const
    {
        return m_requestTassn;
    }

    // CTP wire identity is distinct from the local caller Jetty number.
    void SetCtpRequesterIdentity(uint32_t rcid, uint8_t type)
    {
        m_ctpRcid = rcid;
        m_ctpRcType = type;
    }
    std::optional<uint32_t> GetCtpRcid() const { return m_ctpRcid; }
    uint8_t GetCtpRcType() const { return m_ctpRcType; }

    void SetRequestOpcode(UbTransactionOpcode requestOpcode)
    {
        m_requestOpcode = requestOpcode;
    }

    UbTransactionOpcode GetRequestOpcode() const
    {
        return m_requestOpcode;
    }

    void SetResponseBytes(uint32_t responseBytes)
    {
        m_responseBytes = responseBytes;
    }

    uint32_t GetResponseBytes() const
    {
        return m_responseBytes;
    }

    void SetRemoteAddress(uint64_t remoteAddress)
    {
        m_remoteAddress = remoteAddress;
    }

    uint64_t GetRemoteAddress() const
    {
        return m_remoteAddress;
    }

    void SetRemoteTokenId(uint32_t remoteTokenId)
    {
        m_remoteTokenId = remoteTokenId;
    }

    uint32_t GetRemoteTokenId() const
    {
        return m_remoteTokenId;
    }

    void SetNeedsTransactionResponse(bool needsTransactionResponse)
    {
        m_needsTransactionResponse = needsTransactionResponse;
    }

    bool NeedsTransactionResponse() const
    {
        return m_needsTransactionResponse;
    }

    void SetResLenBytes(uint32_t resLenBytes)
    {
        // TODO: Rename ResLen/resLenBytes to ResponseLen/responseLenBytes in a dedicated
        // mechanical cleanup. Keep the current API stable in the retransmission MR.
        m_resLenBytes = resLenBytes;
    }

    uint32_t GetResLenBytes() const
    {
        return m_resLenBytes;
    }

    void SetLogicalBytes(uint32_t logicalBytes)
    {
        SetResLenBytes(logicalBytes);
    }

    uint32_t GetLogicalBytes() const
    {
        return GetResLenBytes();
    }

    void SetPayloadBytes(uint32_t payloadBytes)
    {
        m_payloadBytes = payloadBytes;
    }

    uint32_t GetPayloadBytes() const
    {
        return m_payloadBytes;
    }

    void SetCarrierBytes(uint32_t carrierBytes)
    {
        m_carrierBytes = carrierBytes;
        m_bytesLeft = carrierBytes;
        m_psnSize = (carrierBytes + UB_MTU_BYTE - 1) / UB_MTU_BYTE;
    }

    uint32_t GetCarrierBytes() const
    {
        return m_carrierBytes;
    }
private:
    // ========== 任务描述信息 ==========
    uint32_t m_src{0};                              // 源节点标识符
    uint32_t m_dest{0};                             // 目的节点标识符
    bool m_hasSrcEntityId{false};
    bool m_hasDstEntityId{false};
    uint32_t m_srcEntityId{0};
    uint32_t m_dstEntityId{0};
    uint8_t m_sport{0};                             // 源端口号
    uint8_t m_dport{0};                             // 目的端口号
    UbTransactionOpcode m_type{UbTransactionOpcode::WRITE};     // 操作类型 (READ/WRITE)
    uint32_t m_size{0};                             // 分段数据大小 (字节)
    bool m_hasStartedSending{false};
    UbPriority m_priority{UB_PRIORITY_DEFAULT};     // 分段优先级
    uint32_t m_taskId{0};                           // 流的任务ID
    uint32_t m_wqeSize{0};                          // 所属WQE的size
    OrderType m_orderType{OrderType::ORDER_NO};     // 所属WQE的order类型
    uint32_t m_tpn{UINT32_MAX}; // 当前WqeSegment由哪一个TP进行传输
    bool m_hasResolvedTpIdentity{false};
    UbLifecycleResourceIdentity m_resolvedTpIdentity;
    bool m_hasTransactionCorrelation{false};
    UbLifecycleResourceIdentity m_operationIdentity;
    UbLifecycleResourceIdentity m_segmentIdentity;

    // ========== 网络层信息 ==========
    Ipv4Address m_sip; // 源IP地址
    Ipv4Address m_dip; // 目的IP地址

    // ========== TA层信息 ==========
    uint32_t m_jettyNum;                // 来源Jetty标号，报文头中需要携带
    uint64_t m_taMsn{0};                // 所属WQE的TA层消息序号
    uint32_t m_taSsn{0};                // 本分段的TA层分段序号
    UbTransactionSegmentKind m_segmentKind{UbTransactionSegmentKind::REQUEST};
    uint32_t m_originJettyNum{UINT32_MAX};
    uint32_t m_requestTassn{UINT32_MAX};
    std::optional<uint32_t> m_ctpRcid;
    uint8_t m_ctpRcType{1};
    UbTransactionOpcode m_requestOpcode{UbTransactionOpcode::WRITE};
    uint32_t m_responseBytes{0};
    uint64_t m_remoteAddress{0};
    uint32_t m_remoteTokenId{0};
    bool m_needsTransactionResponse{true};
    uint32_t m_resLenBytes{0};
    uint32_t m_payloadBytes{0};
    uint32_t m_carrierBytes{0};
    Ptr<const Packet> m_explicitPayload;

    // ========== TP层静态信息 (调度时设置，之后不变) ==========
    uint64_t m_tpMsn{0};    // TP层消息序号
    uint64_t m_psnStart{0}; // TP层起始包序号 (Packet Sequence Number)
    uint32_t m_psnSize{0};  // TP层包数量

    // ========== TP层动态信息 (发送时实时更新) ==========
    UbTransactionFields m_transactionFields;
    std::optional<UbTransactionResolved> m_transactionResolved;

    uint32_t m_bytesLeft{0}; // 剩余字节数
};

/**
 * @brief Transport Group标签结构体
 *
 * 位字段格式: [src:16][dest:16][priority:4][type:2][id:4] = 42 bits
 * 用于标识传输组，管理多个相关的TP通道。
 */
struct TpgTag {
    struct Fields {
        uint64_t src : 18;
        uint64_t dest : 18;
        uint64_t priority : 4;
        uint64_t type : 2;
        uint64_t id : 4;
        uint64_t reserved : 18;
    };

    union {
        uint64_t value;
        Fields fields;
    };

    // 构造函数
    TpgTag()
        : value(0)
    {
    }

    explicit TpgTag(uint64_t v)
        : value(v)
    {
    }

    TpgTag(uint16_t src, uint16_t dest, uint8_t priority, uint8_t type, uint8_t id = 0)
        : value(0)
    {
        fields.src = src;
        fields.dest = dest;
        fields.priority = priority;
        fields.type = type;
        fields.id = id;
    }

    // 字段提取函数
    uint16_t GetSrc() const
    {
        return fields.src;
    }

    uint16_t GetDest() const
    {
        return fields.dest;
    }

    uint8_t GetPriority() const
    {
        return fields.priority;
    }

    uint8_t GetType() const
    {
        return fields.type;
    }

    uint8_t GetId() const
    {
        return fields.id;
    }

    // 字段设置函数
    void SetSrc(uint16_t src)
    {
        fields.src = src;
    }

    void SetDest(uint16_t dest)
    {
        fields.dest = dest;
    }

    void SetPriority(uint8_t priority)
    {
        fields.priority = priority;
    }

    void SetType(uint8_t type)
    {
        fields.type = type;
    }

    void SetId(uint8_t id)
    {
        fields.id = id;
    }

    // 工具函数
    bool IsValid() const
    {
        return value != 0;
    }

    void Reset()
    {
        value = 0;
    }

    // 操作符重载
    explicit operator uint64_t() const
    {
        return value;
    }

    bool operator==(const TpgTag& other) const
    {
        return value == other.value;
    }

    bool operator!=(const TpgTag& other) const
    {
        return value != other.value;
    }

    bool operator<(const TpgTag& other) const
    {
        return value < other.value;
    }
};

// ============================================================================
// 预定义常量
// ============================================================================
// const TpgTag INVALID_TPG_TAG{0};     // 无效TPG标签

// Load Balance Mode values
constexpr bool LB_MODE_PER_FLOW = false;  // 0: per flow
constexpr bool LB_MODE_PER_PACKET = true; // 1: per packet

// Routing Policy values
constexpr bool ROUTING_ALL_PATHS = false; // 0: all paths
constexpr bool ROUTING_SHORTEST = true;   // 1: shortest paths

} // namespace ns3

#endif /* UB_DATATYPE_H */
