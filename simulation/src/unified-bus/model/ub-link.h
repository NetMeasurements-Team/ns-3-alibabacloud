// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LINK_H
#define UB_LINK_H

#include <list>
#include "ns3/channel.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/ptr.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/traced-callback.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/ub-port.h"

namespace ns3 {

class UbPort;
class Packet;

/**
 * @ingroup point-to-point
 * @brief Simple Point To Point link.
 *
 * This class represents a very simple ub link.  Think full
 * duplex RS-232 or RS-422 with null modem and no handshaking.  There is no
 * multi-drop capability on this link -- there can be a maximum of two
 * ub devices connected.
 *
 * There are two "wires" in the link.  The first device connected gets the
 * [0] wire to transmit on.  The second device gets the [1] wire.  There is a
 * state (IDLE, TRANSMITTING) associated with each wire.
 */
class UbLink : public PointToPointChannel {
public:
    static TypeId GetTypeId(void);

    // Bring base-class overloads of TransmitStart into scope to avoid hiding warnings
    using PointToPointChannel::TransmitStart;

    /**
     * @brief Create a UbLink
     *
     * By default, you get a link that
     * has zero transmission delay.
     */
    UbLink();

    /**
     * @brief Attach a given netdevice to this link
     * @param device pointer to the ubdevice to attach to the link
     */
    virtual void Attach(Ptr<UbPort> device);
    
    /**
    * @brief Transmit a packet over this link (UbPort version)
    * @param p Packet to transmit
    * @param src Source UbPort
    * @param txTime Transmit time to apply
    * @returns true if successful (currently always true)
    */
    virtual bool TransmitStart(Ptr<Packet> p, Ptr<UbPort> src, Time txTime);

    /**
     * @brief Transmit a packet that carries a reliable-rerouting attempt token.
     * @param p Packet to transmit
     * @param src Source UbPort
     * @param txTime Transmit time to apply
     * @param attemptToken Opaque sender-local attempt identity, or zero when the sender retains
     *                     no backup.
     * @returns true if successful (currently always true)
     */
    virtual bool TransmitStart(Ptr<Packet> p, Ptr<UbPort> src, Time txTime, uint64_t attemptToken);

    /**
     * @brief Fail both directions of this link permanently.
     *
     * Fails before mutating the link when the link is not an armed switch-to-switch link, when the
     * runtime does not support the feature, or when another link already failed in this simulation.
     */
    void Fail();

    /** True once both directions of this link are failed. */
    bool IsFailed() const { return m_failed; }

    /** True when this link endpoint pair was armed for reliable rerouting. */
    bool IsReliableReroutingArmed() const { return m_reroutingArmed; }

    Time GetFailureDetectionDelay() const { return m_failureDetectionDelay; }
    Time GetLastAckTimeout() const { return m_lastAckTimeout; }

    virtual bool IsRemote(void) const;

    /**
     * @brief Get number of devices on this link
     * @returns number of devices on this link
     */
    virtual size_t GetNDevices(void) const;

    /**
     * @brief Get UbPort corresponding to index i on this link
     * @param i Index number of the device requested
     * @returns Ptr to UbPort requested
     */
    Ptr<UbPort> GetUbPort(uint32_t i) const;

    /**
     * @brief Get NetDevice corresponding to index i on this link
     * @param i Index number of the device requested
     * @returns Ptr to NetDevice requested
     */
    virtual Ptr<NetDevice> GetDevice(std::size_t i) const;

    /**
     * @brief Get the delay associated with this channel
     * @returns Time delay
     */
    Time GetDelay(void) const;

    Ptr<UbPort> GetDestination(Ptr<UbPort> src) const;

    /**
     * @brief Deliver a serialized packet to the peer port unless this link failed first.
     *
     * Opaque gating only: the link never inspects the packet. A suppressed delivery is retained
     * recovery work on the sender, not a runtime drop.
     */
    void Deliver(Ptr<UbPort> dst, Ptr<UbPort> src, Ptr<Packet> p, uint64_t attemptToken);

protected:
    /**
     * @brief Check to make sure the link is initialized
     * @returns true if initialized, asserts otherwise
     */
    bool IsInitialized(void) const;

    /**
     * @brief Get the net-device source
     * @param i the link requested
     * @returns Ptr to UbPort source for the
     * specified link
     */
    Ptr<UbPort> GetSource(uint32_t i) const;

    /**
     * @brief Get the net-device destination
     * @param i the link requested
     * @returns Ptr to UbPort destination for
     * the specified link
     */
    Ptr<UbPort> GetDestination(uint32_t i) const;

private:
    // Each link has exactly two net devices
    static const int nDevices = 2;

    Time m_delay;
    int32_t m_nDevices;
    bool m_failed{false};
    bool m_enableReliableRerouting{false};
    bool m_reroutingArmed{false};
    Time m_failureDetectionDelay{Time(0)};
    Time m_lastAckTimeout{Time(0)};
    // Immutable armed snapshot: recovery configuration may not change after traffic starts.
    Time m_armedFailureDetectionDelay{Time(0)};
    Time m_armedLastAckTimeout{Time(0)};

    // One-link-per-simulation guard. The raw pointer is only used for identity comparison and is
    // released at Simulator::Destroy(), so it never outlives the simulation it belongs to.
    static UbLink* s_failedLink;
    static bool s_guardReleaseScheduled;

    void ArmEndpointsIfEligible();
    static void AbortIfUnsupportedRuntime(Ptr<UbPort> port);
    static void ReleaseSimulationFailureGuard();

    /** @brief Wire states
     *
     */
    enum class WireState {
        /** Initializing state */
        INITIALIZING,
        /** Idle state (no transmission from NetDevice) */
        IDLE,
        /** Transmitting state (data being transmitted from NetDevice. */
        TRANSMITTING,
        /** Propagating state (data is being propagated in the link. */
        PROPAGATING
    };

    class Link {
    public:
        Link() : m_state(WireState::INITIALIZING), m_src(0), m_dst(0) {}
        WireState m_state;
        Ptr<UbPort> m_src;
        Ptr<UbPort> m_dst;
    };

    Link m_link[nDevices];
};

} // namespace ns3

#endif /* UB_LINK_H */
