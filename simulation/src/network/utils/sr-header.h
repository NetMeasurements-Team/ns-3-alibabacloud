#ifndef SR_HEADER_H
#define SR_HEADER_H

#include <stdint.h>
#include <vector>
#include "ns3/header.h"
#include "ns3/buffer.h"

namespace ns3 {

/**
 * \brief Source Routing Header (SRH).
 *
 * Sits between the IPv4 header and the real L4 header, the same way an
 * IPv6 Segment Routing Header sits between the IPv6 header and its payload:
 * the outer IPv4 protocol field is set to SrHeader::PROTO_NUMBER (0x2B,
 * IPv6's real Routing Header protocol number) and this header's own
 * m_nextHeader field carries the true L4 protocol (0x11 UDP, 0xFC/0xFD
 * ACK/NACK, ...).
 *
 * Each segment is a node id (switch or, for the final segment, the
 * destination host) -- the direct SRv6 analogue of a segment address. A
 * source-routing-aware switch resolves segs[ptr] to one of its own local
 * ports via a small per-switch node-id -> port table (SwitchNode/
 * NVSwitchNode::m_srNextHop, populated from the topology's adjacency at
 * setup time), then forwards out that port. The segment list is always
 * serialized at its fixed maximum size (like IntHeader::hop[]), so
 * GetSerializedSize() is a compile-time constant and CustomHeader's offset
 * math never needs to branch on path length.
 */
class SrHeader : public Header
{
public:
  static const uint8_t PROTO_NUMBER = 0x2B;
  static const uint32_t maxSegments = 10;

  // Byte offset of the `ptr` field within the serialized SRH, used by the
  // raw-buffer helpers below.
  static const uint32_t ptrOffset = 2;
  static const uint32_t segsOffset = 3;

  SrHeader ();

  // Constant size (3 + maxSegments*2 bytes): the segment list is always
  // serialized at full width, mirroring IntHeader::GetStaticSize()'s
  // convention, so callers can compute offsets without an instance.
  static uint32_t GetStaticSize (void);

  void SetNextHeader (uint8_t nextHeader);
  uint8_t GetNextHeader (void) const;

  void SetPtr (uint8_t ptr);
  uint8_t GetPtr (void) const;

  // segs.size() must be <= maxSegments.
  void SetSegments (const std::vector<uint16_t> &segs);
  uint8_t GetNumSegments (void) const;
  uint16_t GetSegment (uint8_t idx) const;

  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual void Print (std::ostream &os) const;
  virtual uint32_t GetSerializedSize (void) const;
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);

  /**
   * Direct raw-buffer accessors for the switch forwarding fast path, in the
   * same spirit as IntHeader::PushHop: switches never construct an SrHeader
   * object to advance the pointer, they just poke the one byte that
   * changed directly into the packet's buffer. `srhBytes` must point at
   * the start of the serialized SRH within the packet (i.e.
   * PppHeader::GetStaticSize() + 20 bytes into the buffer, brief IPv4 with
   * no options).
   *
   * NOTE: unlike IntHeader, SrHeader derives from ns3::Header and therefore
   * has a vtable — it must never be reinterpret_cast onto wire bytes.
   * These helpers only ever do manual byte indexing, never a struct cast.
   */
  static void AdvancePtrInPlace (uint8_t* srhBytes);

private:
  uint8_t m_nextHeader;
  uint8_t m_numSegments;
  uint8_t m_ptr;
  uint16_t m_segs[maxSegments];
};

} // namespace ns3

#endif /* SR_HEADER_H */
