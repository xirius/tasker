#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;

void test_11(int& result)
{
    result += 1;
}

void test_21(const std::stop_token&, int& result)
{
    result += 1;
}

void test_22(std::stop_token, int& result)
{
    result += 1;
}

void test_23(const std::stop_token&, int& result) noexcept
{
    result += 1;
}

struct Foo
{
    static void bar_11(int& result)
    {
        result += 1;
    }

    static void bar_21(const std::stop_token&, int& result)
    {
        result += 1;
    }

    static void bar_22(std::stop_token, int& result)
    {
        result += 1;
    }

    static void bar_23(const std::stop_token&, int& result) noexcept
    {
        result += 1;
    }

    void mbar_12(const std::stop_token&, int& result)
    {
        result += 1;
    }

    void mbar_21(const std::stop_token&, int& result)
    {
        result += 1;
    }

    void mbar_22(std::stop_token, int& result)
    {
        result += 1;
    }

    void mbar_23(const std::stop_token&, int& result) const
    {
        result += 1;
    }

    void mbar_24(const std::stop_token&, int& result) const noexcept
    {
        result += 1;
    }
};

/// Tests
/// ================================================================================================

SCENARIO("Task::run #1 void<>", "[task runner]")
{
    GIVEN("A manual executor")
    {
        auto executor = ManualExecutor::create();
        int result = 0;

        WHEN("Task is a Lambda function")
        {
            Task::post(executor.get(), [&result] {
                result = 1; // Flag to confirm execution
            });

            executor->runPending(1);

            THEN("The count should be 0 and the result 1")
            {
                REQUIRE(executor->pendingCount() == 0);
                REQUIRE(result == 1);
            }
        }

        WHEN("Task is a free function #1")
        {
            Task::post(executor.get(), &test_11, std::ref(result));

            executor->runPending(1);

            THEN("The count should be 0 and the result 1")
            {
                REQUIRE(executor->pendingCount() == 0);
                REQUIRE(result == 1);
            }
        }

        WHEN("Task is a free function #2")
        {
            Task::run(executor.get(), &Foo::bar_11, std::ref(result)).submit();

            executor->runPending(1);

            THEN("The count should be 0 and the result 1")
            {
                REQUIRE(executor->pendingCount() == 0);
                REQUIRE(result == 1);
            }
        }

        WHEN("Task is a member function")
        {
            Foo foo;
            Task::post(executor.get(), &Foo::mbar_12, foo, std::ref(result));

            executor->runPending(1);

            THEN("The count should be 0 and the result 1")
            {
                REQUIRE(executor->pendingCount() == 0);
                REQUIRE(result == 1);
            }
        }
    }
}

SCENARIO("Task::run #2 void<[std::stop_token]>", "[task runner]")
{
    GIVEN("A manual executor")
    {
        auto executor = ManualExecutor::create();
        int result = 0;

        WHEN("Task is a Lambda function #1")
        {
            Task::post(executor.get(), [&result](const std::stop_token&) {
                result += 1; // Flag to confirm execution
            });

            Task::run(executor.get(), [&result](std::stop_token) {
                result += 1; // Flag to confirm execution
            }).submit();

            Task::post(executor.get(), [&result](const std::stop_token&) noexcept {
                result += 1; // Flag to confirm execution
            });

            executor->runPending(3);

            THEN("The count should be 0 and the result 3")
            {
                REQUIRE(executor->pendingCount() == 0);
                REQUIRE(result == 3);
            }
        }

        WHEN("Task is a free function #1")
        {
            Task::post(executor.get(), &test_21, std::ref(result));
            Task::post(executor.get(), &test_22, std::ref(result));
            Task::run(executor.get(), &test_23, std::ref(result)).submit();

            executor->runPending(3);

            THEN("The count should be 0 and the result 3")
            {
                REQUIRE(executor->pendingCount() == 0);
                REQUIRE(result == 3);
            }
        }

        WHEN("Task is a free function #3")
        {
            Task::post(executor.get(), &Foo::bar_21, std::ref(result));
            Task::post(executor.get(), &Foo::bar_22, std::ref(result));
            Task::post(executor.get(), &Foo::bar_23, std::ref(result));

            executor->runPending(3);

            THEN("The count should be 0 and the result 3")
            {
                REQUIRE(executor->pendingCount() == 0);
                REQUIRE(result == 3);
            }
        }

        WHEN("Task is a member function")
        {
            Foo foo;
            Task::post(executor.get(), &Foo::mbar_21, foo, std::ref(result));
            Task::post(executor.get(), &Foo::mbar_22, &foo, std::ref(result));
            Task::run(executor.get(), &Foo::mbar_23, std::ref(foo), std::ref(result)).submit();
            Task::run(executor.get(), &Foo::mbar_24, foo, std::ref(result)).submit();

            executor->runPending(4);

            THEN("The count should be 0 and the result 4")
            {
                REQUIRE(executor->pendingCount() == 0);
                REQUIRE(result == 4);
            }
        }
    }
}

SCENARIO("Task::run #3 Result<void> -> Result<Arg>", "[task runner]")
{
    GIVEN("A manual executor")
    {
        const auto executor = ManualExecutor::create();

        WHEN("The result is taken from a 1 level chain")
        {
            auto handle = Task::run(executor.get(), []() { return 123; }).submit();

            executor->runPending(1);

            THEN("The count should be 0 and the result 123")
            {
                REQUIRE(executor->pendingCount() == 0);
                REQUIRE(handle.get() == 123);
            }
        }

        WHEN("The result is taken from a 2 level chain")
        {
            auto handle = Task::run(executor.get(), [] {
                return 1.23; // pass to the next task
            }).then(executor.get(), [](const double arg) {
                return arg * 3.21;
            }).submit();

            executor->runPending(2);

            THEN("The count should be 0 and the result 123")
            {
                REQUIRE(executor->pendingCount() == 0);
                REQUIRE(handle.get() == 1.23 * 3.21);
            }
        }
    }
}

TEST_CASE("Run task: Chaining tasks", "[task]")
{
    const auto executor = ManualExecutor::create();
    int result{0};

    Task::run(executor.get(), []() { return 123; }).then(executor.get(), [&result](const int x) {
        result = x;
    }).then(executor.get(), [&result]() {
        result = result * 2;
    }).submit();

    executor->runPending(3);

    REQUIRE(result == 123 * 2);
}
