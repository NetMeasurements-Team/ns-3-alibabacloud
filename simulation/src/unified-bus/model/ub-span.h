// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_SPAN_H
#define UB_SPAN_H

#include <cstddef>
#include <type_traits>
#include <vector>

namespace ns3 {

/**
 * Minimal C++17 stand-in for std::span.
 *
 * The Unified Bus routing code only needs default construction, construction
 * from a std::vector, and read-only iteration/indexing. Keeping this shim
 * avoids raising the whole ns-3.36 build to C++20 just for those uses.
 */
template <typename T>
class UbSpan
{
  public:
    using element_type = T;
    using value_type = typename std::remove_cv<T>::type;
    using pointer = T*;
    using reference = T&;
    using iterator = T*;
    using const_iterator = T*;
    using size_type = std::size_t;

    UbSpan() = default;
    UbSpan(pointer data, size_type size) : m_data(data), m_size(size) {}

    template <typename Allocator>
    UbSpan(const std::vector<value_type, Allocator>& values)
        : m_data(values.data()), m_size(values.size())
    {
    }

    pointer data() const { return m_data; }
    size_type size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    iterator begin() const { return m_data; }
    iterator end() const { return m_data + m_size; }
    reference operator[](size_type index) const { return m_data[index]; }

  private:
    pointer m_data{nullptr};
    size_type m_size{0};
};

} // namespace ns3

#endif // UB_SPAN_H
