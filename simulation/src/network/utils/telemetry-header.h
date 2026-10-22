#ifndef TELEMETRY_HEADER_H
#define TELEMETRY_HEADER_H

#include <stdint.h>
#include <vector>
#include "ns3/header.h"
#include "ns3/buffer.h"
#include "ns3/sr-header.h"

namespace ns3 {

/**
 * \brief Per-hop flow-index telemetry header.
 *
 * Sits between the Source Routing Header and the real L4 header, chaining
 * off it exactly the way the SRH chains off the IPv4 header: the SRH's own
 * nextHeader field carries TelemetryHeader::PROTO_NUMBER, and this header's
 * m_nextHeader field carries the true L4 protocol (0x11 UDP, 0xFC/0xFD
 * ACK/NACK, ...). The full on-wire chain for a source-routed, telemetered
 * packet is therefore
 *
 *     IPv4 (proto = SrHeader::PROTO_NUMBER)
 *       -> SrHeader (nextHeader = TelemetryHeader::PROTO_NUMBER)
 *         -> TelemetryHeader (nextHeader = real L4)
 *           -> UDP / ACK / NACK / ...
 *
 * The SRH must stay immediately after the IPv4 header, because both
 * SwitchNode::SendToDev and SwitchNode::SwitchNotifyDequeue locate it at the
 * hard-coded offset PppHeader::GetStaticSize() + 20. Telemetry therefore goes
 * *after* the SRH, never before it. (Headers are prepended, so the build
 * order in RdmaHw is L4, then telemetry, then SRH, then IP.)
 *
 * Field k holds the index this packet's flow has been assigned at the switch
 * sitting at position k of the packet's *full* path -- the same full path
 * BuildSourceRoute returns, including the first-hop switch that the SRH
 * segment list deliberately omits. That alignment is the whole point of the
 * design: at the switch occupying path position k, the on-wire SRH pointer
 * reads exactly k on ingress (SwitchNode::GetOutDev consults segs[ptr] and
 * only SendToDev advances it afterwards), so a switch reads its own index
 * with a single array access and no search:
 *
 *     uint16_t myIdx = ch.tel.idx[ch.srh.ptr];
 *
 * A field is emitted for *every* path position, including NVSwitch hops and
 * any hop with no assigned index, using the NO_INDEX sentinel. Compacting the
 * list to real switches only would break the ptr-as-field-index identity and
 * force every switch to count how many NVSwitches preceded it. The
 * destination host, which is the last element of the full path, gets no
 * field -- so numFields == path.size() - 1.
 *
 * Like SrHeader (and IntHeader::hop[]) the field list is always serialized at
 * its fixed maximum width, so GetSerializedSize() is a compile-time constant
 * and CustomHeader's offset arithmetic never has to branch on path length.
 * maxHops is deliberately *the same constant* as SrHeader::maxSegments rather
 * than an independent copy, so the two can never drift apart.
 *
 * See claude_reports/TELEMETRY_FLOW_INDEX_PLAN.md.
 */
class TelemetryHeader : public Header
{
public:
  // 0x3C is IPv6's real Destination Options header number, picked for the
  // same reason SrHeader picked 0x2B: it is a genuine extension-header
  // protocol number and collides with nothing this simulator uses as an
  // l3Prot (0x06 TCP, 0x11 UDP, 0x2B SRH, 0xFC ACK, 0xFD NACK, 0xFE PFC,
  // 0xFF CNP).
  static const uint8_t PROTO_NUMBER = 0x3C;

  // Same bound as the SRH's segment list, by construction.
  static const uint32_t maxHops = SrHeader::maxSegments;

  // Emitted for a hop that has no index assigned to this flow: an NVSwitch,
  // or a real switch the presimulation's flow list never associated with
  // this flow. Readers must treat it as "no telemetry for this hop", never
  // as index 65535.
  static const uint16_t NO_INDEX = 0xFFFF;

  // Byte offsets within the serialized header, for the raw-buffer accessors
  // below.
  static const uint32_t nextHeaderOffset = 0;
  static const uint32_t numFieldsOffset = 1;
  static const uint32_t fieldsOffset = 2;

  TelemetryHeader ();

  // Constant size (2 + maxHops*2 bytes): the field list is always serialized
  // at full width, mirroring SrHeader::GetStaticSize().
  static uint32_t GetStaticSize (void);

  void SetNextHeader (uint8_t nextHeader);
  uint8_t GetNextHeader (void) const;

  // idx.size() must be <= maxHops. Slots beyond idx.size() are serialized as
  // NO_INDEX rather than 0, so a truncated or misparsed header can never be
  // mistaken for a run of valid index-0 hops.
  void SetFields (const std::vector<uint16_t> &idx);
  void SetNumFields (uint8_t numFields);
  uint8_t GetNumFields (void) const;
  void SetField (uint8_t hop, uint16_t index);
  uint16_t GetField (uint8_t hop) const;

  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual void Print (std::ostream &os) const;
  virtual uint32_t GetSerializedSize (void) const;
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);

  /**
   * Direct raw-buffer read for the switch fast path, in the same spirit as
   * SrHeader::AdvancePtrInPlace: read one field without constructing a
   * header object. `telBytes` must point at the first byte of the serialized
   * TelemetryHeader within the packet -- i.e.
   * PppHeader::GetStaticSize() + 20 + SrHeader::GetStaticSize() bytes into
   * the buffer (plain IPv4, no options), when an SRH precedes it.
   *
   * NOTE: exactly as with SrHeader, this class derives from ns3::Header and
   * therefore has a vtable -- it must never be reinterpret_cast onto wire
   * bytes. These helpers only ever do manual byte indexing. Fields are
   * little-endian on the wire because Buffer::Iterator::WriteU16 is.
   */
  static uint16_t ReadFieldInPlace (const uint8_t* telBytes, uint8_t hop);
  static void WriteFieldInPlace (uint8_t* telBytes, uint8_t hop, uint16_t index);

private:
  uint8_t  m_nextHeader;
  uint8_t  m_numFields;
  uint16_t m_idx[maxHops];
};

} // namespace ns3

#endif /* TELEMETRY_HEADER_H */
