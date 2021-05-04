// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "dispatchQueue/dispatchQueue.h"
#include "future/details/ifuture.h"
#include "object/unknownObject.h"
#include <optional>

namespace Mso {
namespace Futures {

// The FutureState must be limited to 8 states to ensure that it can be fit into three bits.
// The three bits become available because we require that all continuation pointers are aligned by 8 bytes.
// It allows us to store FutureState along with a continuation pointer in the same atomic variable.
// This way we can avoid using mutexes and do the synchronization using only atomic operations.
//
// - FutureState::Pending is the initial state of a Future.
// - FutureState::Posting is when the Future starts processing.
// - FutureState::Posted is when the Future is posted for asynchronous invocation.
// - FutureState::Invoking is set before we start to invoke a task. It helps us ensure that task is invoked only once.
// - FutureState::Awaiting is used when task invocation completed, but we have to wait for the returned future to
//   complete.
// - FutureState::SettingResult is set before we start setting the return value or error code.
// - FutureState::Succeeded is the final state indicating that Future is completed successfully and its value is set.
// - FutureState::Failed is the final state indicating that the execution of the Future is failed and we have an error
//   code.
//
// These are the valid transitions:
//
//                                  ┏━━━━━━━━━━┓            ┏━━━━━━━━━━━━┓
//                              ┌──►┃  Posted  ┃───┐    ┌──►┃  Awaiting  ┃───┐
//                              │   ┗━━━━━━━━━━┛   │    │   ┗━━━━━━━━━━━━┛   │                ╔═════════════╗
//                              │                  ▼    │                    ▼           ┌───►║  Succeeded  ║
//   ┏━━━━━━━━━━━┓        ┏━━━━━━━━━━━┓        ┏━━━━━━━━━━━━┓        ┏━━━━━━━━━━━━━━━┓   │    ╚═════════════╝
//   ┃  Pending  ┃───────►┃  Posting  ┃───────►┃  Invoking  ┃───────►┃ SettingResult ┃───┤
//   ┗━━━━━━━━━━━┛        ┗━━━━━━━━━━━┛        ┗━━━━━━━━━━━━┛        ┗━━━━━━━━━━━━━━━┛   │    ╔═════════════╗
//         │                    │                    │                       ▲           └───►║   Failed    ║
//         ▼                    ▼                    ▼                       │                ╚═════════════╝
//         └────────────────────┴────────────────────┴───────────────────────┘
//
// "Pending" can move to:
// - 'Posting'       - when we have a task to invoke, or we have FutureOptions::UseParentValue where we need to
//                     set a link back to the parent).
// - 'SettingResult' - when there is no task to invoke or when we want to set error code before processing starts.
// "Posting" can move to:
// - 'Invoking'      - when associated task is about to be invoked synchronously in the same thread.
// - 'Posted'        - when task is scheduled for asynchronous execution.
// - 'SettingResult' - when there is no task to invoke or we get an error while posting the task.
// "Posted" can move to:
// - 'Invoking'      - when associated task is about to be invoked as a part of asynchronous execution.
// - 'SettingResult' - when task is canceled before the invocation started.
// "Invoking" can move to:
// - 'Awaiting'      - when task is invoked but we have to wait for returned future to provide result.
// - 'SettingResult' - when task is invoked and we start setting result value or error code.
// "Awaiting" can move to:
// - 'SettingResult' - when returned future is completed we start setting result value or error code.
// "SettingResult" can move to:
// - 'Succeeded'     - when future is succeeded and result value is set.
// - 'Failed'        - when future is failed and error code is set.
//
// The three states: 'Posting', 'Invoking', and 'SettingResult' assume that we are changing other Future variables.
// Getting to these states is similar to acquiring a lock. We set a pointer to the current future in a current
// thread's TLS variable. When we move to a different state we 'release' the lock and clean the TLS variable.
// We can only move to a different state from these three 'locking' states in the same thread where we 'acquired' them.
//
// Promise moves to the FutureState::SettingResult state before we start to set value or error code, and then moves to
// the FutureState::Succeeded or FutureState::Failed states.
//
// Future first moves to FutureState::Posting. If the task must be invoked asynchronously, then it moves
// to FutureState::Post state. After that it moves to the FutureState::Invoking state before task invocation starts.
// It moves to FutureState::SettingResult after the callback finished execution and it needs to set the result value or
// error code. In case when Future's lambda returns another future, which must be completed before returning result, it
// moves to the FutureState::Awaiting state. When the returned future is completed, it moves to
// FutureState::SettingResult state to set value or error code. A Future accepts setting failure from another thread
// while it is not in a 'locking' state: FutureState::Pending, FutureState::Posted, FutureState::Awaiting. In the
// 'locking' states failure can be set only from the thread that currently executes the Future. Future gets to failed
// state when it is canceled, invoked task returns an error code, or previous future failed and our future did not
// handle the error, and this error just propagated to our future.
//
// Promise or Future return value can only be initialized after the state is set to FutureState::SettingResult and then
// to FutureState::Succeeded. Otherwise, we consider the value storage to be uninitialized and do not call the value
// destructor.
//
// For the failures we also first move to FutureState::SettingResult state, set the ErrorCode, and then move to the
// FutureState::Failed state.
//
// Valid transitions for MultiPost mode used for WhenAll and WhenAny are:
//
//                                                 ╔═════════════╗
//                                            ┌───►║  Succeeded  ║
//   ┏━━━━━━━━━━━┓        ┏━━━━━━━━━━━━━━━┓   │    ╚═════════════╝
//   ┃  Pending  ┃───────►┃ SettingResult ┃───┤
//   ┗━━━━━━━━━━━┛        ┗━━━━━━━━━━━━━━━┛   │    ╔═════════════╗
//                                            └───►║   Failed    ║
//                                                 ╚═════════════╝
//
// In MultiPost mode we do only inline invocations. The TaskPost callback must be null.
// The input future are going to provide their results, and then at some point we make a decision to set Success or
// Failure.
//
//
// The Future uses a single memory allocation for its multiple parts.
// The memory allocation for Future data looks as the following:
//
//  ╔═════════════════════╗
//  ║                     ║
//  ║    FutureWeakRef    ║  - contains strong and weak ref counters. It is in the 'negative' offset from
//  ║                     ║    the Future pointer.
//  ╟─────────────────────╢
//  ║                     ║
//  ║     FutureImpl      ║  - implements the core Future functionality that includes state management. This is the
//  ║                     ║    section that we return as a pointer to the Future.
//  ╟─────────────────────╢
//  ║                     ║
//  ║   FutureCallback    ║  - implements IVoidFunctor to pass to executor (only present if traits.TaskPost is not null)
//  ║                     ║
//  ╟─────────────────────╢
//  ║                     ║
//  ║        Value        ║  - contains result value (only present if value size is not zero)
//  ║                     ║
//  ╟─────────────────────╢
//  ║                     ║
//  ║        Task         ║  - contains data for executor and lambda, or any other future type specific data.
//  ║                     ║    (only present if task size is not zero)
//  ╚═════════════════════╝
//

enum class FutureState
{
  Pending, // Future execution is not started.
  Posting, // Future execution is being started.
  Posted, // Future execution is scheduled for asynchronous execution.
  Invoking, // Future task is being invoked.
  Awaiting, // Awaiting for asynchronous task completion.
  SettingResult, // Setting value or error code.
  Succeeded, // Future is succeeded and it has a value.
  Failed, // Future is failed and it has an error code.
};

// Used to indicate expected states as bit flags.
enum class ExpectedStates
{
  None = 0,
  Pending = 1 << static_cast<int>(FutureState::Pending),
  Posting = 1 << static_cast<int>(FutureState::Posting),
  Posted = 1 << static_cast<int>(FutureState::Posted),
  Invoking = 1 << static_cast<int>(FutureState::Invoking),
  Awaiting = 1 << static_cast<int>(FutureState::Awaiting),
  SettingResult = 1 << static_cast<int>(FutureState::SettingResult),
  Succeeded = 1 << static_cast<int>(FutureState::Succeeded),
  Failed = 1 << static_cast<int>(FutureState::Failed),
};

constexpr ExpectedStates operator|(ExpectedStates left, ExpectedStates right) noexcept
{
  return static_cast<ExpectedStates>(static_cast<int>(left) | static_cast<int>(right));
}

class FutureImpl;

// FutureData packs FutureState and a pointer to continuation into one atomic variable.
// It must be trivially copyable to be used with std::atomic.
// It means that we must not have constructors or destructors, and this is why it has a public Value field.
//
// We assume that the continuation address is always aligned by 8 bytes that
// allows us storing the FutureState in the lowest three bits of the address.
//
// We have tried to use shifts instead of masks, but in that case the address in mangled too much,
// and it causes issues with memory tracing tools that look for memory leaks.
//
// To get the FutureState, we apply the StateMask to the Value.
// To get the continuation address, we apply the ContinuationMask to the Value.
struct FuturePackedData
{
  FutureState GetState() const noexcept;
  FutureImpl* GetContinuation() noexcept;
  bool IsDone() const noexcept;
  bool IsSucceded() const noexcept;
  bool IsFailed() const noexcept;

  static FuturePackedData Make(FutureState state) noexcept;
  static FuturePackedData Make(FutureState state, const FutureImpl* continuation) noexcept;

  static void VerifyAlignment(const FutureImpl* continuation) noexcept;

  constexpr static const uintptr_t StateMask = 0b0111; // Last three bits.
  constexpr static const uintptr_t ContinuationMask = ~StateMask; // All bits except the last three.

  // A fake pointer to indicate that the continuation was already invoked. Must be aligned by 8.
  static const FutureImpl* const ContinuationInvoked;

public:
  uintptr_t Value;
};

// Instance of FutureCallback is given to Post method.
// This class has a different lifetime comparing with FutureImpl because we want to react
// to situations when Executor did not call Invoke or OnCancel. In such case we cancel the Future.
// The FutureCallback also ensures that only one Invoke or OnCancel method is called and it is called only once.
struct FutureCallback final : QueryCastList<QueryCastHidden<IVoidFunctor>, ICancellationListener>
{
  FutureCallback() noexcept;
  virtual ~FutureCallback() noexcept;

  FutureCallback(const FutureCallback&) = delete;
  FutureCallback& operator=(const FutureCallback&) = delete;

  static FutureImpl* GetFutureImpl(_In_ FutureCallback* callback) noexcept;

public: // IVoidFunctor
  void Invoke() noexcept override;

public: // ICancellationListener
  void OnCancel() noexcept override;

public: // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(GUID const& riid, _COM_Outptr_ void** ppvObject) noexcept override;
  ULONG STDMETHODCALLTYPE AddRef() noexcept override;
  ULONG STDMETHODCALLTYPE Release() noexcept override;

private:
  mutable std::atomic<uint32_t> m_refCount{1};
  mutable std::atomic<bool> m_isCalled{false};
};

MSO_CLASS_GUID(FutureImpl, "0788AA1F-A6C3-4CA2-81CF-AC94A91FA16A")
class FutureImpl final : public Mso::QueryCastList<Mso::QueryCastDerived<FutureImpl>, Mso::QueryCastHidden<IFuture>>
{
public:
  FutureImpl(const FutureTraits& traits, size_t taskSize) noexcept;
  virtual ~FutureImpl() noexcept;

  FutureImpl(const FutureImpl&) = delete;
  FutureImpl& operator=(const FutureImpl&) = delete;

  void Invoke() noexcept;

  // IFuture
  const FutureTraits& GetTraits() const noexcept override;
  ByteArrayView GetTask() noexcept override;
  ByteArrayView GetValue() noexcept override;
  const ErrorCode& GetError() const noexcept override;

  void AddContinuation(Mso::CntPtr<IFuture>&& continuation) noexcept override;

  _Success_(return ) bool TryStartSetValue(
      _Out_ ByteArrayView& valueBuffer,
      _Out_ void** prevThreadFuture,
      bool crashIfFailed = false) noexcept override;
  void Post() noexcept override;
  void StartAwaiting() noexcept override;
  bool TrySetSuccess(_In_ void* lockState, bool crashIfFailed = false) noexcept override;
  bool TrySetError(ErrorCode&& futureError, bool crashIfFailed = false) noexcept override;

  bool IsDone() const noexcept override;
  bool IsSucceeded() const noexcept override;
  bool IsFailed() const noexcept override;

  // IUnknown
  STDMETHOD(QueryInterface)(const GUID& riid, _Outptr_ void** ppvObject) noexcept override;
  STDMETHOD_(ULONG, AddRef)() noexcept override;
  STDMETHOD_(ULONG, Release)() noexcept override;

private:
  static bool IsExpectedState(FutureState state, ExpectedStates expectedStates) noexcept;
  static bool IsLockingState(FutureState state) noexcept;
  static bool IsFinalState(FutureState state) noexcept;

  std::optional<FutureState>
  TrySetState(FutureState newState, ExpectedStates expectedStates, FutureImpl** continuation = nullptr) noexcept;
  std::optional<FutureState> TrySetState(FutureState newState, FutureImpl** continuation = nullptr) noexcept;
  ExpectedStates GetExtectedStates(FutureState newState) noexcept;

  bool TryStartSetError(bool crashIfFailed) noexcept;
  bool TrySetInvoking(bool crashIfFailed = false) noexcept;
  void SetFailed() noexcept;
  bool TrySetPosted() noexcept;

  ByteArrayView GetCallback() noexcept;
  ByteArrayView GetValueInternal() noexcept;

  bool TryPostInternal(FutureImpl* parent, Mso::CntPtr<FutureImpl>& next, bool crashIfFailed = false) noexcept;
  void PostContinuation(Mso::CntPtr<FutureImpl>&& continuation) noexcept;

  void DestroyTask(bool isAfterInvoke) noexcept;

  bool HasThreadAccess() const noexcept;
  static bool UnexpectedState(FutureState state, bool crashIfFailed, const char* errorMessage, uint32_t tag) noexcept;
  bool IsVoidValue() const noexcept;
  bool HasContinuation() const noexcept;

  friend FutureCallback;

private:
  //
  // The internal fields of the FutureImpl.
  // It is very important that we pay close attention to the thread safety of these fields.
  // Each field employs its own strategy for the thread-safe access.
  //

  // A read-only reference to static memory with a representation of pseudo-virtual table for value and task types.
  // The FutureTraits allow us to reduce size of the compiled code by avoiding duplicated function as we would have with
  // a real v-table. The field is set in the constructor and is never changed.
  const FutureTraits& m_traits;

  // We pack in the same atomic value the state and a continuation.
  // We change the field using atomic operations that makes it safe to change from any thread.
  // Some of the states long with a current FutureImpl stored in TLS play a role of the lock when we need to provide
  // an exclusive access to some variables below. We refer to them as the 'virtual lock'.
  std::atomic<FuturePackedData> m_stateAndContinuation{{0}};

  // m_link is either used for multiple continuations to form a single linked list, or point to a parent FutureImpl
  // during task invocation. The parent FutureImpl has the input value for the task being invoked. During lambda
  // invocation we do not maintain the list of continuations. This is why we can re-use the same field for two different
  // modes: keeping the continuation graph and invoking continuation tasks.
  // Due to the nature of the variable, it is one of the most difficult one to implement the thread-safe access.
  // We use the following rules:
  // - The future can read and write value of the m_link only under the virtual lock.
  // - Otherwise, the parent future can change it only in two cases:
  //   - to establish the ownership while adding the future as the continuation;
  //   - to remove the continuation after the future is done and reached a final state.
  Mso::CntPtr<FutureImpl> m_link;

  // The error code for a failed promise or future.
  // This variable is initialized with the default nullptr in constructor.
  // It can be set when we transition from the FutureState::SettingResult to FutureState::Failed under a virtual lock.
  // It can be safely read only after we get to FutureState::Failed or FutureState::Succeeded final states because it
  // remains read-only until it is destroyed in the destructor.
  ErrorCode m_error;

  // Size of the task associated with the Future. We set it to 0 after we free the task on completion to release
  // its resources earlier, and then to avoid the duplicated destruction in the FutureImpl destructor which we do when
  // the task is never run.
  // This variable is initialized in constructor. It must be read and written only under the virtual lock. The
  // destructor can change it without the virtual lock because no other threads have access to it.
  size_t m_taskSize{0};
};

// Future ref count component that supports weak ref count
// It is initialized in memory right before the FutureImpl instance.
class FutureWeakRef
{
public:
  void AddRef() const noexcept;

  bool Release() const noexcept;

  void AddWeakRef() const noexcept;

  void ReleaseWeakRef() const noexcept;

  bool IsExpired() const noexcept;

  bool IncrementRefCountIfNotZero() noexcept;

private:
  mutable std::atomic<uint32_t> m_refCount{1};
  mutable std::atomic<uint32_t> m_weakRefCount{1}; // Controls FutureWeakRef lifetime.
};

} // namespace Futures

Mso::Futures::FutureWeakRef* GetFutureWeakRef(const void* ptr) noexcept;

} // namespace Mso
