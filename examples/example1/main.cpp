#include <vanilo/tasker/Tasker.h>

#include <chrono>
#include <iostream>

// A periodic task on an event loop: it runs every 500 ms on this thread, and stops the loop after five runs.

using namespace vanilo::tasker;
using namespace std::chrono_literals;

int main()
{
    const auto loop = ManualExecutor::create();
    std::stop_source stop;
    auto start = std::chrono::steady_clock::now();
    int counter = 0;

    Task::run(loop.get(), stop.get_token(), 500ms, 500ms, [&start, &counter, &stop] {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        std::cout << "Hello World! Elapsed: " << elapsed.count() << " ms\n";
        start = std::chrono::steady_clock::now();

        if (++counter == 5) {
            stop.request_stop(); // ends the series and the loop below
        }
    }).submit();

    loop->runUntil(stop.get_token());
    return 0;
}
