// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "futureImpl.h"
#include <thread>
#include "eventWaitHandle/eventWaitHandle.h"
#include "future/future.h"

#define CheckFutureStateTag(condition, state, crashIfFailed, errorMessage, tag) \
  Statement(if (!(condition)) { return UnexpectedState(state, crashIfFailed, errorMessage, tag); })

namespace Mso {
namespace Futures {

//=============================================================================
//
// Helper functions
//
//=============================================================================

// Addresses must be aligned at least by 8 byte in order to have the extra 3 bits in the address
// which are used by the Future state. See the usage in the FuturePackedData.
static constexpr const size_t ObjectAlignment = 8;

static bool IsAligned(const void* ptr) noexcept
{
  uintptr_t ptrInt = reinterpret_cast<uintptr_t>(ptr);
  return (ptrInt & (ObjectAlignment - 1)) == 0;
}

static size_t GetAlignedSize(size_t size) noexcept
{
  return (size + ObjectAlignment - 1) & ~(ObjectAlignment - 1);
}

//=============================================================================
//
// Factory functions
//
//=============================================================================

LIBLET_PUBLICAPI Mso::CntPtr<IFuture>
MakeFuture(const FutureTraits& traits, size_t taskSize, _Out_opt_ ByteArrayView* taskBuffer) noexcept
{
  if (IsSet(traits.Options, FutureOptions::UseParentValue))
  {
    VerifyElseCrashSzTag(
        traits.ValueSize == 0,
        "FutureOptions::UseParentValue requires that future does not have its own value.",
        0x016055c1 /* tag_byfxb */);
  }

  if (traits.TaskPost)
  {
    VerifyElseCrashSzTag(
        traits.TaskInvoke != nullptr || traits.TaskCatch != nullptr,
        "TaskInvoke or TaskCatch callback is required if TaskPost callback is present.",
        0x016055c2 /* tag_byfxc */);
  }

  if (IsSet(traits.Options, FutureOptions::IsMultiPost))
  {
    VerifyElseCrashSzTag(
        traits.TaskPost == nullptr, "MultiPost must not have TaskPost callback", 0x016055c3 /* tag_byfxd */);
    VerifyElseCrashSzTag(
        traits.TaskInvoke != nullptr, "MultiPost must have TaskInvoke callback", 0x016055c4 /* tag_byfxe */);
    VerifyElseCrashSzTag(
        traits.TaskCatch != nullptr, "MultiPost must have TaskCatch callback", 0x016055c5 /* tag_byfxf */);
    VerifyElseCrashSzTag(
        !IsSet(traits.Options, FutureOptions::UseParentValue),
        "MultiPost cannot use parent value",
        0x016055c6 /* tag_byfxg */);
  }

  //
  // Allocated memory must have four pieces: FutureWeakRef, FutureImpl, Value, and Task
  //
  // Memory for FutureWeakRef
  static_assert(sizeof(FutureWeakRef) <= ObjectAlignment, "FutureWeakRef must fit into ObjectAlignment");
  size_t memorySize = ObjectAlignment;

  // Memory for FutureImpl
  const size_t futureImplOffset = GetAlignedSize(memorySize);
  memorySize = futureImplOffset + sizeof(FutureImpl);

  // Memory for FutureCallback
  const size_t futureCallbackOffset = GetAlignedSize(memorySize);
  memorySize = futureCallbackOffset + (traits.TaskPost ? sizeof(FutureCallback) : 0);

  // Memory for Value - it can be zero
  const size_t valueOffset = GetAlignedSize(memorySize);
  memorySize = valueOffset + traits.ValueSize;

  // Memory for Task - it can be zero
  const size_t taskOffset = GetAlignedSize(memorySize);
  memorySize = taskOffset + taskSize;

  VerifyElseCrashSzTag(
      (taskSize == 0) || taskBuffer,
      "taskBuffer pointer must not be null for not zero taskSize",
      0x012ca39b /* tag_blko1 */);

  void* memory = Mso::Memory::FailFast::AllocateEx(memorySize, Mso::Memory::AllocFlags::ShutdownLeak);
  VerifyElseCrashSzTag(IsAligned(memory), "memory for FutureImpl must be aligned.", 0x012ca39d /* tag_blko3 */);

  ::new (memory) FutureWeakRef();

  FutureImpl* future = ::new (static_cast<uint8_t*>(memory) + futureImplOffset) FutureImpl(traits, taskSize);

  if (traits.ValueSize > 0)
  {
    const void* value = static_cast<uint8_t*>(memory) + valueOffset;
    VerifyElseCrashSzTag(IsAligned(value), "*value must be aligned.", 0x012ca39e /* tag_blko4 */);
  }

  if (taskSize > 0)
  {
    VerifyElseCrashSzTag(taskBuffer != nullptr, "taskBuffer must not be null", 0x012ca39f /* tag_blko5 */);
    *taskBuffer = ByteArrayView(static_cast<uint8_t*>(memory) + taskOffset, taskSize);
    VerifyElseCrashSzTag(
        IsAligned(taskBuffer->Data()), "taskBuffer->Data() must be aligned.", 0x012ca3a0 /* tag_blko6 */);
  }
  else
  {
    VerifyElseCrashSzTag(taskBuffer == nullptr, "taskBuffer must be null", 0x012ca3a1 /* tag_blko7 */);
  }

  return Mso::CntPtr<IFuture>{future, AttachTag};
}

//=============================================================================
//
// FuturePackedData implementation
//
//=============================================================================

// ContinuationInvoked is a special placeholder address aligned by 8 bytes that we use instead of real continuation
// address after the continuation is either invoked or abandoned. It helps to ensure that we do not have multiple
// continuations for non-shared future, because it prevents adding a second continuation even when the first one is
// already invoked and removed.
const FutureImpl* const FuturePackedData::ContinuationInvoked =
    reinterpret_cast<FutureImpl*>(static_cast<uintptr_t>(-1) & ContinuationMask);

FutureState FuturePackedData::GetState() const noexcept
{
  return static_cast<FutureState>(Value & StateMask);
}

FutureImpl* FuturePackedData::GetContinuation() noexcept
{
  return reinterpret_cast<FutureImpl*>(Value & ContinuationMask);
}

bool FuturePackedData::IsDone() const noexcept
{
  FutureState state = GetState();
  return (state == FutureState::Succeeded || state == FutureState::Failed);
}

bool FuturePackedData::IsSucceded() const noexcept
{
  return (GetState() == FutureState::Succeeded);
}

bool FuturePackedData::IsFailed() const noexcept
{
  return (GetState() == FutureState::Failed);
}

/*static*/ FuturePackedData FuturePackedData::Make(FutureState state) noexcept
{
  return {static_cast<uintptr_t>(state)};
}

/*static*/ FuturePackedData FuturePackedData::Make(FutureState state, const FutureImpl* continuation) noexcept
{
  return {reinterpret_cast<uintptr_t>(continuation) | static_cast<uintptr_t>(state)};
}

/*static*/ void FuturePackedData::VerifyAlignment(const FutureImpl* continuation) noexcept
{
  uintptr_t contInt = reinterpret_cast<uintptr_t>(continuation);
  VerifyElseCrashSzTag(
      (contInt & ContinuationMask) == contInt,
      "Continuation instance is not aligned by 8 bytes.",
      0x012ca3a2 /* tag_blko8 */);
}

//=============================================================================
//
// CurrentFutureImpl implementation
//
//=============================================================================

static thread_local FutureImpl* tls_currentFuture;

struct CurrentFutureImpl
{
  explicit CurrentFutureImpl(FutureImpl& current) noexcept : m_previous(tls_currentFuture)
  {
    tls_currentFuture = &current;
  }

  ~CurrentFutureImpl() noexcept
  {
    tls_currentFuture = m_previous;
  }

  static bool IsCurrent(const FutureImpl* future) noexcept
  {
    return tls_currentFuture == future;
  }

private:
  FutureImpl* m_previous;
};

//=============================================================================
//
// FutureImpl implementation
//
//=============================================================================

FutureImpl::FutureImpl(const FutureTraits& traits, size_t taskSize) noexcept : m_traits(traits), m_taskSize(taskSize) {}

FutureImpl::~FutureImpl() noexcept
{
  FuturePackedData data = m_stateAndContinuation.load(std::memory_order_acquire);
  if (!data.IsDone())
  {
    // It is a bug. The only valid situation is when we have CancelIfUnfulfilled option set.
    VerifyElseCrashSzTag(
        IsSet(m_traits.Options, FutureOptions::CancelIfUnfulfilled),
        "Cannot destroy unfulfilled future.",
        0x012ca3a3 /* tag_blko9 */);

    // Only set error if there are any continuations to observe it.
    if (HasContinuation())
    {
      TrySetError(CancellationErrorProvider().MakeErrorCode(true), /*crashIfFailed:*/ true);
      data = m_stateAndContinuation.load(std::memory_order_acquire);
    }
  }

  // Destroy value if it was set. The value exists only if future succeeded.
  if (m_traits.ValueDestroy && data.IsSucceded())
  {
    m_traits.ValueDestroy(GetValueInternal());
  }

  DestroyTask(/*isAfterInvoke:*/ false);
}

ByteArrayView FutureImpl::GetCallback() noexcept
{
  if (m_traits.TaskPost)
  {
    size_t memorySize = sizeof(FutureImpl);
    return ByteArrayView(reinterpret_cast<uint8_t*>(this) + GetAlignedSize(memorySize), sizeof(FutureCallback));
  }

  return ByteArrayView();
}

ByteArrayView FutureImpl::GetValueInternal() noexcept
{
  if (m_traits.ValueSize > 0)
  {
    size_t memorySize = sizeof(FutureImpl);
    if (m_traits.TaskPost)
    {
      memorySize = sizeof(FutureCallback) + GetAlignedSize(memorySize);
    }
    return ByteArrayView(reinterpret_cast<uint8_t*>(this) + GetAlignedSize(memorySize), m_traits.ValueSize);
  }

  return ByteArrayView();
}

ByteArrayView FutureImpl::GetTask() noexcept
{
  if (m_taskSize > 0)
  {
    size_t memorySize = sizeof(FutureImpl);
    if (m_traits.TaskPost)
    {
      memorySize = sizeof(FutureCallback) + GetAlignedSize(memorySize);
    }
    if (m_traits.ValueSize > 0)
    {
      memorySize = m_traits.ValueSize + GetAlignedSize(memorySize);
    }

    return ByteArrayView(reinterpret_cast<uint8_t*>(this) + GetAlignedSize(memorySize), m_taskSize);
  }

  return ByteArrayView();
}

void FutureImpl::Invoke() noexcept
{
  if (TrySetInvoking(/*crashIfFailed:*/ HasThreadAccess()))
  {
    CurrentFutureImpl current{*this};

    if (!m_error)
    {
      if (m_traits.TaskInvoke)
      {
        m_traits.TaskInvoke(GetTask(), this, m_link.Get());
      }
      else if (IsSet(m_traits.Options, FutureOptions::UseParentValue))
      {
        VerifyElseCrashSzTag(m_link, "Parent must not be null", 0x016055c7 /* tag_byfxh */);
        (void)TrySetSuccess(nullptr, /*crashIfFailed:*/ true);
      }
      else
      {
        VerifyElseCrashSzTag(false, "Invoke the future", 0x016055c8 /* tag_byfxi */);
      }
    }
    else
    {
      if (m_traits.TaskCatch)
      {
        m_traits.TaskCatch(GetTask(), this, std::move(m_error));
      }
      else
      {
        (void)TrySetError(std::move(m_error), /*crashIfFailed:*/ true);
      }
    }
  }
}

const FutureTraits& FutureImpl::GetTraits() const noexcept
{
  return m_traits;
}

ByteArrayView FutureImpl::GetValue() noexcept
{
  FuturePackedData data = m_stateAndContinuation.load(std::memory_order_acquire);
  if (data.IsSucceded())
  {
    if (IsSet(m_traits.Options, FutureOptions::UseParentValue))
    {
      return m_link->GetValue();
    }

    return GetValueInternal();
  }

  VerifyElseCrashSzTag(false, "Value is not initialized", 0x012ca3c0 /* tag_blkpa */);
}

const ErrorCode& FutureImpl::GetError() const noexcept
{
  FuturePackedData data = m_stateAndContinuation.load(std::memory_order_acquire);
  VerifyElseCrashSzTag(data.IsFailed() && m_error, "Error is not initialized", 0x016055c9 /* tag_byfxj */);
  return m_error;
}

void FutureImpl::AddContinuation(Mso::CntPtr<IFuture>&& continuation) noexcept
{
  // We do not support custom IFuture implementations.
  Mso::CntPtr<FutureImpl> contFuture{&query_cast<FutureImpl&>(*continuation.Detach()), AttachTag};
  const FutureImpl* contFuturePtr = contFuture.Get();
  FuturePackedData::VerifyAlignment(contFuturePtr);

  FuturePackedData currentData = m_stateAndContinuation.load(std::memory_order_acquire);

  for (;;)
  {
    FutureImpl* currentContinuation = currentData.GetContinuation();
    if (currentContinuation)
    {
      // We allow multiple continuations for shared futures, and when we create a shared future.
      // In later case the future being added must be shared and use parent value. This way
      // we expect that the new continuation does not change the result and we are safe to have multiple continuations.
      bool isShared = IsSet(m_traits.Options, FutureOptions::IsShared);
      bool contIsShared = IsSet(contFuturePtr->m_traits.Options, FutureOptions::IsShared);
      bool contUsesParentValue = IsSet(contFuturePtr->m_traits.Options, FutureOptions::UseParentValue);
      VerifyElseCrashSzTag(
          isShared || (contIsShared && contUsesParentValue),
          "AddContinuation called more than once for unique future.",
          0x012ca3c1 /* tag_blkpb */);
    }

    //
    // Keep the state and store either the continuation or the FuturePackedData::ContinuationInvoked value.
    // Existing continuation is added to the single linked list. We must make sure that we do not access
    // any members of the existing continuation because it could be destroyed from a different thread.
    //
    FutureState state = currentData.GetState();
    const FutureImpl* newContinuation = FuturePackedData::ContinuationInvoked;

    //
    // m_link could be assigned in the previous loop iteration. The assignment did not change ref count because
    // we use move assignment and create Mso::CntPtr with /*shouldAddRef:*/false option. See more on assignment below.
    // We must call Detach() to set it back to nullptr to avoid calling unbalanced Release().
    //
    contFuture->m_link.Detach();
    bool isDone = (state == FutureState::Succeeded || state == FutureState::Failed);
    if (!isDone)
    {
      newContinuation = contFuture.Get();
      //
      // Do not change ref count when assigning previous continuation. It could be already deleted by now.
      // So, we treat it just as a raw pointer until compare_exchange_weak succeeds.
      // Initially it is owned by this FutureImpl instance, and if compare_exchange_weak succeeds then the
      // ownership is transfered to the new continuation contFuture. No ref count should be changed during that
      // ownership transition.
      //
      contFuture->m_link = Mso::CntPtr<FutureImpl>{currentContinuation, AttachTag};
    }

    FuturePackedData newData = FuturePackedData::Make(state, newContinuation);
    if (m_stateAndContinuation.compare_exchange_weak(currentData, newData))
    {
      if (isDone)
      {
        VerifyElseCrashSzTag(
            contFuture->m_link.IsEmpty(),
            "All existing continuations must be already executed",
            0x014441c2 /* tag_brehc */);
        PostContinuation(std::move(contFuture));
      }
      else
      {
        // m_stateAndContinuation now owns the reference.
        contFuture.Detach();
      }

      break;
    }
  }
}

template <typename TLambda>
static void LoopAndWait(TLambda lambda) noexcept
{
  int iterationCount = 0;
  while (!lambda())
  {
    // We continue iterate until the lambda returns false;
    // If we cannot get the false result after some iterations, then
    // first try to yield the thread, and then put the thread to the sleeping state with the increasing interval.
    // We exit after waiting for 10 seconds.
    ++iterationCount;
    if (iterationCount > 10'000) // 10 seconds since we wait 1ms each cycle most of times.
    {
      return;
    }
    else if (iterationCount > 50)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    else if (iterationCount > 20)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    else if (iterationCount > 10)
    {
      std::this_thread::yield();
    }
  }
}

bool FutureImpl::TrySetInvoking(bool crashIfFailed) noexcept
{
  // We can start Invoking either from Posting or from Posted states.
  // From Posting state we must do it synchronously, while from Posted it can be done asynchronously.
  // It is possible that asynchronous execution starts before we moved to Posted state.
  // For that reason we wait if we try to get from Posting to Invoking state asynchronously.

  bool hasThreadAccess = HasThreadAccess();
  ExpectedStates expectedStates = ExpectedStates::Posted;
  if (hasThreadAccess)
  {
    expectedStates = expectedStates | ExpectedStates::Posting;
  }

  std::optional<FutureState> actualState;
  LoopAndWait([&]() noexcept {
    actualState = TrySetState(FutureState::Invoking, expectedStates);
    return actualState && *actualState == FutureState::Posting;
  });

  if (actualState)
  {
    return UnexpectedState(
        *actualState, crashIfFailed, "Cannot move to Invoking state", MsoReserveTag(0x016055cb /* tag_byfxl */));
  }

  return true;
}

// Returns std::nullopt when succeeded, or the current state when failed.
std::optional<FutureState>
FutureImpl::TrySetState(FutureState newState, ExpectedStates expectedStates, FutureImpl** continuation) noexcept
{
  std::optional<FutureState> result;
  FuturePackedData currentData = m_stateAndContinuation.load(std::memory_order_acquire);
  LoopAndWait([&]() noexcept {
    result = currentData.GetState();
    if (!IsExpectedState(*result, expectedStates))
    {
      // If current state is a locking state, then we continue to wait until we get into a different state.
      return !IsLockingState(*result);
    }

    FutureImpl* currentContinuation = currentData.GetContinuation();
    const FutureImpl* newContinuation = currentContinuation;
    if (IsFinalState(newState))
    {
      newContinuation = newContinuation != nullptr ? FuturePackedData::ContinuationInvoked : nullptr;
    }
    FuturePackedData newData = FuturePackedData::Make(newState, newContinuation);
    if (m_stateAndContinuation.compare_exchange_weak(/*ref*/ currentData, newData))
    {
      if (continuation)
      {
        *continuation = currentContinuation;
      }
      result = std::nullopt; // To indicate success
      return true;
    }

    return false;
  });

  return result;
}

// Returns std::nullopt when succeeded, or the current state when failed.
std::optional<FutureState> FutureImpl::TrySetState(FutureState newState, FutureImpl** continuation) noexcept
{
  ExpectedStates expectedStates = GetExtectedStates(newState);
  std::optional<FutureState> result;
  FuturePackedData currentData = m_stateAndContinuation.load(std::memory_order_acquire);
  LoopAndWait([&]() noexcept {
    result = currentData.GetState();
    if (!IsExpectedState(*result, expectedStates))
    {
      // If current state is a locking state, then we continue to wait until we get into a different state.
      return !IsLockingState(*result);
    }

    FutureImpl* currentContinuation = currentData.GetContinuation();
    const FutureImpl* newContinuation = currentContinuation;
    if (IsFinalState(newState))
    {
      newContinuation = newContinuation != nullptr ? FuturePackedData::ContinuationInvoked : nullptr;
    }
    FuturePackedData newData = FuturePackedData::Make(newState, newContinuation);
    if (m_stateAndContinuation.compare_exchange_weak(/*ref*/ currentData, newData))
    {
      if (continuation)
      {
        *continuation = currentContinuation;
      }
      result = std::nullopt; // To indicate success
      return true;
    }

    return false;
  });

  return result;
}

/*static*/ bool FutureImpl::IsExpectedState(FutureState state, ExpectedStates expectedStates) noexcept
{
  return ((1 << (int)state) & (int)expectedStates) != 0;
}

/*static*/ bool FutureImpl::IsLockingState(FutureState state) noexcept
{
  return IsExpectedState(state, ExpectedStates::Posting | ExpectedStates::Invoking | ExpectedStates::SettingResult);
}

/*static*/ bool FutureImpl::IsFinalState(FutureState state) noexcept
{
  return IsExpectedState(state, ExpectedStates::Succeeded | ExpectedStates::Failed);
}

// The valid state transitions from the futureImpl.h:
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
ExpectedStates FutureImpl::GetExtectedStates(FutureState newState) noexcept
{
  auto whenCurrent = [this](ExpectedStates states) { return HasThreadAccess() ? states : ExpectedStates::None; };

  switch (newState)
  {
    case FutureState::Posting: return ExpectedStates::Pending;
    case FutureState::Posted: return whenCurrent(ExpectedStates::Posting);
    case FutureState::Invoking: return whenCurrent(ExpectedStates::Posting) | ExpectedStates::Posted;
    case FutureState::Awaiting: return whenCurrent(ExpectedStates::Invoking);
    case FutureState::SettingResult:
      return ExpectedStates::Pending | ExpectedStates::Posted | ExpectedStates::Awaiting
          | whenCurrent(ExpectedStates::Posting | ExpectedStates::Invoking);
    case FutureState::Succeeded: return whenCurrent(ExpectedStates::SettingResult);
    case FutureState::Failed: return whenCurrent(ExpectedStates::SettingResult);
    default: return ExpectedStates::None;
  }
}

_Use_decl_annotations_ bool
FutureImpl::TryStartSetValue(ByteArrayView& valueBuffer, void** prevThreadFuture, bool crashIfFailed) noexcept
{
  // We can set value only if it is not of void type.
  // We can move to SettingResult state to set value only from these states:
  // 1. Pending - if there is no TaskInvoke callback or if it is a MultiPost future.
  // 2. Invoking - if the value set synchronously.
  // 3. Awaiting.

  if (m_traits.ValueSize == 0)
  {
    VerifyElseCrashSzTag(!crashIfFailed, "Value must not be of void type", 0x016055cc /* tag_byfxm */);
    return false;
  }

  ExpectedStates expectedStates{};
  if (IsSet(m_traits.Options, FutureOptions::IsMultiPost) || !m_traits.TaskInvoke)
  {
    // We can set value from the Pending state for the MultiPost futures or when no Task needs to be invoked first.
    expectedStates = expectedStates | ExpectedStates::Pending;
  }
  if (HasThreadAccess())
  {
    // We can set value from the code being invoked synchronously.
    expectedStates = expectedStates | ExpectedStates::Invoking;
  }
  expectedStates = expectedStates | ExpectedStates::Awaiting;

  if (auto unexpectedState = TrySetState(FutureState::SettingResult, expectedStates))
  {
    switch (*unexpectedState)
    {
      case FutureState::Pending:
        return UnexpectedState(
            *unexpectedState,
            crashIfFailed,
            "TaskInvoke must be called before setting value.",
            MsoReserveTag(0x016055cd /* tag_byfxn */));
      case FutureState::Invoking:
        return UnexpectedState(
            *unexpectedState,
            crashIfFailed,
            "Value can be set from Invoking state only synchronously",
            MsoReserveTag(0x016055ce /* tag_byfxo */));
      default:
        return UnexpectedState(
            *unexpectedState,
            crashIfFailed,
            "We cannot move to SettingResult from this state.",
            MsoReserveTag(0x016055cf /* tag_byfxp */));
    }
  }

  *prevThreadFuture = tls_currentFuture;
  tls_currentFuture = this;
  valueBuffer = GetValueInternal();
  return true;
}

bool FutureImpl::HasThreadAccess() const noexcept
{
  return CurrentFutureImpl::IsCurrent(this);
}

void AppendFutureStateToString(std::string& str, FutureState state) noexcept
{
  switch (state)
  {
    case FutureState::Pending: str += "Pending"; break;
    case FutureState::Posting: str += "Posting"; break;
    case FutureState::Posted: str += "Posted"; break;
    case FutureState::Invoking: str += "Invoking"; break;
    case FutureState::Awaiting: str += "Awaiting"; break;
    case FutureState::SettingResult: str += "SettingResult"; break;
    case FutureState::Succeeded: str += "Succeeded"; break;
    case FutureState::Failed: str += "Failed"; break;
    default: VerifyElseCrashSz(false, "Unknown state: only 8 states must be supported"); break;
  }
}

bool FutureImpl::UnexpectedState(FutureState state, bool crashIfFailed, const char* errorMessage, uint32_t tag) noexcept
{
  if (crashIfFailed)
  {
    std::string errorText = "State: ";
    AppendFutureStateToString(errorText, state);
    errorText += ". ";
    errorText += errorMessage;
    VerifyElseCrashSzTag(false, errorText.c_str(), tag);
  }

  return false;
}

void FutureImpl::StartAwaiting() noexcept
{
  if (auto unexpectedState = TrySetState(FutureState::Awaiting, ExpectedStates::Invoking))
  {
    VerifyElseCrashSzTag(false, "Current state must be FutureState::Invoking", 0x012ca3c4 /* tag_blkpe */);
  }
}

bool FutureImpl::IsVoidValue() const noexcept
{
  return m_traits.ValueSize == 0;
}

bool FutureImpl::HasContinuation() const noexcept
{
  FuturePackedData currentData = m_stateAndContinuation.load(std::memory_order_acquire);
  const FutureImpl* continuation = currentData.GetContinuation();
  return (continuation != nullptr) && (continuation != FuturePackedData::ContinuationInvoked);
}

bool FutureImpl::TrySetSuccess(void* lockState, bool crashIfFailed) noexcept
{
  // Success can be set from the following states:
  // 1. Pending if there is no TaskInvoke, value type is void, and there is no UseParentValue
  // 2. Posting if there is no TaskInvoke, and there is UseParentValue (this flag forces having void value type).
  // 3. Invoking if value type is void and called synchronously.
  // 4. Awaiting if value type is void.
  // 5. SettingResult if value type is not void.

  ExpectedStates expectedStates = ExpectedStates::SettingResult;
  if (IsVoidValue()
      && (IsSet(m_traits.Options, FutureOptions::IsMultiPost)
          || (!m_traits.TaskInvoke && !IsSet(m_traits.Options, FutureOptions::UseParentValue))))
  {
    expectedStates = expectedStates | ExpectedStates::Pending;
  }
  if (!m_traits.TaskInvoke && HasThreadAccess() && IsSet(m_traits.Options, FutureOptions::UseParentValue))
  {
    expectedStates = expectedStates | ExpectedStates::Posting;
  }
  if (HasThreadAccess() && IsVoidValue())
  {
    expectedStates = expectedStates | ExpectedStates::Invoking;
  }
  if (IsVoidValue())
  {
    expectedStates = expectedStates | ExpectedStates::Awaiting;
  }

  FutureImpl* continuation{};
  if (auto actualState = TrySetState(FutureState::Succeeded, expectedStates, &continuation))
  {
    switch (*actualState)
    {
      case FutureState::Pending:
        if (!IsSet(m_traits.Options, FutureOptions::IsMultiPost))
        {
          if (m_traits.TaskInvoke)
          {
            return UnexpectedState(
                *actualState,
                crashIfFailed,
                "Task must be invoked before moving to Succeeded state.",
                MsoReserveTag(0x016055d0 /* tag_byfxq */));
          }

          if (IsSet(m_traits.Options, FutureOptions::UseParentValue))
          {
            return UnexpectedState(
                *actualState,
                crashIfFailed,
                "Futures that use parent value must move to Posting state before moving to Succeeded state.",
                MsoReserveTag(0x016055d1 /* tag_byfxr */));
          }
        }

        return UnexpectedState(
            *actualState,
            crashIfFailed,
            "Non-void value must be set before moving to Succeeded state.",
            MsoReserveTag(0x016055d2 /* tag_byfxs */));

        break;

      case FutureState::Posting:
        if (m_traits.TaskInvoke)
        {
          return UnexpectedState(
              *actualState,
              crashIfFailed,
              "Task must be invoked before moving to Succeeded state.",
              MsoReserveTag(0x016055d3 /* tag_byfxt */));
        }

        if (!HasThreadAccess())
        {
          return UnexpectedState(
              *actualState,
              crashIfFailed,
              "From Posting state we can move to Succeeded state only synchronously.",
              MsoReserveTag(0x016055d4 /* tag_byfxu */));
        }

        return UnexpectedState(
            *actualState,
            crashIfFailed,
            "We can only move to Succeeded state from Posting state if future uses parent value.",
            MsoReserveTag(0x016055d5 /* tag_byfxv */));

      case FutureState::Invoking:
        if (!HasThreadAccess())
        {
          return UnexpectedState(
              *actualState,
              crashIfFailed,
              "From Invoking state we can move to Succeeded state only synchronously.",
              MsoReserveTag(0x016055d6 /* tag_byfxw */));
        }

        return UnexpectedState(
            *actualState,
            crashIfFailed,
            "Non-void value must be set before moving to Succeeded state.",
            MsoReserveTag(0x016055d7 /* tag_byfxx */));

      case FutureState::Awaiting:
        return UnexpectedState(
            *actualState,
            crashIfFailed,
            "Non-void value must be set before moving to Succeeded state.",
            MsoReserveTag(0x016055d8 /* tag_byfxy */));

      default:
        return UnexpectedState(
            *actualState, crashIfFailed, "Cannot move to Succeeded state.", MsoReserveTag(0x016055d9 /* tag_byfxz */));
    }
  }

  tls_currentFuture = static_cast<FutureImpl*>(lockState);

  if (m_link && !IsSet(m_traits.Options, FutureOptions::UseParentValue))
  {
    m_link = nullptr;
  }

  m_error = nullptr;

  // Task execution is completed. It should be destroyed now to avoid keeping captured resources for long time.
  DestroyTask(/*isAfterInvoke:*/ true);

  VerifyElseCrashSzTag(
      continuation != FuturePackedData::ContinuationInvoked,
      "Continuation must not be invoked yet.",
      0x012ca3c6 /* tag_blkpg */);

  PostContinuation(Mso::CntPtr<FutureImpl>{continuation, AttachTag});

  return true;
}

bool FutureImpl::TrySetError(ErrorCode&& futureError, bool crashIfFailed) noexcept
{
  if (TryStartSetError(crashIfFailed))
  {
    m_error = std::move(futureError);
    SetFailed();
    return true;
  }

  return false;
}

bool FutureImpl::TryStartSetError(bool crashIfFailed) noexcept
{
  // TODO:
  // CheckFutureStateTag(
  //   m_link == nullptr,
  //   state,
  //   crashIfFailed,
  //   "Error cannot be set from Pending state if future is a part of linked list",
  //   MsoReserveTag(0x016055da /* tag_byfx0 */));

  // TODO:
  // case FutureState::Posting:
  // if (!HasThreadAccess())
  //{
  //  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  //  currentData = m_stateAndContinuation.load(std::memory_order_acquire);
  //  continue;
  //}

  // We can set error if current states are:
  // - Pending - no execution started yet.
  // - Posted - scheduled for asynchronous invocation
  // - Awaiting - waiting for returned future to complete.
  // From these states we can set error synchronously:
  // - Posting - when there is UseCurrentValue flag, or in the TaskPost callback.
  // - Invoking - when TaskInvoke ends up with an error.
  // When error is set asynchronously in Posting state we should wait until Future moves to a different state.
  // The main reason is that we may have scheduled asynchronous invocation and it started before we moved to Posted
  // state. In the other three states: SettingValue, Succeeded, and Failed, it is too late to set the error.

  ExpectedStates expectedStates =
      ExpectedStates::Pending | ExpectedStates::Posted | ExpectedStates::Awaiting | ExpectedStates::Posting;

  if (HasThreadAccess())
  {
    expectedStates = expectedStates | ExpectedStates::Invoking;
  }

  if (auto actualState = TrySetState(FutureState::SettingResult, expectedStates))
  {
    return UnexpectedState(
        *actualState, crashIfFailed, "From this state we cannot set error", MsoReserveTag(0x016055dc /* tag_byfx2 */));
  }

  return true;
}

void FutureImpl::SetFailed() noexcept
{
  // Move to FutureState::Failed state and notify continuations about the failure.
  // We can only move to FutureState::Failed if previous state was FutureState::SettingResult.
  FutureImpl* continuation{};
  if (auto actualState = TrySetState(FutureState::Failed, ExpectedStates::SettingResult, &continuation))
  {
    UnexpectedState(
        *actualState,
        /*crashIfFailed:*/ true,
        "Cannot move to Failed state",
        MsoReserveTag(0x016055dd /* tag_byfx3 */));
  }

  m_link = nullptr;

  // Task execution is completed. It should be destroyed now to avoid keeping captured resources for long time.
  DestroyTask(/*isAfterInvoke:*/ true);

  VerifyElseCrashSzTag(
      continuation != FuturePackedData::ContinuationInvoked,
      "Continuation must not be invoked yet.",
      0x012ca3c9 /* tag_blkpj */);

  PostContinuation(Mso::CntPtr<FutureImpl>{continuation, AttachTag});
}

bool FutureImpl::TrySetPosted() noexcept
{
  // We can only move to FutureState::Posted if previous state was FutureState::Posting.
  VerifyElseCrashSzTag(HasThreadAccess(), "Current future must own the thread", 0x016055de /* tag_byfx4 */);
  return !TrySetState(FutureState::Posted, ExpectedStates::Posting);
}

bool FutureImpl::IsDone() const noexcept
{
  return m_stateAndContinuation.load(std::memory_order_acquire).IsDone();
}

bool FutureImpl::IsSucceeded() const noexcept
{
  return m_stateAndContinuation.load(std::memory_order_acquire).IsSucceded();
}

bool FutureImpl::IsFailed() const noexcept
{
  return m_stateAndContinuation.load(std::memory_order_acquire).IsFailed();
}

void FutureImpl::Post() noexcept
{
  Mso::CntPtr<FutureImpl> next;
  (void)TryPostInternal(nullptr, /*ref*/ next, /*crashIfFailed:*/ true);
}

bool FutureImpl::TryPostInternal(FutureImpl* parent, Mso::CntPtr<FutureImpl>& next, bool crashIfFailed) noexcept
{
  if (!IsSet(m_traits.Options, FutureOptions::IsMultiPost))
  {
    // TryPostInternal tries first move to Posting state.
    // If it succeeds, then it gives a chance to TaskPost callback to schedule asynchronous work, do inline invocation,
    // or set an error. If after TaskPost completion we are still in Posting state, then we move to Posted state.
    // If there is no TaskPost callback, then we try to execute the TaskInvoke callback inline.
    // If there is no TaskInvoke callback then we expect future to use parent value and just complete the future.
    // Otherwise, the situation is unexpected and we crash.

    // We can only start post if we were able to move to FutureState::Posting from FutureState::Pending
    if (auto unexpectedState = TrySetState(FutureState::Posting, ExpectedStates::Pending))
    {
      VerifyElseCrashSzTag(
          m_link == nullptr, "State must be FutureState::Pending if m_link is not null", 0x016055df /* tag_byfx5 */);
      next = nullptr;
      return UnexpectedState(
          *unexpectedState, crashIfFailed, "Post expects Pending state", MsoReserveTag(0x016055e0 /* tag_byfx6 */));
    }

    CurrentFutureImpl current{*this}; // For synchronous call checks.

    next = std::move(m_link);
    VerifyElseCrashSzTag(
        parent != nullptr || next == nullptr,
        "Future must not be part of a list if parent is null",
        0x016055e1 /* tag_byfx7 */);

    if (parent)
    {
      if (parent->IsDone())
      {
        if (!IsSet(m_traits.Options, FutureOptions::CallTaskInvokeOnError))
        {
          if (parent->IsSucceeded())
          {
            m_link = parent;
          }
          else
          {
            m_error = parent->m_error;
          }
        }
        else
        {
          m_link = parent;
        }
      }
      else
      {
        UnexpectedState(
            parent->m_stateAndContinuation.load(std::memory_order_acquire).GetState(),
            /*crashIfFailed:*/ true,
            "Unexpected parent state while posting",
            MsoReserveTag(0x016055e2 /* tag_byfx8 */));
      }
    }

    if (m_traits.TaskPost)
    {
      // TaskPost expects a DispatchTask that wraps up an instance of IDispatchTask interface.
      // The expectation is that TaskPost synchronously or asynchronously calls either IDispatchTask::Invoke()
      // or IDispatchTask::Cancel() methods.
      // It is a crash when these methods are both called or called twice.
      // Though in tests we may have a situation when none of these methods is called. In such case we cancel the
      // Future. To be able to cancel a not-invoked Future, we use a separate FutureCallback class with its own ref
      // count.
      FutureCallback* callback = GetCallback().As<FutureCallback>();
      ::new (callback) FutureCallback();
      m_traits.TaskPost(GetTask(), Mso::DispatchTask{callback, AttachTag});
      // If TaskPost did not execute code inline then we should move to Posted state.
      (void)TrySetPosted();
    }
    else
    {
      Invoke(); // Invoke inline
    }
  }
  else
  {
    CurrentFutureImpl current{*this}; // For synchronous call checks.

    // In MultiPost mode we just invoke TaskInvoke or TaskCatch callbacks inline.
    // Multiple parent futures may call these callbacks simultaneously even after this future is succeeded or failed.
    VerifyElseCrashSzTag(parent != nullptr, "MultiPost parent must not be null", 0x016055e3 /* tag_byfx9 */);
    if (parent->IsSucceeded())
    {
      m_traits.TaskInvoke(GetTask(), this, parent);
    }
    else if (parent->IsFailed())
    {
      m_traits.TaskCatch(GetTask(), this, std::move(parent->m_error));
    }
    else
    {
      VerifyElseCrashSzTag(false, "MultiPost parent is in unexpected state", 0x01605600 /* tag_byfya */);
    }
  }

  return true;
}

void FutureImpl::PostContinuation(Mso::CntPtr<FutureImpl>&& continuation) noexcept
{
  while (continuation)
  {
    // TryPostInternal returns next continuation in the single linked list.
    Mso::CntPtr<FutureImpl> next;
    (void)continuation->TryPostInternal(this, /*ref*/ next, /*crashIfFailed:*/ false);
    continuation = std::move(next);
  }
}

void FutureImpl::DestroyTask(bool isAfterInvoke) noexcept
{
  if (m_taskSize > 0 && m_traits.TaskDestroy)
  {
    if (!isAfterInvoke || IsSet(m_traits.Options, FutureOptions::DestroyTaskAfterInvoke))
    {
      m_traits.TaskDestroy(GetTask());

      // Set task size to zero to indicate that we do not use this memory anymore.
      m_taskSize = 0;
    }
  }
}

// IUnknown
STDMETHODIMP FutureImpl::QueryInterface(const GUID& riid, _Outptr_ void** ppvObject) noexcept
{
  return ::Mso::Details::QueryInterfaceHelper<FutureImpl>::QueryInterface(this, riid, ppvObject);
}

STDMETHODIMP_(ULONG) FutureImpl::AddRef() noexcept
{
  const FutureWeakRef* weakRef = Mso::GetFutureWeakRef(this);
  weakRef->AddRef();
  return 1;
}

STDMETHODIMP_(ULONG) FutureImpl::Release() noexcept
{
  const FutureWeakRef* weakRef = Mso::GetFutureWeakRef(this);
  const bool shouldDestroy = weakRef->Release();
  if (shouldDestroy)
  {
    this->~FutureImpl();
    weakRef->ReleaseWeakRef();
  }
  return 1;
}

//=============================================================================
//
// FutureCallback implementation
//
//=============================================================================

FutureImpl* FutureCallback::GetFutureImpl(_In_ FutureCallback* callback) noexcept
{
  return reinterpret_cast<FutureImpl*>(reinterpret_cast<uint8_t*>(callback) - sizeof(FutureImpl));
}

FutureCallback::FutureCallback() noexcept
{
  GetFutureImpl(this)->AddRef();
}

FutureCallback::~FutureCallback() noexcept
{
  if (!m_isCalled)
  {
    // The FutureCallback is never called: we must cancel the future
    GetFutureImpl(this)->TrySetError(Mso::CancellationErrorProvider().MakeErrorCode(true));
  }
}

void FutureCallback::Invoke() noexcept
{
  VerifyElseCrashSzTag(!m_isCalled.exchange(true), "FutureCallback is called twice", 0x024c5890 /* tag_ctf8q */);
  GetFutureImpl(this)->Invoke();
}

void FutureCallback::OnCancel() noexcept
{
  VerifyElseCrashSzTag(!m_isCalled.exchange(true), "FutureCallback is called twice", 0x024c5891 /* tag_ctf8r */);

  // If dispatch queue cannot execute our callback then we try to cancel it.
  GetFutureImpl(this)->TrySetError(Mso::CancellationErrorProvider().MakeErrorCode(true));
}

HRESULT STDMETHODCALLTYPE FutureCallback::QueryInterface(GUID const& riid, _COM_Outptr_ void** ppvObject) noexcept
{
  return ::Mso::Details::QueryInterfaceHelper<FutureCallback>::QueryInterface(this, riid, ppvObject);
}

ULONG STDMETHODCALLTYPE FutureCallback::AddRef() noexcept
{
  if (++m_refCount == 1)
  {
    Debug(VerifyElseCrashSzTag(false, "Ref count must not bounce from zero", 0x024c5892 /* tag_ctf8s */));
  }

  return 1; // Return an invalid counter to avoid other code depending on it.
}

ULONG STDMETHODCALLTYPE FutureCallback::Release() noexcept
{
  const uint32_t refCount = --m_refCount;
  Debug(VerifyElseCrashSzTag(
      static_cast<int32_t>(refCount) >= 0, "Ref count must not be negative.", 0x024c5893 /* tag_ctf8t */));
  if (refCount == 0)
  {
    this->~FutureCallback();
    GetFutureImpl(this)->Release();
  }

  return 1; // Return an invalid counter to avoid other code depending on it.
}

//=============================================================================
//
// FutureWeakRef implementation
//
//=============================================================================

void FutureWeakRef::AddRef() const noexcept
{
  if (++m_refCount == 1)
  {
    Debug(VerifyElseCrashSzTag(false, "Ref count must not bounce from zero", 0x01605601 /* tag_byfyb */));
  }
}

bool FutureWeakRef::Release() const noexcept
{
  const uint32_t refCount = --m_refCount;
  Debug(VerifyElseCrashSzTag(
      static_cast<int32_t>(refCount) >= 0, "Ref count must not be negative.", 0x01605602 /* tag_byfyc */));
  return (refCount == 0);
}

void FutureWeakRef::AddWeakRef() const noexcept
{
  if (++m_weakRefCount == 1)
  {
    Debug(VerifyElseCrashSzTag(false, "Weak ref count must not bounce from zero", 0x01605603 /* tag_byfyd */));
  }
}

void FutureWeakRef::ReleaseWeakRef() const noexcept
{
  const uint32_t weakRefCount = --m_weakRefCount;
  Debug(VerifyElseCrashSzTag(
      static_cast<int32_t>(weakRefCount) >= 0, "Weak ref count must not be negative.", 0x01605604 /* tag_byfye */));
  if (weakRefCount == 0)
  {
    Mso::MakeAllocator::Deallocate(const_cast<FutureWeakRef*>(this));
  }
}

bool FutureWeakRef::IsExpired() const noexcept
{
  return (m_refCount.load(std::memory_order_acquire) == 0);
}

bool FutureWeakRef::IncrementRefCountIfNotZero() noexcept
{
  uint32_t count = m_refCount.load(std::memory_order_acquire);
  for (;;)
  {
    if (count == 0)
    {
      return false;
    }

    if (m_refCount.compare_exchange_weak(/*ref*/ count, count + 1))
    {
      return true;
    }
  }
}

} // namespace Futures

inline Mso::Futures::FutureWeakRef* GetFutureWeakRef(const void* ptr) noexcept
{
  return reinterpret_cast<Mso::Futures::FutureWeakRef*>(
      reinterpret_cast<uint8_t*>(const_cast<void*>(ptr)) - Mso::Futures::ObjectAlignment);
}

//=============================================================================
//
// FutureWeakPtrBase implementation
//
//=============================================================================

LIBLET_PUBLICAPI FutureWeakPtrBase::FutureWeakPtrBase() noexcept : m_ptr(nullptr) {}

LIBLET_PUBLICAPI FutureWeakPtrBase::FutureWeakPtrBase(_In_opt_ void* ptr, bool shouldAddWeakRef) noexcept : m_ptr(ptr)
{
  if (shouldAddWeakRef)
    CheckedAddWeakRef(m_ptr);
}

LIBLET_PUBLICAPI FutureWeakPtrBase::~FutureWeakPtrBase() noexcept
{
  CheckedReleaseWeakRef(m_ptr);
}

LIBLET_PUBLICAPI /*static*/ bool FutureWeakPtrBase::IncrementRefCountIfNotZero(_In_opt_ const void* ptr) noexcept
{
  return ptr && GetFutureWeakRef(ptr)->IncrementRefCountIfNotZero();
}

LIBLET_PUBLICAPI /*static*/ void FutureWeakPtrBase::CheckedAddWeakRef(_In_opt_ const void* ptr) noexcept
{
  if (ptr)
    GetFutureWeakRef(ptr)->AddWeakRef();
}

LIBLET_PUBLICAPI /*static*/ void FutureWeakPtrBase::CheckedReleaseWeakRef(_In_opt_ const void* ptr) noexcept
{
  if (ptr)
    GetFutureWeakRef(ptr)->ReleaseWeakRef();
}

LIBLET_PUBLICAPI void FutureWeakPtrBase::Reset() noexcept
{
  const void* ptr = m_ptr;
  m_ptr = nullptr;
  CheckedReleaseWeakRef(ptr);
}

LIBLET_PUBLICAPI bool FutureWeakPtrBase::IsEmpty() const noexcept
{
  return m_ptr == nullptr;
}

LIBLET_PUBLICAPI bool FutureWeakPtrBase::IsExpired() const noexcept
{
  return !m_ptr || GetFutureWeakRef(m_ptr)->IsExpired();
}

LIBLET_PUBLICAPI void FutureWeakPtrBase::Assign(_In_opt_ void* from) noexcept
{
  if (m_ptr != from)
  {
    const void* ptr = m_ptr;
    m_ptr = from;
    CheckedAddWeakRef(m_ptr);
    CheckedReleaseWeakRef(ptr);
  }
}

LIBLET_PUBLICAPI void FutureWeakPtrBase::Attach(FutureWeakPtrBase&& from) noexcept
{
  if (m_ptr != from.m_ptr)
  {
    const void* ptr = m_ptr;
    m_ptr = from.m_ptr;
    from.m_ptr = nullptr;
    CheckedReleaseWeakRef(ptr);
  }
}

//=============================================================================
// FutureWaitTask implementation
//=============================================================================

namespace Futures {

struct FutureWaitTask
{
  static void Invoke(const ByteArrayView& taskBuffer, IFuture* future, IFuture* /*parentFuture*/) noexcept
  {
    future->TrySetSuccess(nullptr, /*crashIfFailed:*/ true);
    SetFinished(taskBuffer);
  }

  static void Catch(const ByteArrayView& taskBuffer, IFuture* future, ErrorCode&& parentError) noexcept
  {
    future->TrySetError(std::move(parentError), /*crashIfFailed:*/ true);
    SetFinished(taskBuffer);
  }

private:
  static void SetFinished(const ByteArrayView& taskBuffer) noexcept
  {
    auto task = taskBuffer.As<FutureWaitTask>();
    task->IsFinished.Set();
  }

public:
  constexpr static FutureCatchCallback* CatchPtr = &Catch;

public:
  Mso::ManualResetEvent& IsFinished;
};

LIBLET_PUBLICAPI void FutureWait(IFuture& future) noexcept
{
  if (!future.IsDone())
  {
    Mso::ManualResetEvent finished;

    constexpr const auto& futureTraits = FutureTraitsProvider<
        /*Options:    */ FutureOptions::UseParentValue,
        /*ResultType: */ void,
        /*TaskType:   */ void,
        /*PostType:   */ void,
        /*InvokeType: */ FutureWaitTask,
        /*CatchType:  */ FutureWaitTask>::Traits;

    ByteArrayView waitTaskBuffer;
    Mso::CntPtr<IFuture> waitFuture = MakeFuture(futureTraits, sizeof(FutureWaitTask), &waitTaskBuffer);
    ::new (waitTaskBuffer.Data()) FutureWaitTask{finished};

    future.AddContinuation(std::move(waitFuture));

    finished.Wait();
  }
}

} // namespace Futures
} // namespace Mso
