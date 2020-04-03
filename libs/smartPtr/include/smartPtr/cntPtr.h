// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#ifndef LIBLET_CORE_TCNTPTR_H
#define LIBLET_CORE_TCNTPTR_H

#include <compilerAdapters/declspecDefinitions.h>
#include <crash/verifyElseCrash.h>
#include <debugAssertApi/debugAssertApi.h>
#include <smartPtr/smartPointerBase.h>
#include <atomic>

#ifdef __cplusplus
namespace Mso {

class ObjectWeakRef;

/**
  Simple ref-counting
*/
struct DECLSPEC_NOVTABLE IRefCounted
{
  virtual void AddRef() const noexcept = 0;
  virtual void Release() const noexcept = 0;
};

/**
  Provides weak ref-counting support at interface level: when there is a need to convert
  CntPtr<IFoo> to WeakPtr<IFoo>, deriving IFoo from IWeakRefCounted ensures that there
  is no run-time error tag_bad22 (under condition that implementations of IFoo are derived
  from RefCountedObject<RefCountStrategy::WeakRef, IFoo>).
*/
struct DECLSPEC_NOVTABLE IWeakRefCounted : public IRefCounted
{
  virtual ObjectWeakRef& GetWeakRef() const noexcept = 0;
};

/**
  DEPRECATED: please use Mso::RefCountedObject

  Provides implementation for an interface with AddRef + Release
  MostDerivedInterface is the IRefCounted-derived interface you are implementing.

  e.g.
  interface IMyInterface : public Mso::IRefCounted {  ...   };

  class MyInterfaceImpl : public Mso::TRefCountedImpl< IMyInterface >
  {
    // Override method from IMyInterface
    // No need for implementing IRefCounted methods
  };
*/
template <class... TInterfaces>
class TRefCountedImpl : public TInterfaces...
{
  OACR_MARK_CLASS_DEPRECATED(Mso::RefCountedObject)

public:
  MSO_NO_COPY_CTOR_AND_ASSIGNMENT(TRefCountedImpl);

  virtual void AddRef() const noexcept override
  {
    m_ref++;
  }

  virtual void Release() const noexcept override
  {
    if (m_ref.fetch_sub(1) == 1)
      delete this;
  }

protected:
  TRefCountedImpl() noexcept : m_ref(0) {}

  virtual ~TRefCountedImpl() noexcept {}

  uint32_t GetRefCount() const noexcept
  {
    return m_ref.load();
  }

private:
  mutable std::atomic<uint32_t> m_ref;
#pragma warning(suppress : 4820) // 4 bytes padding added after data member
};

/**
  Manages a pointer cleaned up with Release()
*/
template <typename T>
struct TReleaseHelper
{
  static void Free(T pT) noexcept
  {
    using ArrowType = decltype(Mso::RawTraits<T>::GetArrowType(pT));
    using NonConstArrowType = std::remove_const_t<std::remove_pointer_t<ArrowType>>;
    const_cast<NonConstArrowType*>(Mso::RawTraits<T>::GetArrowType(pT))->Release();
  }
};

template <typename T>
class CntPtr;

// Code in details namespace is for internal usage only
namespace Details {

/// Used for CntPtr operator& implementation.
/// It allows to avoid memory leaks when non-empty CntPtr is used as output parameter T**.
/// For TCntRef class, please find its definition below CntPtr class
template <typename T>
class CntPtrRef
{
  // We only allow CntPtr to make instance of this type
  template <typename U>
  friend class Mso::CntPtr;

public:
  operator Mso::CntPtr<T> *() noexcept
  {
    return m_pTCntPtr;
  }
  operator void*() noexcept
  {
    return m_pTCntPtr;
  }
  operator T* *() noexcept
  {
    return m_pTCntPtr->GetAddressOf();
  }
  operator void* *() noexcept
  {
    return reinterpret_cast<void**>(m_pTCntPtr->GetAddressOf());
  }
  operator const void* *() noexcept
  {
    return reinterpret_cast<const void**>(const_cast<const T**>(m_pTCntPtr->GetAddressOf()));
  }

  // If you are sure it is safe, use static_cast<U**>(&p) to access this operator
  template <typename U, typename = std::enable_if_t<std::is_base_of<U, T>::value>>
  explicit operator U* *() const noexcept
  {
    return reinterpret_cast<U**>(m_pTCntPtr->GetAddressOf());
  }

  T*& operator*() noexcept
  {
    return *(m_pTCntPtr->GetAddressOf());
  }
  T** GetRaw() const noexcept
  {
    return m_pTCntPtr->GetRaw();
  }
  T* const* GetAddressOf() const noexcept
  {
    return m_pTCntPtr->GetAddressOf();
  }
  T** ClearAndGetAddressOf() noexcept
  {
    return m_pTCntPtr->ClearAndGetAddressOf();
  }

private:
  CntPtrRef(_In_ Mso::CntPtr<T>* pT) noexcept : m_pTCntPtr(pT) {}

private:
  Mso::CntPtr<T>* m_pTCntPtr;
};

template <typename T, typename U>
bool operator==(const CntPtrRef<T>& left, const CntPtrRef<U>& right) noexcept
{
  static_assert(
      std::is_base_of<T, U>::value || std::is_base_of<U, T>::value, "'T' and 'U' pointers must be comparable");
  return left.GetRaw() == right.GetRaw();
}

template <typename T, typename U>
bool operator!=(const CntPtrRef<T>& left, const CntPtrRef<U>& right) noexcept
{
  return !(left == right);
}

} // namespace Details

/**
  The TCntPtrAddRefStrategy enumeration and TCntPtrAddRefStrategyForType template allows the
  code to specify how specific types of CntPtr's will perform the add ref operation
*/
enum TCntPtrAddRefStrategy : uint32_t
{
  AddRef = 0,
  RefTrack = 1,
};

/**
  To change the type of RefCount strategy that a particular type of CntPtr will use
  override this general template with one specific to the pointer type
*/
template <typename T>
struct TCntPtrAddRefStrategyForType
{
  static const TCntPtrAddRefStrategy Strategy = TCntPtrAddRefStrategy::AddRef;
};

// Code in details namespace is for internal usage only
namespace Details {

/**
  Implements CheckedAddRef for ref counted interfaces that are not using RefTrack

  Q: What is this strategy only applied to CntPtr and not TCntRef
  A: TCntRef has a Copy method that does AddRef and then returns a reference to the
     object. This is not compatible with RefTrack and so the strategy is not applied.
*/
template <TCntPtrAddRefStrategy ARS>
struct TCntPtrAddRefStrategyImpl
{
  /**
    Safely performs an AddRef on the newly assigned ptr value, this means that the caller does
    not currently have a reference on the supplied value.
  */
  template <typename T>
  static void CheckedAddRefOnNewlyAssignedPtr(_Inout_ _Deref_pre_valid_ _Deref_post_valid_ T** ppT) noexcept
  {
    using TNonConst = typename std::remove_const<T>::type; // For AddRef() and Release() calls

    if (*ppT)
    {
      const_cast<TNonConst*>(*ppT)->AddRef();
    }
  }

  /**
    Depending upon the RefCount strategy used it may be necessary to unwrap the IUnknown before
    it is used for comparing to determine object identity. By default we just use the provided
    IUnknown value.
  */
  template <typename T>
  static T* GetIUnknownForObjectCompare(T* pValue)
  {
    return pValue;
  }
};

/**
  Allows for the AddRefStrategy for the type to be obtained. This is needed when using the
  strategy outside of a tempalte class.
*/
template <typename T>
struct AddRefStrategyForType
{
  using TAddRefStrategy = TCntPtrAddRefStrategyForType<T>;
};

} // namespace Details

/**
  Ref-counted smart pointer, once set always the same, never null.
  Typically used for member variables.
  Methods will assert if TCntRef is used after its content is moved to another TCntRef.
*/
template <typename T>
class TCntRef final
{
public:
  TCntRef() noexcept : m_pT(nullptr) {}

  explicit TCntRef(T& ref, bool fDoAddRef = true) noexcept : m_pT(&ref)
  {
    if (fDoAddRef)
      CheckedAddRef(m_pT);
  }

  template <typename TOther, typename = typename std::enable_if<std::is_base_of<T, TOther>::value>::type>
  explicit TCntRef(TOther& ref, bool fDoAddRef = true) noexcept : m_pT(&ref)
  {
    if (fDoAddRef)
      CheckedAddRef(m_pT);
  }

  TCntRef(const TCntRef& other) noexcept : m_pT(other.Ptr())
  {
    CheckedAddRef(m_pT);
  }

  template <typename TOther, typename = typename std::enable_if<std::is_base_of<T, TOther>::value>::type>
  TCntRef(const Mso::TCntRef<TOther>& other) noexcept : m_pT(other.Ptr())
  {
    CheckedAddRef(m_pT);
  }

  TCntRef(TCntRef&& other) noexcept : m_pT(other.Ptr())
  {
    other.m_pT = nullptr;
  }

  template <typename TOther, typename = typename std::enable_if<std::is_base_of<T, TOther>::value>::type>
  TCntRef(Mso::TCntRef<TOther>&& other) noexcept : m_pT(other.Ptr())
  {
    other.m_pT = nullptr;
  }

  TCntRef& operator=(const TCntRef& other) = delete;
  TCntRef& operator=(TCntRef&& other) = delete;

  ~TCntRef() noexcept
  {
    Clear();
  }

  bool IsEmpty() const noexcept
  {
    return m_pT == nullptr;
  }

  T* Ptr() const noexcept
  {
    AssertTag(m_pT, 0x0152139d /* tag_bu7o3 */);
    return const_cast<TCntRef*>(this)->m_pT;
  }

  T& Get() const noexcept
  {
    VerifyElseCrashTag(m_pT, 0x0152139e /* tag_bu7o4 */);
    return *const_cast<TCntRef*>(this)->m_pT;
  }

#ifndef MSO_THOLDER_EXPLICIT_GET_ONLY
  /// Implicit cast operator
  /*_SA_deprecated_(Get)*/ operator T&() const noexcept
  {
    return Get();
  }
#endif

  /// Returns an AddRef'd raw reference
  T& Copy() const noexcept
  {
    T& ref = Get();
    CheckedAddRef(&ref);
    return ref;
  }

  /// Copy to a naked pointer and AddRef
  template <typename TOther>
  void AssignTo(_Out_opt_ TOther* ppT) const noexcept
  {
    AssertTag(ppT != nullptr, 0x0152139f /* tag_bu7o5 */);
    if (ppT != nullptr)
      *ppT = Copy();
  }

#ifdef MSO_THOLDER_EXPLICIT_GET_ONLY
  explicit operator bool() const noexcept
  {
    return !IsEmpty();
  }
#endif

  T* operator->() const noexcept
  {
    VerifyElseCrashTag(m_pT, 0x015213a0 /* tag_bu7o6 */);
    return const_cast<TCntRef*>(this)->m_pT;
  }

private:
  using TNonConst = typename std::remove_const<T>::type;

  static void CheckedAddRef(T* pT) noexcept
  {
    if (pT)
    {
      const_cast<TNonConst*>(pT)->AddRef();
    }
  }

  void Clear() noexcept
  {
    if (m_pT)
    {
      TNonConst* pT = const_cast<TNonConst*>(m_pT);
      m_pT = nullptr;
      (void)pT; // To avoid compilation warnings
      pT->Release();
    }
  }

private:
  T* m_pT;

  template <typename TOther>
  friend class TCntRef;
};

#ifdef MSO_THOLDER_EXPLICIT_GET_ONLY
/**
  Operators for TCntRef
*/

template <typename T1, typename T2>
bool operator==(const TCntRef<T1>& left, const TCntRef<T2>& right) noexcept
{
  return (&left.Get() == &right.Get());
}

template <typename T1, typename T2>
bool operator!=(const TCntRef<T1>& left, const TCntRef<T2>& right) noexcept
{
  return !(left == right);
}
#endif // MSO_THOLDER_EXPLICIT_GET_ONLY

/**
  Ref-counted smart pointer, possibly null
*/
template <typename T>
class CntPtr
{
  using TNonConst = typename std::remove_const<T>::type; // For AddRef() and Release() calls
  using TAddrType = T**;
  using TAddRefStrategy = TCntPtrAddRefStrategyForType<T>;

public:
  /**
    Constructors
  */

  CntPtr() noexcept : m_pT(nullptr) {}
  explicit CntPtr(std::nullptr_t) noexcept : m_pT(nullptr) {}

  // Expressly delete the legacy NULL ctor
  CntPtr(int) = delete;

  // TODO: This should be explicit, but it requires fixing a lot of code
  /*explicit*/ CntPtr(_In_opt_ T* pT, bool fDoAddRef = true) noexcept : m_pT(pT)
  {
    if (fDoAddRef)
      Details::TCntPtrAddRefStrategyImpl<TAddRefStrategy::Strategy>::CheckedAddRefOnNewlyAssignedPtr(&m_pT);
  }

  template <typename TOther, typename = typename std::enable_if<std::is_base_of<T, TOther>::value>::type>
  explicit CntPtr(_In_opt_ TOther* pOther, bool fDoAddRef = true) noexcept : m_pT(pOther)
  {
    if (fDoAddRef)
      Details::TCntPtrAddRefStrategyImpl<TAddRefStrategy::Strategy>::CheckedAddRefOnNewlyAssignedPtr(&m_pT);
  }

  // Delete this constructor. Developers must be explicit in their code when they want to use unsafe conversion from
  // void* or from a base class. Use CntPtr<T>(static_cast<T*>(ptr)) or qi_cast<T>(ptr) instead.
  template <typename TOther, typename = typename std::enable_if<!std::is_base_of<T, TOther>::value>::type>
  explicit CntPtr(_In_opt_ TOther* pOther, bool fDoAddRef = true, int /*doNotUseThisConstructor*/ = 0) = delete;

  CntPtr(const CntPtr& from) noexcept : m_pT(from.Get())
  {
    Details::TCntPtrAddRefStrategyImpl<TAddRefStrategy::Strategy>::CheckedAddRefOnNewlyAssignedPtr(&m_pT);
  }

  template <typename TOther, typename = typename std::enable_if<std::is_base_of<T, TOther>::value>::type>
  CntPtr(const Mso::CntPtr<TOther>& from) noexcept : m_pT(from.Get())
  {
    Details::TCntPtrAddRefStrategyImpl<TAddRefStrategy::Strategy>::CheckedAddRefOnNewlyAssignedPtr(&m_pT);
  }

  CntPtr(CntPtr&& from) noexcept : m_pT(from.Detach()) {}

  template <typename TOther, typename = typename std::enable_if<std::is_base_of<T, TOther>::value>::type>
  CntPtr(Mso::CntPtr<TOther>&& from) noexcept : m_pT(from.Detach())
  {
  }

  template <typename TOther>
  CntPtr(const Mso::TCntRef<TOther>& from) noexcept : CntPtr{from.Ptr()}
  {
  }

  ~CntPtr() noexcept
  {
    Clear();
  }

  /**
    operator =
  */

  CntPtr& operator=(std::nullptr_t) noexcept
  {
    this->Clear();
    return *this;
  }

  template <typename TOther, typename = typename std::enable_if<std::is_base_of<T, TOther>::value>::type>
  CntPtr& operator=(_In_opt_ TOther* pT) noexcept
  {
    if (m_pT != pT)
    {
      T* pNew = pT;
      Details::TCntPtrAddRefStrategyImpl<TAddRefStrategy::Strategy>::CheckedAddRefOnNewlyAssignedPtr(&pNew);

      Clear();
      m_pT = pNew;
    }
    return *this;
  }

  CntPtr& operator=(const CntPtr& from) noexcept
  {
    return operator=(from.Get());
  }

  template <typename TOther, typename = typename std::enable_if<std::is_base_of<T, TOther>::value>::type>
  CntPtr& operator=(const Mso::CntPtr<TOther>& from) noexcept
  {
    return operator=(from.Get());
  }

  CntPtr& operator=(CntPtr&& from) noexcept
  {
    Mso::CntPtr<T>(std::move(from)).Swap(*this);
    return *this;
  }

  template <typename TOther, typename = typename std::enable_if<std::is_base_of<T, TOther>::value>::type>
  CntPtr& operator=(Mso::CntPtr<TOther>&& from) noexcept
  {
    Mso::CntPtr<T>(std::move(from)).Swap(*this);
    return *this;
  }

  /// The rare case that you need a pointer to the smart pointer itself
  const CntPtr* GetThis() const noexcept
  {
    return this;
  }
  CntPtr* GetThis() noexcept
  {
    return this;
  }

  /// Return an AddRef'd raw pointer
  T* Copy() const noexcept
  {
    CntPtr<T> copy(m_pT);
    return copy.Detach();
  }

  /// Explicitly delete owned object
  void Clear() noexcept
  {
    if (m_pT)
    {
      TNonConst* pT = const_cast<TNonConst*>(m_pT);
      m_pT = nullptr;
      (void)pT; // to avoid compilation warnings
      pT->Release();
    }
  }

  bool IsEmpty() const noexcept
  {
    return (m_pT == nullptr);
  }

#ifdef MSO_THOLDER_EXPLICIT_GET_ONLY
  explicit operator bool() const noexcept
  {
    return !IsEmpty();
  }

  T& operator*() const noexcept
  {
    return *Get();
  }
#endif

  T* Get() const noexcept
  {
    return const_cast<CntPtr*>(this)->m_pT;
  }

#ifndef MSO_THOLDER_EXPLICIT_GET_ONLY
  /// Implicit cast operator
  /*_SA_deprecated_(Get)*/ operator T*() const noexcept
  {
    return m_pT;
  }
#endif

  T* operator->() const noexcept
  {
    VerifyElseCrashTag(m_pT, 0x0152139a /* tag_bu7o0 */);
    return const_cast<CntPtr*>(this)->m_pT;
  }

  void Swap(CntPtr& other) noexcept
  {
    T* pT = m_pT;
    m_pT = other.m_pT;
    other.m_pT = pT;
  }

  /// Take object ownership, m_pT must be empty
  T* Place(T* pT) noexcept
  {
    AssertTag(IsEmpty(), 0x0152139b /* tag_bu7o1 */);
    m_pT = pT;
    return m_pT;
  }

  /// Take object ownership, deletes previously owned object if any
  T* Attach(T* pT) noexcept
  {
    Clear();
    m_pT = pT;
    return m_pT;
  }

  /// Release ownership without deleting object
  T* Detach() noexcept
  {
    T* pT = m_pT;
    m_pT = nullptr;
    return pT;
  }

  /// & operator to retrieve object.
  /// Returned CntPtrRef takes care about safe use of the pointer.
  Mso::Details::CntPtrRef<T> operator&() noexcept
  {
    return Mso::Details::CntPtrRef<T>(this);
  }

  /**
    Retrieves the address of the object. Asserts that CntPtr is empty.

    void GetSomething(T** ppT);

    Mso::CntPtr<T> pT;
    GetSomething(pT.GetAddressOf());

    ClearAndGetAddressOf() will ensure any existing object is cleared first.
    GetRaw() avoids the assert if you need the address of an existing object.
  */
  TAddrType GetAddressOf() noexcept
  {
    AssertSzTag(
        IsEmpty(),
        "Getting the address of an existing object? This usually leads to a leak.",
        0x0152139c /* tag_bu7o2 */);
    return GetRaw();
  }

  /**/
  TAddrType ClearAndGetAddressOf() noexcept
  {
    Clear();
    return GetRaw();
  }

  /**/
  TAddrType GetRaw() noexcept
  {
    // This is dangerous so you better know what you are doing
    return &m_pT;
  }

  /**
    Deprecated API to remove over time.
  */
  /*_SA_deprecated_(Get)*/ T* get() const noexcept
  {
    return this->Get();
  }
  /*_SA_deprecated_(Swap)*/ void swap(CntPtr& from) noexcept
  {
    Swap(from);
  }
  /*_SA_deprecated_(Clear)*/ void clear() noexcept
  {
    this->Clear();
  }
  /*_SA_deprecated_(Clear)*/ void Empty() noexcept
  {
    Clear();
  }
  /*_SA_deprecated_(Clear)*/ void Free() noexcept
  {
    Clear();
  }
  /*_SA_deprecated_(Clear)*/ void Close() noexcept
  {
    Clear();
  }
  /*_SA_deprecated_(Clear)*/ void Release() noexcept
  {
    Clear();
  }
  /*_SA_deprecated_(IsEmpty)*/ bool FIsEmpty() const noexcept
  {
    return IsEmpty();
  }
  /*_SA_deprecated_(Detach)*/ T* Extract() noexcept
  {
    return Detach();
  }
  template <typename TOther>
  /*_SA_deprecated_(move assignment)*/ void TransferFrom(_Inout_ CntPtr<TOther>& from) noexcept
  {
    Attach(from.Detach());
  }
  template <typename TOther>
  /*_SA_deprecated_(move assignment)*/ void Transfer(CntPtr<TOther>& from) noexcept
  {
    return TransferFrom(from);
  }
  /*_SA_deprecated_(GetAddressOf)*/ TAddrType Ptr() noexcept
  {
    return this->GetAddressOf();
  }
  /*_SA_deprecated_(ClearAndGetAddressOf)*/ TAddrType Address() noexcept
  {
    return this->ClearAndGetAddressOf();
  }

private:
  T* m_pT;
};

#ifdef MSO_THOLDER_EXPLICIT_GET_ONLY
/**
  Operators for CntPtr
*/

template <typename T1, typename T2>
bool operator==(const CntPtr<T1>& left, const CntPtr<T2>& right) noexcept
{
  return (left.Get() == right.Get());
}

template <typename T>
bool operator==(const CntPtr<T>& left, std::nullptr_t) noexcept
{
  return left.IsEmpty();
}

template <typename T>
bool operator==(std::nullptr_t, const CntPtr<T>& right) noexcept
{
  return right.IsEmpty();
}

template <typename T1, typename T2>
bool operator!=(const CntPtr<T1>& left, const CntPtr<T2>& right) noexcept
{
  return !(left == right);
}

template <typename T>
bool operator!=(const CntPtr<T>& left, std::nullptr_t) noexcept
{
  return !left.IsEmpty();
}

template <typename T>
bool operator!=(std::nullptr_t, const CntPtr<T>& right) noexcept
{
  return !right.IsEmpty();
}
#endif // MSO_THOLDER_EXPLICIT_GET_ONLY

} // namespace Mso
#endif // __cplusplus
#endif // LIBLET_CORE_TCNTPTR_H