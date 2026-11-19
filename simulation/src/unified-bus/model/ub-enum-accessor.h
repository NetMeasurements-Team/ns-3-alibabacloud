// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_ENUM_ACCESSOR_H
#define UB_ENUM_ACCESSOR_H

#include "ns3/attribute-accessor-helper.h"
#include "ns3/enum.h"

#include <string>
#include <utility>

namespace ns3 {

/**
 * Attribute accessor for scoped enums (enum class).
 *
 * ns-3.36's MakeEnumAccessor stores the value as an int and assigns it back
 * with an implicit conversion, which does not compile for enum class. This
 * accessor keeps EnumValue as the underlying AttributeValue so Config and the
 * default-value machinery keep working, but performs explicit casts.
 */
template <typename EnumT, typename T>
class UbEnumMemberAccessor : public AccessorHelper<T, EnumValue>
{
  public:
    explicit UbEnumMemberAccessor(EnumT T::*member) : m_member(member) {}

    bool HasGetter() const override { return true; }
    bool HasSetter() const override { return true; }

  private:
    bool DoSet(T* object, const EnumValue* value) const override
    {
        (object->*m_member) = static_cast<EnumT>(value->Get());
        return true;
    }

    bool DoGet(const T* object, EnumValue* value) const override
    {
        value->Set(static_cast<int>(object->*m_member));
        return true;
    }

    EnumT T::*m_member;
};

template <typename EnumT, typename T>
Ptr<const AttributeAccessor> MakeUbEnumAccessor(EnumT T::*member)
{
    return Ptr<const AttributeAccessor>(new UbEnumMemberAccessor<EnumT, T>(member), false);
}

template <typename EnumT, typename T>
class UbEnumFunctorAccessor : public AccessorHelper<T, EnumValue>
{
  public:
    UbEnumFunctorAccessor(void (T::*setter)(EnumT), EnumT (T::*getter)() const)
        : m_setter(setter), m_getter(getter)
    {
    }

    bool HasGetter() const override { return m_getter != nullptr; }
    bool HasSetter() const override { return m_setter != nullptr; }

  private:
    bool DoSet(T* object, const EnumValue* value) const override
    {
        (object->*m_setter)(static_cast<EnumT>(value->Get()));
        return true;
    }

    bool DoGet(const T* object, EnumValue* value) const override
    {
        value->Set(static_cast<int>((object->*m_getter)()));
        return true;
    }

    void (T::*m_setter)(EnumT);
    EnumT (T::*m_getter)() const;
};

template <typename EnumT, typename T>
Ptr<const AttributeAccessor> MakeUbEnumAccessor(void (T::*setter)(EnumT),
                                                EnumT (T::*getter)() const)
{
    return Ptr<const AttributeAccessor>(new UbEnumFunctorAccessor<EnumT, T>(setter, getter), false);
}

template <typename EnumT>
EnumValue MakeUbEnumValue(EnumT value)
{
    return EnumValue(static_cast<int>(value));
}

namespace ub_enum_detail {

inline void AppendEnumCheckerValues(Ptr<EnumChecker>&)
{
}

template <typename EnumT, typename... Rest>
void AppendEnumCheckerValues(Ptr<EnumChecker>& checker, EnumT value, std::string name, Rest... rest)
{
    checker->Add(static_cast<int>(value), std::move(name));
    AppendEnumCheckerValues(checker, rest...);
}

} // namespace ub_enum_detail

template <typename EnumT, typename... Rest>
Ptr<const AttributeChecker> MakeUbEnumChecker(EnumT value, std::string name, Rest... rest)
{
    Ptr<EnumChecker> checker = Create<EnumChecker>();
    checker->AddDefault(static_cast<int>(value), std::move(name));
    ub_enum_detail::AppendEnumCheckerValues(checker, rest...);
    return checker;
}

} // namespace ns3

#endif // UB_ENUM_ACCESSOR_H
