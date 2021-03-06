// Copyright (c) 2013-2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "mutex.h"
#include "debug.h"
#include "thread.h"
#ifndef _WIN32
#include <pthread.h>
#else
#include <atomic>
#endif
#include "platform.h"
#include <gtest/gtest.h>

namespace kj {
namespace {

#ifndef _WIN32
inline void delay() { usleep(10000); }
#else
inline void delay() { Sleep(10); }
#endif

#if KJ_NO_EXCEPTIONS
#undef EXPECT_ANY_THROW
#define EXPECT_ANY_THROW(code) EXPECT_DEATH(code, ".")
#define EXPECT_NONFATAL_FAILURE(code) code
#else
#define EXPECT_NONFATAL_FAILURE EXPECT_ANY_THROW
#endif

#ifdef KJ_DEBUG
#define EXPECT_DEBUG_ANY_THROW EXPECT_ANY_THROW
#else
#define EXPECT_DEBUG_ANY_THROW(EXP)
#endif

TEST(Mutex, MutexGuarded) {
  MutexGuarded<uint> value(123);

  {
    Locked<uint> lock = value.lockExclusive();
    EXPECT_EQ(123u, *lock);
    EXPECT_EQ(123u, value.getAlreadyLockedExclusive());

    Thread thread([&]() {
      Locked<uint> threadLock = value.lockExclusive();
      EXPECT_EQ(456u, *threadLock);
      *threadLock = 789;
    });

    delay();
    EXPECT_EQ(123u, *lock);
    *lock = 456;
    auto earlyRelease = kj::mv(lock);
  }

  EXPECT_EQ(789u, *value.lockExclusive());

  {
    auto rlock1 = value.lockShared();
    EXPECT_EQ(789u, *rlock1);
    EXPECT_EQ(789u, value.getAlreadyLockedShared());

    {
      auto rlock2 = value.lockShared();
      EXPECT_EQ(789u, *rlock2);
      auto rlock3 = value.lockShared();
      EXPECT_EQ(789u, *rlock3);
      auto rlock4 = value.lockShared();
      EXPECT_EQ(789u, *rlock4);
    }

    Thread thread2([&]() {
      Locked<uint> threadLock = value.lockExclusive();
      *threadLock = 321;
    });

#if KJ_USE_FUTEX
    // So, it turns out that pthread_rwlock on BSD "prioritizes" readers over writers.  The result
    // is that if one thread tries to take multiple read locks, but another thread happens to
    // request a write lock it between, you get a deadlock.  This seems to contradict the man pages
    // and common sense, but this is how it is.  The futex-based implementation doesn't currently
    // have this problem because it does not prioritize writers.  Perhaps it will in the future,
    // but we'll leave this test here until then to make sure we notice the change.

    delay();
    EXPECT_EQ(789u, *rlock1);

    {
      auto rlock2 = value.lockShared();
      EXPECT_EQ(789u, *rlock2);
      auto rlock3 = value.lockShared();
      EXPECT_EQ(789u, *rlock3);
      auto rlock4 = value.lockShared();
      EXPECT_EQ(789u, *rlock4);
    }
#endif

    delay();
    EXPECT_EQ(789u, *rlock1);
    auto earlyRelease = kj::mv(rlock1);
  }

  EXPECT_EQ(321u, *value.lockExclusive());

  EXPECT_DEBUG_ANY_THROW(value.getAlreadyLockedExclusive());
  EXPECT_DEBUG_ANY_THROW(value.getAlreadyLockedShared());
  EXPECT_EQ(321u, value.getWithoutLock());
}

TEST(Mutex, Lazy) {
  Lazy<uint> lazy;
#ifndef WIN32
  bool initStarted = false;
#else
  std::atomic<bool> initStarted = false;
#endif

  Thread thread([&]() {
#ifndef WIN32
    EXPECT_EQ(123u, lazy.get([&](SpaceFor<uint>& space) -> Own<uint> {
      __atomic_store_n(&initStarted, true, __ATOMIC_RELAXED);
      delay();
      return space.construct(123);
    }));
#else
    EXPECT_EQ(123u, lazy.get([&](SpaceFor<uint>& space) -> Own<uint> {
      std::atomic_store_explicit(&initStarted, true, std::memory_order_relaxed);
      delay();
      return space.construct(123);
    }));
#endif
  });

  // Spin until the initializer has been entered in the thread.
#ifndef WIN32
  while (!__atomic_load_n(&initStarted, __ATOMIC_RELAXED)) {
    sched_yield();
  }
#else
  while (!std::atomic_load_explicit(&initStarted, std::memory_order_relaxed)) {
    SwitchToThread();
  }
#endif

  EXPECT_EQ(123u, lazy.get([](SpaceFor<uint>& space) { return space.construct(456); }));
  EXPECT_EQ(123u, lazy.get([](SpaceFor<uint>& space) { return space.construct(789); }));
}

TEST(Mutex, LazyException) {
  Lazy<uint> lazy;

  auto exception = kj::runCatchingExceptions([&]() {
    lazy.get([&](SpaceFor<uint>& space) -> Own<uint> {
          KJ_FAIL_ASSERT("foo") { break; }
          return space.construct(123);
        });
  });
  EXPECT_TRUE(exception != nullptr);

  uint i = lazy.get([&](SpaceFor<uint>& space) -> Own<uint> {
        return space.construct(456);
      });

  // Unfortunately, the results differ depending on whether exceptions are enabled.
  // TODO(someday):  Fix this?  Does it matter?
#if KJ_NO_EXCEPTIONS
  EXPECT_EQ(123, i);
#else
  EXPECT_EQ(456, i);
#endif
}

}  // namespace
}  // namespace kj
