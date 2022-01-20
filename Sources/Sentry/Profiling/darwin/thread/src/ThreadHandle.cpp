// Copyright (c) Specto Inc. All rights reserved.

#include "ThreadHandle.h"

#include "MemorySafety.h"
#include "DarwinLog.h"
#include "Log.h"

#include <cstdint>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <pthread.h>

namespace specto::darwin {

ThreadHandle::ThreadHandle(NativeHandle handle, bool isOwnedPort) noexcept :
    handle_(handle), isOwnedPort_(isOwnedPort), pthreadHandle_(nullptr) { }

ThreadHandle::ThreadHandle(NativeHandle handle) noexcept :
    ThreadHandle(handle, false /* isOwnedPort */) { }

ThreadHandle::~ThreadHandle() {
    // If the ThreadHandle object owns the mach_port (i.e. with a +1 reference count)
    // the port must be deallocated.
    if (isOwnedPort_) {
        SPECTO_LOG_KERN_RETURN(mach_port_deallocate(mach_task_self(), handle_));
    }
}

std::unique_ptr<ThreadHandle> ThreadHandle::current() noexcept {
    const auto port = mach_thread_self();
    SPECTO_LOG_KERN_RETURN(mach_port_deallocate(mach_task_self(), port));
    return std::make_unique<ThreadHandle>(port);
}

std::vector<std::unique_ptr<ThreadHandle>> ThreadHandle::all() noexcept {
    std::vector<std::unique_ptr<ThreadHandle>> threads;
    mach_msg_type_number_t count;
    thread_act_array_t list;
    if (SPECTO_LOG_KERN_RETURN(task_threads(mach_task_self(), &list, &count)) == KERN_SUCCESS) {
        for (decltype(count) i = 0; i < count; i++) {
            const auto thread = list[i];
            threads.push_back(
              std::unique_ptr<ThreadHandle>(new ThreadHandle(thread, true /* isOwnedPort */)));
        }
    }
    SPECTO_LOG_KERN_RETURN(
      vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), sizeof(*list) * count));
    return threads;
}

std::pair<std::vector<std::unique_ptr<ThreadHandle>>, std::unique_ptr<ThreadHandle>>
  ThreadHandle::allExcludingCurrent() noexcept {
    std::vector<std::unique_ptr<ThreadHandle>> threads;
    mach_msg_type_number_t count;
    thread_act_array_t list;
    auto current = ThreadHandle::current();
    if (SPECTO_LOG_KERN_RETURN(task_threads(mach_task_self(), &list, &count)) == KERN_SUCCESS) {
        for (decltype(count) i = 0; i < count; i++) {
            const auto thread = list[i];
            if (thread != current->nativeHandle()) {
                threads.push_back(
                  std::unique_ptr<ThreadHandle>(new ThreadHandle(thread, true /* isOwnedPort */)));
            } else {
                SPECTO_LOG_KERN_RETURN(mach_port_deallocate(mach_task_self(), thread));
            }
        }
    }
    SPECTO_LOG_KERN_RETURN(
      vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), sizeof(*list) * count));
    return std::make_pair(std::move(threads), std::move(current));
}

thread::TIDType ThreadHandle::tidFromNativeHandle(NativeHandle handle) {
    return static_cast<std::uint64_t>(handle);
}

ThreadHandle::NativeHandle ThreadHandle::nativeHandle() const noexcept {
    return handle_;
}

thread::TIDType ThreadHandle::tid() const noexcept {
    return tidFromNativeHandle(handle_);
}

std::string ThreadHandle::name() const noexcept {
    const auto handle = pthreadHandle();
    if (handle == nullptr) {
        return {};
    }
    char name[128];
    if (SPECTO_LOG_ERROR_RETURN(pthread_getname_np(handle, name, sizeof(name))) == 0) {
        return std::string(name);
    }
    return {};
}

std::string ThreadHandle::dispatchQueueLabel() const noexcept {
#if defined(SPECTO_ENV_PRODUCTION)
    return {};
#else
    if (handle_ == THREAD_NULL) {
        return {};
    }
    integer_t infoBuffer[THREAD_IDENTIFIER_INFO_COUNT] = {0};
    thread_info_t info = infoBuffer;
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    const auto rv = thread_info(handle_, THREAD_IDENTIFIER_INFO, info, &count);
    const auto idInfo = reinterpret_cast<thread_identifier_info_t>(info);
    // MACH_SEND_INVALID_DEST is returned when the thread no longer exists
    if ((rv != MACH_SEND_INVALID_DEST) && (SPECTO_LOG_KERN_RETURN(rv) == KERN_SUCCESS)
        && isMemoryReadable(idInfo, sizeof(*idInfo))) {
        const auto queuePtr = reinterpret_cast<dispatch_queue_t *>(idInfo->dispatch_qaddr);
        if (queuePtr != nullptr && isMemoryReadable(queuePtr, sizeof(*queuePtr))
            && idInfo->thread_handle != 0 && *queuePtr != nullptr) {
            const auto label = dispatch_queue_get_label(*queuePtr);
            if (label != nullptr) {
                return std::string(label);
            }
        }
    }
    return {};
#endif
}

int ThreadHandle::priority() const noexcept {
    const auto handle = pthreadHandle();
    if (handle == nullptr) {
        return -1;
    }
    struct sched_param param;
    if (SPECTO_LOG_ERROR_RETURN(pthread_getschedparam(handle, nullptr, &param)) == 0) {
        return param.sched_priority;
    }
    return -1;
}

QoS ThreadHandle::qos() const noexcept {
    const auto handle = pthreadHandle();
    if (handle == nullptr) {
        return {};
    }
    qos_class_t qosClass;
    int relativePriority;
    if (SPECTO_LOG_ERROR_RETURN(pthread_get_qos_class_np(handle, &qosClass, &relativePriority))
        == 0) {
        return {qosClass, relativePriority};
    }
    return {};
}

namespace {
ThreadRunState runStateFromRawValue(integer_t state) {
    switch (state) {
        case TH_STATE_RUNNING:
            return ThreadRunState::Running;
        case TH_STATE_STOPPED:
            return ThreadRunState::Stopped;
        case TH_STATE_WAITING:
            return ThreadRunState::Waiting;
        case TH_STATE_UNINTERRUPTIBLE:
            return ThreadRunState::Uninterruptible;
        case TH_STATE_HALTED:
            return ThreadRunState::Halted;
        default:
            return ThreadRunState::Undefined;
    }
}
} // namespace

ThreadCPUInfo ThreadHandle::cpuInfo() const noexcept {
    if (handle_ == THREAD_NULL) {
        return {};
    }
    ThreadCPUInfo cpuInfo;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    thread_basic_info_data_t data;
    const auto rv =
      thread_info(handle_, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&data), &count);
    // MACH_SEND_INVALID_DEST is returned when the thread no longer exists
    if ((rv != MACH_SEND_INVALID_DEST) && (SPECTO_LOG_KERN_RETURN(rv) == KERN_SUCCESS)) {
        cpuInfo.userTimeMicros = std::chrono::seconds(data.user_time.seconds)
                                 + std::chrono::microseconds(data.user_time.microseconds);
        cpuInfo.systemTimeMicros = std::chrono::seconds(data.system_time.seconds)
                                   + std::chrono::microseconds(data.system_time.microseconds);
        cpuInfo.usagePercent = static_cast<float>(data.cpu_usage) / TH_USAGE_SCALE;
        cpuInfo.runState = runStateFromRawValue(data.run_state);
        cpuInfo.idle = ((data.flags & TH_FLAGS_IDLE) == TH_FLAGS_IDLE);
    }
    return cpuInfo;
}

bool ThreadHandle::isIdle() const noexcept {
    if (handle_ == THREAD_NULL) {
        return true;
    }
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    thread_basic_info_data_t data;
    const auto rv =
      thread_info(handle_, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&data), &count);
    // MACH_SEND_INVALID_DEST is returned when the thread no longer exists
    if ((rv != MACH_SEND_INVALID_DEST) && (SPECTO_LOG_KERN_RETURN(rv) == KERN_SUCCESS)) {
        return ((data.flags & TH_FLAGS_IDLE) == TH_FLAGS_IDLE)
               || (data.run_state != TH_STATE_RUNNING);
    }
    return true;
}

StackBounds ThreadHandle::stackBounds() const noexcept {
    const auto handle = pthreadHandle();
    if (handle == nullptr) {
        return {};
    }
    const auto start = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(handle));
    const auto end = start - pthread_get_stacksize_np(handle);
    return {start, end};
}

bool ThreadHandle::suspend() const noexcept {
    if (handle_ == THREAD_NULL) {
        return false;
    }
    return SPECTO_LOG_KERN_RETURN(thread_suspend(handle_)) == KERN_SUCCESS;
}

bool ThreadHandle::resume() const noexcept {
    if (handle_ == THREAD_NULL) {
        return false;
    }
    return SPECTO_LOG_KERN_RETURN(thread_resume(handle_)) == KERN_SUCCESS;
}

bool ThreadHandle::operator==(const ThreadHandle &other) const {
    return handle_ == other.handle_;
}

pthread_t ThreadHandle::pthreadHandle() const noexcept {
    if (pthreadHandle_ == nullptr) {
        // We cache this because the implementation of this function requires taking
        // a lock and iterating over a list of threads:
        // https://github.com/apple/darwin-libpthread/blob/master/src/pthread.c#L945
        pthreadHandle_ = pthread_from_mach_thread_np(handle_);
        if (pthreadHandle_ == nullptr) {
            // The thread no longer exists; this is not a recoverable failure so there's nothing
            // more we can do here.
            SPECTO_LOG_DEBUG("Failed to get pthread handle for mach thread {}", handle_);
        }
    }
    return pthreadHandle_;
}

} // namespace specto::darwin
