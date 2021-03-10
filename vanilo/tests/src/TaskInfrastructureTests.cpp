#include <vanilo/tasker/Tasker.h>

#include <catch2/catch.hpp>

using namespace vanilo::tasker;

TEST_CASE("Run task: Result<Arg> without taking the result", "[task]")
{
    double result  = 0;
    auto task      = [&result](double value) { return result = 3.21 * value; };
    auto invokable = std::make_unique<internal::Invocable<decltype(task), double, double>>(nullptr, std::move(task));

    invokable->setArgument(1.23);
    invokable->run();

    REQUIRE(result == 1.23 * 3.21);
}

TEST_CASE("Run task: void<void> and take std::future<void>", "[task]")
{
    auto value     = 0;
    auto task      = [&value]() { value = 1; };
    auto invokable = std::make_unique<internal::Invocable<decltype(task), void, void, true>>(nullptr, std::move(task));
    auto future    = invokable->getFuture();

    invokable->run();
    future.get();

    REQUIRE(value == 1);
}

TEST_CASE("Run task: void<Arg> and take std::future<void>", "[task]")
{
    auto value     = 0;
    auto task      = [&value](int val) { value = val; };
    auto invokable = std::make_unique<internal::Invocable<decltype(task), void, int, true>>(nullptr, std::move(task));
    auto future    = invokable->getFuture();

    invokable->setArgument(1);
    invokable->run();
    future.get();

    REQUIRE(value == 1);
}

TEST_CASE("Run task: Result<void> and take std::future<Result>", "[task]")
{
    auto task      = []() { return 123; };
    auto invokable = std::make_unique<internal::Invocable<decltype(task), int, void, true>>(nullptr, std::move(task));
    auto future    = invokable->getFuture();

    invokable->run();

    REQUIRE(future.get() == 123);
}

TEST_CASE("Run task: Result<Arg> and take std::future<Result>", "[task]")
{
    auto task      = [](double value) { return value * 3.21; };
    auto invokable = std::make_unique<internal::Invocable<decltype(task), double, double, true>>(nullptr, std::move(task));
    auto future    = invokable->getFuture();

    invokable->setArgument(1.23);
    invokable->run();

    REQUIRE(future.get() == 1.23 * 3.21);
}

TEST_CASE("Run task: void<void> and take std::future<void> with exception", "[task]")
{
    auto task      = []() { throw std::runtime_error{"Something happened"}; };
    auto invokable = std::make_unique<internal::Invocable<decltype(task), void, void, true>>(nullptr, std::move(task));
    auto future    = invokable->getFuture();

    invokable->run();

    CHECK_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("Run task: void<void> chained and take std::future<void> with exception", "[task]")
{
    auto task1      = []() { throw std::runtime_error{"Something happened"}; };
    auto task2      = []() {};
    auto invokable1 = std::make_unique<internal::Invocable<decltype(task1), void, void, false>>(nullptr, std::move(task1));
    auto invokable2 = std::make_unique<internal::Invocable<decltype(task2), void, void, true>>(nullptr, std::move(task2));
    auto future     = invokable2->getFuture();

    invokable1->setNext(std::move(invokable2));
    invokable1->run();

    CHECK_THROWS_AS(future.get(), std::runtime_error);
}
