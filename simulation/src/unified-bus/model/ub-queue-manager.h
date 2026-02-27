// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_QUEUE_MANAGER_H
#define UB_QUEUE_MANAGER_H

#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ub-network-address.h"

namespace ns3 {

constexpr long DEFAULT_INPORT_PRIORITY_BUFFER_SIZE = 1048576;  // 1MB

enum class IngressQueueType {
    VOQ,        // Virtual Output Queue - 转发数据包队列
    TP,         // Transport Channel - 传输层可靠通道
    BASE        // 基类默认值 - 不应出现在运行时
};

/**
 * @brief port的收包缓存队列
 */
class UbIngressQueue : public Object {
public:
    UbIngressQueue();
    virtual ~UbIngressQueue();
    static TypeId GetTypeId(void);
    
    virtual IngressQueueType GetIngressQueueType()
    {
        return IngressQueueType::BASE;
    }

    virtual bool IsEmpty();
    virtual bool IsLimited() { return false; }
    virtual Ptr<Packet> GetNextPacket();
    virtual uint32_t GetNextPacketSize();
    void SetInPortId(uint32_t inPortId) { m_inPortId = inPortId; }
    void SetIngressPriority(uint32_t priority) { m_ingressPriority = priority; }
    void SetOutPortId(uint32_t outPortId) { m_outPortId = outPortId; }
    uint32_t GetInPortId() { return m_inPortId; }
    uint32_t GetIngressPriority() { return m_ingressPriority; }
    uint32_t GetOutPortId() { return m_outPortId; }
    Time GetHeadArrivalTime() { return m_headArrivalTime; }
    bool IsControlFrame();
    bool IsForwardedDataPacket();
    bool IsGeneratedDataPacket();

protected:
    Time m_headArrivalTime = Seconds(0);

private:
    uint32_t m_ingressPriority;
    uint32_t m_inPortId;
    uint32_t m_outPortId;
};

/**
 * @brief VOQ (Virtual Output Queue) implementation of ingress queue
 */
class UbPacketQueue : public UbIngressQueue {
public:
    UbPacketQueue();
    ~UbPacketQueue() final;
    static TypeId GetTypeId(void);

    bool IsEmpty() override;
    Ptr<Packet> GetNextPacket() override;
    std::queue<Ptr<Packet>>& Get() {return m_queue;}
    Ptr<Packet> Front() {return m_queue.front();}
    void Pop() {
        m_queue.pop();
        if (!m_queue.empty()) {
            m_headArrivalTime = Simulator::Now();
        }
    }
    void Push(Ptr<Packet> p) {
        if (m_queue.empty()) {
            m_headArrivalTime = Simulator::Now();
        }
        m_queue.push(p);
    }
    IngressQueueType GetIngressQueueType();
    uint32_t GetNextPacketSize() override;

private:
    std::queue<Ptr<Packet>> m_queue;
    IngressQueueType m_ingressQueueType = IngressQueueType::VOQ;
};

/**
 * @brief VOQ Buffer统计管理模块（双视图：InPort + OutPort）
 * 
 * 架构说明：
 * - VOQ是Input-side的虚拟输出队列，按[outPort][priority][inPort]组织
 * - 本类提供两个统计视图：
 *   1. InPort视图：用于入端口流控（PFC检查入端口是否拥塞）
 *   2. OutPort视图：用于路由负载均衡和拥塞控制（检查出端口负载）
 * - 物理上只有一个包，但在两个视图中都有记录
 */
class UbQueueManager : public Object {
public:
    UbQueueManager();
    ~UbQueueManager() {}
    void Init();
    static TypeId GetTypeId(void);

    // Init
    void SetVLNum(uint32_t vlNum)
    {
        m_vlNum = vlNum;
    }
    void SetPortsNum(uint32_t portsNum)
    {
        m_portsNum = portsNum;
    }

    // ========== VOQ Dual-View Operations ==========
    
    /**
     * @brief 检查VOQ是否有空间容纳新包（检查双视图）
     * @param inPort 入端口
     * @param outPort 出端口
     * @param priority 优先级
     * @param pSize 包大小（字节）
     * @return true if both views have space
     */
    bool CheckVoqSpace(uint32_t inPort, uint32_t outPort, uint32_t priority, uint32_t pSize);
    
    /**
     * @brief 检查InPort视图是否有空间（用于丢包判断）
     * @param inPort 入端口
     * @param priority 优先级
     * @param pSize 包大小（字节）
     * @return true if InPort buffer has space
     */
    bool CheckInPortSpace(uint32_t inPort, uint32_t priority, uint32_t pSize);
    
    /**
     * @brief 查询OutPort视图的缓冲区占用（纯监控功能，不用于丢包决策）
     * @param outPort 出端口
     * @param priority 优先级
     * @param pSize 包大小（字节）
     * @return 统计信息，始终返回true（OutPort视图无物理缓冲区限制）
     */
    bool CheckOutPortSpace(uint32_t outPort, uint32_t priority, uint32_t pSize);
    
    /**
     * @brief 包进入VOQ时调用（同时更新双视图）
     */
    void PushToVoq(uint32_t inPort, uint32_t outPort, uint32_t priority, uint32_t pSize);
    
    /**
     * @brief 包离开VOQ时调用（同时更新双视图）
     */
    void PopFromVoq(uint32_t inPort, uint32_t outPort, uint32_t priority, uint32_t pSize);
    
    // ========== 查询接口：InPort视图（用于流控） ==========
    
    /**
     * @brief 获取入端口某优先级的buffer占用（InPort视图）
     * @param inPort 入端口ID
     * @param priority 优先级
     * @return 字节数
     */
    uint64_t GetInPortBufferUsed(uint32_t inPort, uint32_t priority);
    
    /**
     * @brief 获取入端口所有优先级的总buffer占用
     */
    uint64_t GetTotalInPortBufferUsed(uint32_t inPort);
    
    // ========== 查询接口：OutPort视图（用于路由和拥塞控制） ==========
    
    /**
     * @brief 获取出端口某优先级的buffer占用（OutPort视图）
     * @param outPort 出端口ID
     * @param priority 优先级
     * @return 字节数
     */
    uint64_t GetOutPortBufferUsed(uint32_t outPort, uint32_t priority);
    
    /**
     * @brief 获取出端口所有优先级的总buffer占用
     */
    uint64_t GetTotalOutPortBufferUsed(uint32_t outPort);
    
    /**
     * @brief 获取每个(port, priority)队列的buffer大小限制（用于丢包判断）
     */
    uint32_t GetBufferSizePerQueue() const { return m_inPortPriorityBufferLimit; }
    
    void SetBufferSize(uint32_t size);

private:
    using DarrayU64 = std::vector<std::vector<uint64_t>>;
    uint32_t m_vlNum = 0;
    uint32_t m_portsNum = 0;
    uint32_t m_inPortPriorityBufferLimit;  // InPort buffer limit per (inPort, priority) queue (for drop decision)
    
    // 双视图统计（同一个包在VOQ中，但从两个维度统计）
    DarrayU64 m_inPortBuffer;   // [inPort][priority] - 用于流控
    DarrayU64 m_outPortBuffer;  // [outPort][priority] - 用于路由和拥塞控制
};
} // namespace ns3

#endif /* UB_BUFFER_MANAGE_H */