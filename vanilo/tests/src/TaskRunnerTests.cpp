#include <vanilo/tasker/Tasker.h>

#include <catch2/catch.hpp>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;

void test_11(int& result)
{
    result += 1;
}

void test_21(CancellationToken& token, int& result)
{
    result += 1;
}

void test_22(const CancellationToken& token, int& result)
{
    result += 1;
}

void test_23(const CancellationToken& token, int& result) noexcept
{
    result += 1;
}

struct Foo
{
    static void bar_11(int& result)
    {
        result += 1;
    }

    static void bar_21(CancellationToken& token, int& result)
    {
        result += 1;
    }

    static void bar_22(const CancellationToken& token, int& result)
    {
        result += 1;
    }

    static void bar_23(const CancellationToken& token, int& result) noexcept
    {
        result += 1;
    }

    void mbar_12(CancellationToken& token, int& result)
    {
        result += 1;
    }

    void mbar_21(CancellationToken& token, int& result)
    {
        result += 1;
    }

    void mbar_22(const CancellationToken& token, int& result)
    {
        result += 1;
    }

    void mbar_23(const CancellationToken& token, int& result) const
    {
        result += 1;
    }

    void mbar_24(const CancellationToken& token, int& result) const noexcept
    {
        result += 1;
    }
};

/// Tests
/// ================================================================================================

SCENARIO("Task::run #1 void<>", "[task runner]")
{
    GIVEN("A local executor")
    {
        auto executor = LocalThreadExecutor::create();
        int result    = 0;
        Foo foo;

        WHEN("Task is a Lambda function")
        {
            Task::run(executor.get(), [&]() {
                result = 1; // Flag to confirm execution
            });

            executor->process(1);

            THEN("The count should be 0 and the result 1")
            {
                REQUIRE(executor->count() == 0);
                REQUIRE(result == 1);
            }
        }

        WHEN("Task is a free function #1")
        {
            Task::run(executor.get(), &test_11, std::ref(result));

            executor->process(1);

            THEN("The count should be 0 and the result 1")
            {
                REQUIRE(executor->count() == 0);
                REQUIRE(result == 1);
            }
        }

        WHEN("Task is a free function #2")
        {
            Task::run(executor.get(), &Foo::bar_11, std::ref(result));

            executor->process(1);

            THEN("The count should be 0 and the result 1")
            {
                REQUIRE(executor->count() == 0);
                REQUIRE(result == 1);
            }
        }

        WHEN("Task is a member function")
        {
            Task::run(executor.get(), &Foo::mbar_12, foo, std::ref(result));

            executor->process(1);

            THEN("The count should be 0 and the result 1")
            {
                REQUIRE(executor->count() == 0);
                REQUIRE(result == 1);
            }
        }
    }
}

SCENARIO("Task::run #2 void<[CancellationToken]>", "[task runner]")
{
    GIVEN("A local executor")
    {
        auto executor = LocalThreadExecutor::create();
        int result    = 0;
        Foo foo;

        WHEN("Task is a Lambda function #1")
        {
            Task::run(executor.get(), [&](CancellationToken& token) {
                result += 1; // Flag to confirm execution
            });

            Task::run(executor.get(), [&](const CancellationToken& token) {
                result += 1; // Flag to confirm execution
            });

            Task::run(executor.get(), [&](const CancellationToken& token) noexcept {
                result += 1; // Flag to confirm execution
            });

            executor->process(3);

            THEN("The count should be 0 and the result 3")
            {
                REQUIRE(executor->count() == 0);
                REQUIRE(result == 3);
            }
        }

        WHEN("Task is a free function #1")
        {
            Task::run(executor.get(), &test_21, std::ref(result));
            Task::run(executor.get(), &test_22, std::ref(result));
            Task::run(executor.get(), &test_23, std::ref(result));

            executor->process(3);

            THEN("The count should be 0 and the result 3")
            {
                REQUIRE(executor->count() == 0);
                REQUIRE(result == 3);
            }
        }

        WHEN("Task is a free function #3")
        {
            Task::run(executor.get(), &Foo::bar_21, std::ref(result));
            Task::run(executor.get(), &Foo::bar_22, std::ref(result));
            Task::run(executor.get(), &Foo::bar_23, std::ref(result));

            executor->process(3);

            THEN("The count should be 0 and the result 3")
            {
                REQUIRE(executor->count() == 0);
                REQUIRE(result == 3);
            }
        }

        WHEN("Task is a member function")
        {
            Task::run(executor.get(), &Foo::mbar_21, foo, std::ref(result));
            Task::run(executor.get(), &Foo::mbar_22, foo, std::ref(result));
            Task::run(executor.get(), &Foo::mbar_23, foo, std::ref(result));
            Task::run(executor.get(), &Foo::mbar_24, foo, std::ref(result));

            executor->process(4);

            THEN("The count should be 0 and the result 4")
            {
                REQUIRE(executor->count() == 0);
                REQUIRE(result == 4);
            }
        }
    }
}

SCENARIO("Task::run #3 Result<void> -> Result<Arg>", "[task runner]")
{
    GIVEN("A local executor")
    {
        auto executor = LocalThreadExecutor::create();

        WHEN("Future is taken from 1 level task #1")
        {
            auto future = Task::run(executor.get(), []() { return 123; }).getFuture();

            executor->process(1);

            THEN("The count should be 0 and the result 123")
            {
                REQUIRE(executor->count() == 0);
                REQUIRE(future.get() == 123);
            }
        }

        WHEN("Future is taken from 2 level task #1")
        {
            auto future = Task::run(executor.get(), []() {
                              return 1.23; // pass to the next task
                          }).then(executor.get(), [](double arg) {
                                return arg * 3.21;
                            }).getFuture();

            executor->process(2);

            THEN("The count should be 0 and the result 123")
            {
                REQUIRE(executor->count() == 0);
                REQUIRE(future.get() == 1.23 * 3.21);
            }
        }
    }
}