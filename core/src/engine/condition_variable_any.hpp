#pragma once

#include <memory>

#include <engine/condition_variable.hpp>
#include <engine/deadline.hpp>
#include <engine/task/cancel.hpp>
#include <utils/assert.hpp>

#include "task/task_context.hpp"
#include "wait_list.hpp"

namespace engine {
namespace impl {

template <typename MutexType>
class CvWaitStrategy final : public WaitStrategy {
 public:
  CvWaitStrategy(Deadline deadline, WaitList& waiters, TaskContext& current,
                 std::unique_lock<MutexType>& mutex_lock)
      : WaitStrategy(deadline),
        waiters_(waiters),
        waiter_token_(waiters_),
        waiters_lock_(waiters_),
        current_(current),
        mutex_lock_(mutex_lock) {}

  void AfterAsleep() override {
    UASSERT(&current_ == current_task::GetCurrentTaskContext());
    waiters_.Append(waiters_lock_, &current_);
    waiters_lock_.unlock();
    mutex_lock_.unlock();
  }

  void BeforeAwake() override {
    UASSERT(&current_ == current_task::GetCurrentTaskContext());
    {
      WaitList::Lock guard_waiters{waiters_};
      waiters_.Remove(guard_waiters, &current_);
    }
    mutex_lock_.lock();
  }

 private:
  WaitList& waiters_;
  const WaitList::WaitersScopeCounter waiter_token_;
  WaitList::Lock waiters_lock_;
  TaskContext& current_;
  std::unique_lock<MutexType>& mutex_lock_;
};

template <typename MutexType>
class ConditionVariableAny {
 public:
  ConditionVariableAny() = default;

  ConditionVariableAny(const ConditionVariableAny&) = delete;
  ConditionVariableAny(ConditionVariableAny&&) = delete;
  ConditionVariableAny& operator=(const ConditionVariableAny&) = delete;
  ConditionVariableAny& operator=(ConditionVariableAny&&) = delete;

  [[nodiscard]] CvStatus Wait(std::unique_lock<MutexType>&);

  template <typename Predicate>
  [[nodiscard]] bool Wait(std::unique_lock<MutexType>&, Predicate&&);

  CvStatus WaitUntil(std::unique_lock<MutexType>&, Deadline);

  template <typename Predicate>
  bool WaitUntil(std::unique_lock<MutexType>&, Deadline, Predicate&&);

  void NotifyOne();
  void NotifyAll();

 private:
  WaitList waiters_;
};

template <typename MutexType>
CvStatus ConditionVariableAny<MutexType>::Wait(
    std::unique_lock<MutexType>& lock) {
  return WaitUntil(lock, {});
}

template <typename MutexType>
template <typename Predicate>
bool ConditionVariableAny<MutexType>::Wait(std::unique_lock<MutexType>& lock,
                                           Predicate&& predicate) {
  return WaitUntil(lock, {}, std::forward<Predicate>(predicate));
}

template <typename MutexType>
CvStatus ConditionVariableAny<MutexType>::WaitUntil(
    std::unique_lock<MutexType>& lock, Deadline deadline) {
  if (deadline.IsReached()) {
    return CvStatus::kTimeout;
  }

  auto current = current_task::GetCurrentTaskContext();
  if (current->ShouldCancel()) {
    return CvStatus::kCancelled;
  }

  CvWaitStrategy<MutexType> wait_manager(deadline, waiters_, *current, lock);

  const auto wakeup_source = current->Sleep(wait_manager);

  switch (wakeup_source) {
    case TaskContext::WakeupSource::kCancelRequest:
      return CvStatus::kCancelled;
    case TaskContext::WakeupSource::kDeadlineTimer:
      return CvStatus::kTimeout;
    case TaskContext::WakeupSource::kNone:
    case TaskContext::WakeupSource::kBootstrap:
      UASSERT(!"invalid wakeup source");
      [[fallthrough]];
    case TaskContext::WakeupSource::kWaitList:
      return CvStatus::kNoTimeout;
  }
}

template <typename MutexType>
template <typename Predicate>
bool ConditionVariableAny<MutexType>::WaitUntil(
    std::unique_lock<MutexType>& lock, Deadline deadline,
    Predicate&& predicate) {
  bool predicate_result = predicate();
  auto status = CvStatus::kNoTimeout;
  while (!predicate_result && status == CvStatus::kNoTimeout) {
    status = WaitUntil(lock, deadline);
    predicate_result = predicate();
    if (!predicate_result && status == CvStatus::kNoTimeout) {
      current_task::AccountSpuriousWakeup();
    }
  }
  return predicate_result;
}

template <typename MutexType>
void ConditionVariableAny<MutexType>::NotifyOne() {
  if (waiters_.GetCountOfSleepies()) {
    WaitList::Lock lock(waiters_);
    waiters_.WakeupOne(lock);
  }
}

template <typename MutexType>
void ConditionVariableAny<MutexType>::NotifyAll() {
  if (waiters_.GetCountOfSleepies()) {
    WaitList::Lock lock(waiters_);
    waiters_.WakeupAll(lock);
  }
}

}  // namespace impl
}  // namespace engine
