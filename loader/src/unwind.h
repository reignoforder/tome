#pragma once

// Cleanups that also run when a crash guard abandons a call (guard.h).
//
// A guard resumes after a crash by unwinding: RtlUnwind calls the handler of
// every SEH frame between the fault and the guard, then the stack is cut back
// to the guard. Code in those frames never finishes, so a lock it held would
// stay held and memory it owned would leak. An OnUnwind scope registers an
// SEH frame of its own whose handler runs the cleanup during that unwind,
// like a __finally block; when the scope ends normally it's just unlinked.
// The locks below are built on it: every lock Tome takes is one of them.

#include <windows.h>

#include <utility>

namespace tome {

namespace unwind {

// An SEH registration record with the cleanup it runs on unwind.
struct Record {
    Record* next;
    void* handler;
    void (*fn)(void*);
    void* context;
};

// Links `r` into fs:[0] / unlinks it (it must be the innermost record).
void push(Record& r);
void pop(Record& r);

} // namespace unwind

// Runs `f` if a crash guard unwinds through this scope; nothing otherwise.
template <typename F>
class OnUnwind {
public:
    explicit OnUnwind(F f) : f_(std::move(f)) {
        record_.fn = [](void* self) { static_cast<OnUnwind*>(self)->f_(); };
        record_.context = this;
        unwind::push(record_);
    }
    ~OnUnwind() { unwind::pop(record_); }
    OnUnwind(const OnUnwind&) = delete;
    OnUnwind& operator=(const OnUnwind&) = delete;

private:
    unwind::Record record_;
    F f_;
};

// An SRW lock held for a scope. unlock() releases it early; a crash guard
// unwinding through the scope releases it too.
class ExclusiveLock {
public:
    explicit ExclusiveLock(SRWLOCK& lock) : lock_(&lock) {
        AcquireSRWLockExclusive(lock_);
        record_.fn = [](void* self) { static_cast<ExclusiveLock*>(self)->unlock(); };
        record_.context = this;
        unwind::push(record_);
    }
    ~ExclusiveLock() {
        unwind::pop(record_);
        unlock();
    }
    void unlock() {
        if (lock_) ReleaseSRWLockExclusive(std::exchange(lock_, nullptr));
    }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;

private:
    SRWLOCK* lock_;
    unwind::Record record_;
};

class SharedLock {
public:
    explicit SharedLock(SRWLOCK& lock) : lock_(&lock) {
        AcquireSRWLockShared(lock_);
        record_.fn = [](void* self) { static_cast<SharedLock*>(self)->unlock(); };
        record_.context = this;
        unwind::push(record_);
    }
    ~SharedLock() {
        unwind::pop(record_);
        unlock();
    }
    void unlock() {
        if (lock_) ReleaseSRWLockShared(std::exchange(lock_, nullptr));
    }
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;

private:
    SRWLOCK* lock_;
    unwind::Record record_;
};

} // namespace tome
