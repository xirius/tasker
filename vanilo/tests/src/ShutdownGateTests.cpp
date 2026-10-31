#include "TestUtilities.h"

#include <vanilo/concurrent/ShutdownGate.h>
#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::runsWithin;

TEST_CASE("Race condition: executor destroyed before a delayed task is queued", "[shutdown]")
{
    // The delay is handled by the timer queue; when it passes, the task is handed to its executor.
    auto executor = ThreadPoolExecutor::create(1);
    std::atomic_bool executed{false};

    Task::run(executor.get(), 100ms, [&executed] { executed = true; }).submit();

    // Destroy the executor immediately
    executor.reset();

    // Wait for the delay to pass + some buffer
    std::this_thread::sleep_for(200ms);

    // The task must not have run (its executor is gone), and nothing crashed.
    REQUIRE_FALSE(executed);
}

TEST_CASE("ShutdownGate::Entry can be moved", "[ShutdownGate]")
{
    ShutdownGate gate;

    SECTION("move construction transfers the entry")
    {
        auto first = gate.enter();
        auto second = std::move(first);
        REQUIRE_FALSE(first); // NOLINT(bugprone-use-after-move): checking the moved-from state is the point
        REQUIRE(second);
    }

    SECTION("move assignment leaves the old gate and takes the new entry")
    {
        ShutdownGate other;
        auto entry = gate.enter();
        auto otherEntry = other.enter();

        entry = std::move(otherEntry);
        REQUIRE(entry);
        REQUIRE_FALSE(otherEntry); // NOLINT(bugprone-use-after-move)

        // gate's entry has left, so close() returns at once; other's entry is still inside.
        gate.close();
        entry = ShutdownGate::Entry{};
        other.close();
    }

    SECTION("self move assignment keeps the entry")
    {
        auto entry = gate.enter();
        auto& alias = entry;
        entry = std::move(alias);
        REQUIRE(entry);
    }

    SECTION("a closed gate can't be entered")
    {
        gate.close();
        REQUIRE(gate.isClosed());
        REQUIRE_FALSE(gate.enter());
        REQUIRE_FALSE(gate.tryEnter());
    }
}

TEST_CASE("ShutdownGate::close() waits for the entries inside", "[ShutdownGate]")
{
    ShutdownGate gate;
    auto entry = gate.enter();
    std::atomic<bool> closed{false};

    std::thread closer{[&gate, &closed] {
        gate.close();
        closed = true;
    }};

    std::this_thread::sleep_for(50ms);
    REQUIRE_FALSE(closed);
    REQUIRE_FALSE(gate.enter()); // closing: no new entries

    entry = ShutdownGate::Entry{};
    REQUIRE(runsWithin([&closer] { closer.join(); }, 5s));
    REQUIRE(closed);
}

TEST_CASE("ShutdownGate never lets an entry overlap the end of close()", "[regression][ShutdownGate][stress]")
{
    // close() and tryEnter() each write one variable and then read the other. With only release/acquire
    // ordering both could read the old value (store buffering), letting a caller enter after close() had
    // already returned.
    constexpr int Rounds = 200;
    constexpr int Threads = 4;

    for (int round = 0; round < Rounds; ++round) {
        ShutdownGate gate;
        std::atomic<bool> start{false};
        std::atomic<bool> closed{false};
        std::atomic<int> violations{0};
        std::vector<std::thread> threads;

        for (int i = 0; i < Threads; ++i) {
            threads.emplace_back([&] {
                while (!start.load()) {
                    std::this_thread::yield();
                }
                for (int attempt = 0; attempt < 1000; ++attempt) {
                    if (const auto entry = gate.enter()) {
                        if (closed.load()) {
                            violations.fetch_add(1);
                        }
                    }
                }
            });
        }

        start = true;
        gate.close();
        closed = true;

        for (auto& thread : threads) {
            thread.join();
        }
        REQUIRE(violations == 0);
    }
}
