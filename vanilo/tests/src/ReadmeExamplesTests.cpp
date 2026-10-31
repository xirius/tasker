#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <string>
#include <vector>

// The examples of README.md, so they keep compiling and doing what the README says.

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::UnhandledExceptions;
using vanilo::tests::waitFor;

TEST_CASE("README: the introduction example", "[readme]")
{
    auto pool = ThreadPoolExecutor::create();

    auto answer = Task::run(pool.get(), [] { return 6; }).then(pool.get(), [](int value) { return value * 7; }).submit();

    REQUIRE(answer.get() == 42);
}

TEST_CASE("README: executors", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(4);
    pool->resize(8).wait();
    REQUIRE(pool->threadCount() == 8);

    auto loop = ManualExecutor::create();
    REQUIRE(loop->runPending(10) == 0);
}

TEST_CASE("README: an event loop on a ManualExecutor", "[readme]")
{
    auto loop = ManualExecutor::create();
    std::stop_source stop;
    std::atomic<bool> ran{false};

    std::thread producer{[&] {
        Task::post(loop.get(), [&ran, &stop] {
            ran = true;
            stop.request_stop(); // ends the loop below
        });
    }};

    loop->runUntil(stop.get_token());
    producer.join();

    REQUIRE(ran);
}

TEST_CASE("README: arguments are copied, std::ref changes in place", "[readme]")
{
    auto loop = ManualExecutor::create();
    int counter = 0;

    Task::post(loop.get(), [](int& value, int step) { value += step; }, std::ref(counter), 5);
    loop->runPending();

    REQUIRE(counter == 5);
}

namespace {
    struct Thermostat
    {
        void setTarget(double celsius)
        {
            target = celsius;
        }

        double target = 0;
    };
} // namespace

TEST_CASE("README: member functions on an existing object", "[readme]")
{
    auto loop = ManualExecutor::create();
    Thermostat thermostat;

    Task::post(loop.get(), &Thermostat::setTarget, &thermostat, 21.5);
    loop->runPending();

    REQUIRE(thermostat.target == 21.5);
}

TEST_CASE("README: a chain across executors", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(2);
    auto loop = ManualExecutor::create();
    auto readSensor = [] { return 22.5; };

    auto tooWarm = Task::run(pool.get(), readSensor).then(loop.get(), [](double celsius) { return celsius > 21; }).submit();

    REQUIRE(waitFor([&] {
        loop->runPending();
        return tooWarm.isReady();
    }));
    REQUIRE(tooWarm.get());
}

TEST_CASE("README: one handler for the errors of a chain", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(2);
    std::atomic<bool> logged{false};
    auto download = []() -> std::string { throw std::runtime_error{"offline"}; };

    Task::run(pool.get(), download).then(pool.get(), [](const std::string&) {
    }).onError([&logged](const std::exception& ex) {
        logged = std::string{ex.what()} == "offline";
    }).submit();

    REQUIRE(waitFor([&logged] { return logged.load(); }));
}

namespace {
    struct Device
    {
        void connect()
        {
            throw std::runtime_error{"no answer"};
        }

        void markOffline(const std::string& reason)
        {
            offlineReason = reason;
        }

        std::string offlineReason;
    };
} // namespace

TEST_CASE("README: a handler on another executor", "[readme]")
{
    auto loop = ManualExecutor::create();
    auto pool = ThreadPoolExecutor::create(1);
    Device device;
    std::atomic<bool> handled{false};

    Task::run(loop.get(), &Device::connect, &device)
        .onError(pool.get(), [&device, &handled](const std::exception& ex) {
        device.markOffline(ex.what());
        handled = true;
    }).submit();

    loop->runPending();
    REQUIRE(waitFor([&handled] { return handled.load(); }));
    REQUIRE(device.offlineReason == "no answer");
}

TEST_CASE("README: a custom unhandled-exception handler", "[readme]")
{
    const UnhandledExceptions unhandled; // installs a recording handler like the README's
    auto loop = ManualExecutor::create();

    Task::post(loop.get(), [] { throw std::runtime_error{"nobody handles this"}; });
    loop->runPending();

    REQUIRE(unhandled.messages() == std::vector<std::string>{"nobody handles this"});
}

TEST_CASE("README: a task that stops halfway when canceled", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(1);
    std::stop_source source;
    std::atomic<int> processed{0};
    const std::vector<int> chunks(1000, 1);

    auto handle = Task::run(pool.get(), source.get_token(), [&chunks, &processed, &source](const std::stop_token& token) {
        for (const auto& chunk : chunks) {
            throwIfStopRequested(token);
            processed += chunk;
            if (processed == 10) {
                source.request_stop();
            }
        }
    }).submit();

    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    REQUIRE(processed == 10);
}

TEST_CASE("README: canceling through the handle", "[readme]")
{
    auto loop = ManualExecutor::create();
    bool ran = false;

    auto handle = Task::run(loop.get(), [&ran] { ran = true; }).submit();
    handle.cancel();
    loop->runPending();

    REQUIRE_FALSE(ran);
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("README: delayed and periodic tasks", "[readme]")
{
    auto loop = ManualExecutor::create();
    std::stop_source stop;
    std::atomic<int> once{0};
    std::atomic<int> repeated{0};

    Task::run(loop.get(), stop.get_token(), 5ms, [&once] { ++once; }).submit();
    Task::run(loop.get(), stop.get_token(), 0ms, 5ms, [&repeated] { ++repeated; }).onError([](const std::exception&) {}).submit();

    REQUIRE(waitFor([&] {
        loop->runPending();
        return once == 1 && repeated >= 2;
    }));
    stop.request_stop();
}
