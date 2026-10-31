#include "TestUtilities.h"

#include <vanilo/concurrent/ShutdownGate.h>
#include <vanilo/core/Tracer.h>
#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <variant>
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

TEST_CASE("README: building a chain in steps", "[readme]")
{
    auto loop = ManualExecutor::create();

    auto chain = Task::run(loop.get(), [] { return 1; });
    auto handle = std::move(chain).submit();
    loop->runPending();

    REQUIRE(handle.get() == 1);
}

TEST_CASE("README: a chain across executors", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(2);
    auto loop = ManualExecutor::create();
    auto readSensor = [] { return 22.5; };

    auto tooWarm =
        Task::run(pool.get(), [&readSensor] { return readSensor(); }).then(loop.get(), [](double celsius) {
        return celsius > 21;
    }).submit();

    REQUIRE(waitFor([&] {
        loop->runPending();
        return tooWarm.isReady();
    }));
    REQUIRE(tooWarm.get());
}

TEST_CASE("README: one handler for the errors of a chain, and a recovery", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(2);
    auto loop = ManualExecutor::create();
    std::atomic<bool> logged{false};
    auto download = []() -> std::string { throw std::runtime_error{"offline"}; };
    auto parse = [](const std::string&) {};
    auto readSensor = []() -> double { throw std::runtime_error{"sensor offline"}; };

    Task::run(pool.get(), [&download] { return download(); }).then(pool.get(), [&parse](const std::string& body) {
        parse(body);
    }).onError([&logged](const std::exception& ex) {
        logged = std::string{ex.what()} == "offline";
    }).submit();

    auto temperature = Task::run(pool.get(), [&readSensor] { return readSensor(); }).recover([](const std::exception&) {
        return 20.0;
    }).then(loop.get(), [](double celsius) {
        return celsius;
    }).submit();

    REQUIRE(waitFor([&] {
        loop->runPending();
        return logged && temperature.isReady();
    }));
    REQUIRE(temperature.get() == 20.0);
}

namespace {
    struct TimeoutError: std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

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

TEST_CASE("README: typed handlers, and a handler on another executor", "[readme]")
{
    auto loop = ManualExecutor::create();
    auto pool = ThreadPoolExecutor::create(1);
    Device device;
    bool timedOut = false;
    std::atomic<bool> handled{false};

    Task::run(loop.get(), &Device::connect, &device)
        .onError([&timedOut](const TimeoutError&) { timedOut = true; })
        .onError(pool.get(), [&device, &handled](const std::exception& ex) {
        device.markOffline(ex.what());
        handled = true;
    }).submit();

    loop->runPending();
    REQUIRE(waitFor([&handled] { return handled.load(); }));
    REQUIRE_FALSE(timedOut); // a runtime_error, not a TimeoutError
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

TEST_CASE("README: delayed and periodic tasks", "[readme]")
{
    auto loop = ManualExecutor::create();
    std::stop_source stop;
    std::atomic<int> once{0};
    std::atomic<int> repeated{0};

    Task::run(loop.get(), stop.get_token(), 5ms, [&once] { ++once; }).submit();

    auto ticker = Task::run(loop.get(), 0ms, 5ms, [&repeated] { ++repeated; }).onError([](const std::exception&) {}).submit();

    REQUIRE(waitFor([&] {
        loop->runPending();
        return once == 1 && repeated >= 2;
    }));
    ticker.cancel();
    stop.request_stop();
    REQUIRE(ticker.waitFor(5s));
}

TEST_CASE("README: a periodic chain with a fixed delay", "[readme]")
{
    enum class Status
    {
        Ok
    };

    auto pool = ThreadPoolExecutor::create(2);
    auto loop = ManualExecutor::create();
    std::atomic<int> shown{0};

    auto series =
        Task::run(pool.get(), 0ms, FixedDelay{2ms}, [] { return Status::Ok; }).then(loop.get(), [&shown](Status) { ++shown; }).submit();

    REQUIRE(waitFor([&] {
        loop->runPending();
        return shown >= 2;
    }));
    series.cancel();
}

TEST_CASE("README: a periodic chain with a fixed rate that skips", "[readme]")
{
    enum class Status
    {
        Ok
    };

    auto pool = ThreadPoolExecutor::create(2);
    auto loop = ManualExecutor::create();
    std::atomic<int> shown{0};

    auto series =
        Task::run(pool.get(), 0ms, FixedRate{2ms, Overrun::Skip}, [] { return Status::Ok; }).then(loop.get(), [&shown](Status) {
        ++shown;
    }).submit();

    REQUIRE(waitFor([&] {
        loop->runPending();
        return shown >= 2;
    }));
    series.cancel();
    REQUIRE(series.waitFor(5s));
}

TEST_CASE("README: running chains together", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(2);
    auto loop = ManualExecutor::create();
    auto readTemperature = [] { return 21.5; };
    auto readHumidity = [] { return 40.0; };
    auto format = [](const double temperature, const double humidity) {
        return std::to_string(temperature) + "/" + std::to_string(humidity);
    };

    auto report = whenAll(Task::run(pool.get(), [&] { return readTemperature(); }), Task::run(pool.get(), [&] { return readHumidity(); }))
                      .then(loop.get(), [&format](double temperature, double humidity) {
        return format(temperature, humidity);
    }).submit();

    auto fastest = whenAny(Task::run(pool.get(), [] { return std::string{"primary"}; }), Task::run(pool.get(), [] { return 2; })).submit();

    REQUIRE(waitFor([&] {
        loop->runPending();
        return report.isReady();
    }));
    REQUIRE(report.get() == format(21.5, 40.0));

    const auto winner = fastest.get();
    REQUIRE((winner.index() == 0 || winner.index() == 1));
}

namespace {
    struct Sensor
    {
        [[nodiscard]] double read() const
        {
            return value;
        }

        double value = 0;
    };
} // namespace

TEST_CASE("README: joining a vector of chains", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(2);
    const std::vector<Sensor> sensors{{19.5}, {21.0}};

    std::vector<TaskBuilder<double>> readings;
    for (const auto& sensor : sensors) {
        readings.push_back(Task::run(pool.get(), [&sensor] { return sensor.read(); }));
    }

    auto all = whenAll(std::move(readings)).submit();

    REQUIRE(all.get() == std::vector<double>{19.5, 21.0});
}

namespace {
    class FlushTask final: public Task
    {
      public:
        explicit FlushTask(std::atomic<int>& flushed): _flushed{flushed}
        {
        }

        void run() override
        {
            ++_flushed;
        }

        void cancel() noexcept override
        {
        }

      private:
        std::atomic<int>& _flushed;
    };
} // namespace

TEST_CASE("README: a Task object", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(1);
    std::atomic<int> flushed{0};

    pool->submit(std::make_unique<FlushTask>(flushed));

    REQUIRE(waitFor([&flushed] { return flushed == 1; }));
}

TEST_CASE("README: cancellations reach handlers and recoveries", "[readme]")
{
    auto loop = ManualExecutor::create();

    SECTION("a std::exception handler receives them")
    {
        bool received = false;
        auto handle = Task::run(loop.get(), [] {}).onError([&received](const std::exception&) { received = true; }).submit();
        handle.cancel();
        loop->runPending();
        REQUIRE(received);
    }

    SECTION("a handler of a more specific type doesn't")
    {
        bool received = false;
        auto handle = Task::run(loop.get(), [] {}).onError([&received](const std::logic_error&) { received = true; }).submit();
        handle.cancel();
        loop->runPending();
        REQUIRE_FALSE(received);
    }

    SECTION("a recovery as the last step gives the handle its value")
    {
        auto handle = Task::run(loop.get(), [] { return 1; }).recover([](const std::exception&) { return 2; }).submit();
        handle.cancel();
        loop->runPending();
        REQUIRE(handle.get() == 2);
    }

    SECTION("the tasks after a recovery still don't run")
    {
        bool ran = false;
        auto handle = Task::run(loop.get(), [] { return 1; }).recover([](const std::exception&) {
            return 2;
        }).then(loop.get(), [&ran](int) {
            ran = true;
        }).submit();
        handle.cancel();
        loop->runPending();
        REQUIRE_FALSE(ran);
        REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    }
}

TEST_CASE("README: a handler on another executor can run after the handle is ready", "[readme]")
{
    auto loop = ManualExecutor::create();
    auto other = ManualExecutor::create();
    bool handled = false;

    auto handle = Task::run(loop.get(), [] { throw std::runtime_error{"failed"}; })
                      .onError(other.get(), [&handled](const std::exception&) {
        handled = true;
    }).submit();

    loop->runPending();
    REQUIRE(handle.isReady());
    REQUIRE_FALSE(handled);

    other->runPending();
    REQUIRE(handled);
}

TEST_CASE("README: a fixed-rate series piles up on an executor that doesn't keep up", "[readme]")
{
    auto loop = ManualExecutor::create();
    std::atomic<int> runs{0};

    auto ticker = Task::run(loop.get(), 0ms, 1ms, [&runs] { ++runs; }).submit();

    REQUIRE(waitFor([&loop] { return loop->pendingCount() >= 3; }));
    REQUIRE(runs == 0);

    ticker.cancel();
    REQUIRE(ticker.waitFor(5s));
}

TEST_CASE("README: what a mutable lambda changes is gone by the next periodic run", "[readme]")
{
    auto loop = ManualExecutor::create();
    std::atomic<int> runs{0};
    std::atomic<int> highest{0};

    auto ticker = Task::run(loop.get(), 0ms, 1ms, [&runs, &highest, count = 0]() mutable {
        highest = std::max(highest.load(), ++count);
        ++runs;
    }).submit();

    REQUIRE(waitFor([&] {
        loop->runPending();
        return runs >= 3;
    }));
    ticker.cancel();
    REQUIRE(ticker.waitFor(5s));
    REQUIRE(highest == 1);
}

TEST_CASE("README: a join doesn't wait for the chains it stops", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(2);
    std::promise<void> release;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};

    auto join = whenAll(
                    Task::run(
                        pool.get(),
                        [&started, &finished, wait = release.get_future().share()] {
        started = true;
        wait.wait();
        finished = true;
    }),
                    Task::run(pool.get(), [&started] {
        waitFor([&started] { return started.load(); }); // or the join's stop would cancel the other task before it starts
        throw std::runtime_error{"failed"};
    })).submit();

    REQUIRE_THROWS_AS(join.get(), std::runtime_error);
    REQUIRE_FALSE(finished);

    release.set_value();
    REQUIRE(waitFor([&finished] { return finished.load(); }));
}

TEST_CASE("README: ShutdownGate", "[readme]")
{
    ShutdownGate gate;
    int used = 0;

    if (const auto entry = gate.enter()) {
        ++used;
    }

    gate.close();

    if (const auto entry = gate.enter()) {
        ++used;
    }
    REQUIRE(used == 1);
    REQUIRE(gate.isClosed());
}

TEST_CASE("README: the debug macros", "[readme]")
{
    [[maybe_unused]] const int count = 0;
    int closed = 0;
    auto closeFile = [&closed] { return ++closed == 1 ? 0 : -1; };

    VANILO_TRACE("%d tasks left", count);
    VANILO_ASSERT(count >= 0);
    VANILO_VERIFY(closeFile() == 0);

    REQUIRE(closed == 1); // in release builds too
}
