#include <vanilo/tasker/Tasker.h>

#include <catch2/catch.hpp>

using namespace vanilo::tasker;

TEST_CASE("Run task: Result<Arg> without taking the result", "[task]")
{
    double result  = 0;
    auto task      = [&result](double value) { return result = 3.21 * value; };
    auto invokable = std::make_unique<details::Invocable<decltype(task), double, double>>(nullptr, std::move(task));

    invokable->setArgument(1.23);
    invokable->run();

    REQUIRE(result == 1.23 * 3.21);
}

TEST_CASE("Run task: Result<Arg> and take std::future<Result>", "[task]")
{
    auto task      = [](double value) { return 3.21 * value; };
    auto invokable = std::make_unique<details::Invocable<decltype(task), double, double>>(nullptr, std::move(task));
    auto future    = invokable->getFuture();

    invokable->setArgument(1.23);
    invokable->run();

    REQUIRE(future.get() == 1.23 * 3.21);
}
