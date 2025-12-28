#include <vanilo/core/Tracer.h>
#include <vanilo/tasker/Tasker.h>

#include <thread>

using namespace vanilo::tasker;
using namespace vanilo::concurrent;

int main()
{
    auto finished = false;
    auto counter = 0;
    const auto executor = LocalThreadExecutor::create();
    auto start = std::chrono::steady_clock::now();
    auto source = CancellationTokenSource{};

    Task::run(executor.get(), source.token(), std::chrono::seconds(3), std::chrono::seconds(1), [&finished, &start, &counter, &source] {
        std::cout << "Hello World! " << std::endl;

        const auto end = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "Elapsed: " << elapsedMs << " ms\n";

        // reset start
        start = std::chrono::steady_clock::now();

        if (counter++ == 10) {
            source.cancel();
        }
        // finished = true;
    });

    while (!finished) {
        executor->process(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}