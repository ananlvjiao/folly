/*
 * Copyright 2019-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/Portability.h>

#if FOLLY_HAS_COROUTINES

#include <folly/ScopeGuard.h>
#include <folly/experimental/coro/AsyncGenerator.h>
#include <folly/experimental/coro/Baton.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Task.h>
#include <folly/futures/Future.h>

#include <folly/portability/GTest.h>

#include <chrono>
#include <map>
#include <string>
#include <tuple>

TEST(AsyncGenerator, DefaultConstructedGeneratorIsEmpty) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    folly::coro::AsyncGenerator<int> g;
    auto result = co_await g.next();
    CHECK(!result);
  }());
}

TEST(AsyncGenerator, GeneratorDestroyedBeforeCallingBegin) {
  bool started = false;
  auto makeGenerator = [&]() -> folly::coro::AsyncGenerator<int> {
    started = true;
    co_return;
  };

  {
    auto gen = makeGenerator();
    (void)gen;
  }

  CHECK(!started);
}

TEST(AsyncGenerator, PartiallyConsumingSequenceDestroysObjectsInScope) {
  bool started = false;
  bool destroyed = false;
  auto makeGenerator = [&]() -> folly::coro::AsyncGenerator<int> {
    SCOPE_EXIT {
      destroyed = true;
    };
    started = true;
    co_yield 1;
    co_yield 2;
    co_return;
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    {
      auto gen = makeGenerator();
      CHECK(!started);
      CHECK(!destroyed);
      auto result = co_await gen.next();
      CHECK(started);
      CHECK(!destroyed);
      CHECK(result);
      CHECK_EQ(1, *result);
    }
    CHECK(destroyed);
  }());
}

TEST(AsyncGenerator, FullyConsumeSequence) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<int> {
    for (int i = 0; i < 4; ++i) {
      co_yield i;
    }
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK(result);
    CHECK_EQ(0, *result);
    result = co_await gen.next();
    CHECK(result);
    CHECK_EQ(1, *result);
    result = co_await gen.next();
    CHECK(result);
    CHECK_EQ(2, *result);
    result = co_await gen.next();
    CHECK(result);
    CHECK_EQ(3, *result);
    result = co_await gen.next();
    CHECK(!result);
  }());
}

namespace {
struct SomeError : std::exception {};
} // namespace

TEST(AsyncGenerator, ThrowExceptionBeforeFirstYield) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<int> {
    if (true) {
      throw SomeError{};
    }
    co_return;
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    bool caughtException = false;
    try {
      (void)co_await gen.next();
      CHECK(false);
    } catch (const SomeError&) {
      caughtException = true;
    }
    CHECK(caughtException);
  }());
}

TEST(AsyncGenerator, ThrowExceptionAfterFirstYield) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<int> {
    co_yield 42;
    throw SomeError{};
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK(result);
    CHECK_EQ(42, *result);
    bool caughtException = false;
    try {
      (void)co_await gen.next();
      CHECK(false);
    } catch (const SomeError&) {
      caughtException = true;
    }
    CHECK(caughtException);
  }());
}

TEST(AsyncGenerator, ConsumingManySynchronousElementsDoesNotOverflowStack) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<std::uint64_t> {
    for (std::uint64_t i = 0; i < 1'000'000; ++i) {
      co_yield i;
    }
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    std::uint64_t sum = 0;
    while (auto result = co_await gen.next()) {
      sum += *result;
    }
    CHECK_EQ(499999500000u, sum);
  }());
}

TEST(AsyncGenerator, ProduceResultsAsynchronously) {
  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    folly::Executor* executor = co_await folly::coro::co_current_executor;
    auto makeGenerator = [&]() -> folly::coro::AsyncGenerator<int> {
      using namespace std::literals::chrono_literals;
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
      co_await folly::futures::sleep(1ms);
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
      co_yield 1;
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
      co_await folly::futures::sleep(1ms);
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
      co_yield 2;
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
      co_await folly::futures::sleep(1ms);
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
    };

    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK_EQ(1, *result);
    result = co_await gen.next();
    CHECK_EQ(2, *result);
    result = co_await gen.next();
    CHECK(!result);
  }());
}

struct ConvertibleToIntReference {
  int value;
  operator int&() {
    return value;
  }
};

TEST(AsyncGenerator, GeneratorOfLValueReference) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<int&> {
    int value = 10;
    co_yield value;
    // Consumer gets a mutable reference to the value and can modify it.
    CHECK_EQ(20, value);

    // NOTE: Not allowed to yield an rvalue from an AsyncGenerator<T&>?
    // co_yield 30;  // Compile-error

    co_yield ConvertibleToIntReference{30};
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK_EQ(10, result.value());
    *result = 20;
    result = co_await gen.next();
    CHECK_EQ(30, result.value());
    result = co_await gen.next();
    CHECK(!result.has_value());
  }());
}

struct ConvertibleToInt {
  operator int() const {
    return 99;
  }
};

TEST(AsyncGenerator, GeneratorOfConstLValueReference) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<const int&> {
    int value = 10;
    co_yield value;
    // Consumer gets a const reference to the value.

    // Allowed to yield an rvalue from an AsyncGenerator<const T&>.
    co_yield 30;

    co_yield ConvertibleToInt{};
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK_EQ(10, *result);
    result = co_await gen.next();
    CHECK_EQ(30, *result);
    result = co_await gen.next();
    CHECK_EQ(99, *result);
    result = co_await gen.next();
    CHECK(!result);
  }());
}

TEST(AsyncGenerator, GeneratorOfRValueReference) {
  auto makeGenerator =
      []() -> folly::coro::AsyncGenerator<std::unique_ptr<int>&&> {
    co_yield std::make_unique<int>(10);

    auto ptr = std::make_unique<int>(20);
    co_yield std::move(ptr);
    CHECK(ptr == nullptr);
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();

    auto result = co_await gen.next();
    CHECK_EQ(10, **result);
    // Don't move it to a local var.

    result = co_await gen.next();
    CHECK_EQ(20, **result);
    auto ptr = *result; // Move it to a local var.

    result = co_await gen.next();
    CHECK(!result);
  }());
}

struct MoveOnly {
  explicit MoveOnly(int value) : value_(value) {}
  MoveOnly(MoveOnly&& other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  ~MoveOnly() {}
  MoveOnly& operator=(MoveOnly&&) = delete;
  int value() const {
    return value_;
  }

 private:
  int value_;
};

TEST(AsyncGenerator, GeneratorOfMoveOnlyType) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<MoveOnly> {
    MoveOnly rvalue(1);
    co_yield std::move(rvalue);
    CHECK_EQ(-1, rvalue.value());

    co_yield MoveOnly(2);
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();

    // NOTE: It's an error to dereference using '*it' as this returns a copy
    // of the iterator's reference type, which in this case is 'MoveOnly'.
    CHECK_EQ(1, result->value());

    result = co_await gen.next();
    CHECK_EQ(2, result->value());

    result = co_await gen.next();
    CHECK(!result);
  }());
}

TEST(AsyncGenerator, GeneratorOfConstValue) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<const int> {
    // OK to yield prvalue
    co_yield 42;

    // OK to yield lvalue
    int x = 123;
    co_yield x;

    co_yield ConvertibleToInt{};
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK_EQ(42, *result);
    static_assert(std::is_same_v<decltype(*result), const int&>);
    result = co_await gen.next();
    CHECK_EQ(123, *result);
    result = co_await gen.next();
    CHECK_EQ(99, *result);
    result = co_await gen.next();
    CHECK(!result);
  }());
}

TEST(AsyncGenerator, ExplicitValueType) {
  std::map<std::string, std::string> items;
  items["foo"] = "hello";
  items["bar"] = "goodbye";

  auto makeGenerator = [&]() -> folly::coro::AsyncGenerator<
                                 std::tuple<const std::string&, std::string&>,
                                 std::tuple<std::string, std::string>> {
    for (auto& [k, v] : items) {
      co_yield{k, v};
    }
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    {
      auto [kRef, vRef] = *result;
      CHECK_EQ("bar", kRef);
      CHECK_EQ("goodbye", vRef);
      decltype(gen)::value_type copy = *result;
      vRef = "au revoir";
      CHECK_EQ("goodbye", std::get<1>(copy));
      CHECK_EQ("au revoir", std::get<1>(*result));
    }
  }());

  CHECK_EQ("au revoir", items["bar"]);
}

TEST(AsyncGenerator, InvokeLambda) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto ptr = std::make_unique<int>(123);
    auto gen = folly::coro::co_invoke(
        [p = std::move(ptr)]() mutable
        -> folly::coro::AsyncGenerator<std::unique_ptr<int>&&> {
          co_yield std::move(p);
        });

    auto result = co_await gen.next();
    CHECK(result);
    ptr = *result;
    CHECK(ptr);
    CHECK(*ptr == 123);
  }());
}

#endif
