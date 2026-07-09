#include "sr-header.h"
#include "ns3/log.h"

NS_LOG_COMPONENT_DEFINE ("SrHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SrHeader);

SrHeader::SrHeader ()
  : m_nextHeader (0), m_numSegments (0), m_ptr (0)
{
  for (uint32_t i = 0; i < maxSegments; i++)
    m_segs[i] = 0;
}

void SrHeader::SetNextHeader (uint8_t nextHeader)
{
  m_nextHeader = nextHeader;
}

uint8_t SrHeader::GetNextHeader (void) const
{
  return m_nextHeader;
}

void SrHeader::SetPtr (uint8_t ptr)
{
  m_ptr = ptr;
}

uint8_t SrHeader::GetPtr (void) const
{
  return m_ptr;
}

void SrHeader::SetSegments (const std::vector<uint16_t> &segs)
{
  NS_ASSERT_MSG (segs.size () <= maxSegments, "SrHeader: path longer than maxSegments");
  m_numSegments = static_cast<uint8_t> (segs.size ());
  for (uint32_t i = 0; i < maxSegments; i++)
    m_segs[i] = i < segs.size () ? segs[i] : 0;
}

uint8_t SrHeader::GetNumSegments (void) const
{
  return m_numSegments;
}

uint16_t SrHeader::GetSegment (uint8_t idx) const
{
  NS_ASSERT (idx < maxSegments);
  return m_segs[idx];
}

TypeId
SrHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SrHeader")
    .SetParent<Header> ()
    .SetGroupName ("Network")
    .AddConstructor<SrHeader> ()
    ;
  return tid;
}

TypeId
SrHeader::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}

void SrHeader::Print (std::ostream &os) const
{
  os << "srh nextHeader=" << (uint32_t) m_nextHeader
     << " ptr=" << (uint32_t) m_ptr
     << " numSegments=" << (uint32_t) m_numSegments;
}

uint32_t SrHeader::GetStaticSize (void)
{
  return 3 + maxSegments * 2;
}

uint32_t SrHeader::GetSerializedSize (void) const
{
  return GetStaticSize ();
}

void SrHeader::Serialize (Buffer::Iterator start) const
{
  Buffer::Iterator i = start;
  i.WriteU8 (m_nextHeader);
  i.WriteU8 (m_numSegments);
  i.WriteU8 (m_ptr);
  for (uint32_t j = 0; j < maxSegments; j++)
    i.WriteU16 (m_segs[j]);
}

uint32_t SrHeader::Deserialize (Buffer::Iterator start)
{
  Buffer::Iterator i = start;
  m_nextHeader = i.ReadU8 ();
  m_numSegments = i.ReadU8 ();
  m_ptr = i.ReadU8 ();
  for (uint32_t j = 0; j < maxSegments; j++)
    m_segs[j] = i.ReadU16 ();
  return GetSerializedSize ();
}

void SrHeader::AdvancePtrInPlace (uint8_t* srhBytes)
{
  srhBytes[ptrOffset] += 1;
}

} // namespace ns3
