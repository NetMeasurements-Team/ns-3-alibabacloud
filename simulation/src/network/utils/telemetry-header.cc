#include "telemetry-header.h"
#include "ns3/log.h"

NS_LOG_COMPONENT_DEFINE ("TelemetryHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (TelemetryHeader);

TelemetryHeader::TelemetryHeader ()
  : m_nextHeader (0), m_numFields (0)
{
  for (uint32_t i = 0; i < maxHops; i++)
    m_idx[i] = NO_INDEX;
}

void TelemetryHeader::SetNextHeader (uint8_t nextHeader)
{
  m_nextHeader = nextHeader;
}

uint8_t TelemetryHeader::GetNextHeader (void) const
{
  return m_nextHeader;
}

void TelemetryHeader::SetFields (const std::vector<uint16_t> &idx)
{
  NS_ASSERT_MSG (idx.size () <= maxHops, "TelemetryHeader: path longer than maxHops");
  m_numFields = static_cast<uint8_t> (idx.size ());
  for (uint32_t i = 0; i < maxHops; i++)
    m_idx[i] = i < idx.size () ? idx[i] : NO_INDEX;
}

void TelemetryHeader::SetNumFields (uint8_t numFields)
{
  NS_ASSERT_MSG (numFields <= maxHops, "TelemetryHeader: path longer than maxHops");
  m_numFields = numFields;
}

uint8_t TelemetryHeader::GetNumFields (void) const
{
  return m_numFields;
}

void TelemetryHeader::SetField (uint8_t hop, uint16_t index)
{
  NS_ASSERT (hop < maxHops);
  m_idx[hop] = index;
}

uint16_t TelemetryHeader::GetField (uint8_t hop) const
{
  NS_ASSERT (hop < maxHops);
  return m_idx[hop];
}

TypeId
TelemetryHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TelemetryHeader")
    .SetParent<Header> ()
    .SetGroupName ("Network")
    .AddConstructor<TelemetryHeader> ()
    ;
  return tid;
}

TypeId
TelemetryHeader::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}

void TelemetryHeader::Print (std::ostream &os) const
{
  os << "tel nextHeader=" << (uint32_t) m_nextHeader
     << " numFields=" << (uint32_t) m_numFields
     << " idx=[";
  for (uint32_t j = 0; j < m_numFields; j++)
    {
      if (j)
        os << ",";
      if (m_idx[j] == NO_INDEX)
        os << "-";
      else
        os << m_idx[j];
    }
  os << "]";
}

uint32_t TelemetryHeader::GetStaticSize (void)
{
  return 2 + maxHops * 2;
}

uint32_t TelemetryHeader::GetSerializedSize (void) const
{
  return GetStaticSize ();
}

void TelemetryHeader::Serialize (Buffer::Iterator start) const
{
  Buffer::Iterator i = start;
  i.WriteU8 (m_nextHeader);
  i.WriteU8 (m_numFields);
  for (uint32_t j = 0; j < maxHops; j++)
    i.WriteU16 (m_idx[j]);
}

uint32_t TelemetryHeader::Deserialize (Buffer::Iterator start)
{
  Buffer::Iterator i = start;
  m_nextHeader = i.ReadU8 ();
  m_numFields = i.ReadU8 ();
  for (uint32_t j = 0; j < maxHops; j++)
    m_idx[j] = i.ReadU16 ();
  return GetSerializedSize ();
}

uint16_t TelemetryHeader::ReadFieldInPlace (const uint8_t* telBytes, uint8_t hop)
{
  NS_ASSERT (hop < maxHops);
  const uint32_t off = fieldsOffset + 2u * hop;
  // Little-endian, matching Buffer::Iterator::WriteU16.
  return static_cast<uint16_t> (telBytes[off]) |
         (static_cast<uint16_t> (telBytes[off + 1]) << 8);
}

void TelemetryHeader::WriteFieldInPlace (uint8_t* telBytes, uint8_t hop, uint16_t index)
{
  NS_ASSERT (hop < maxHops);
  const uint32_t off = fieldsOffset + 2u * hop;
  telBytes[off] = static_cast<uint8_t> (index & 0xff);
  telBytes[off + 1] = static_cast<uint8_t> (index >> 8);
}

} // namespace ns3
