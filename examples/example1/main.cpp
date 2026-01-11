#include <vanilo/core/Tracer.h>
#include <vanilo/tasker/Tasker.h>

#include <thread>

using namespace vanilo::tasker;
using namespace vanilo::concurrent;

int main()
{
    auto counter = 0;
    const auto executor = LocalThreadExecutor::create();
    auto start = std::chrono::steady_clock::now();
    auto source = CancellationTokenSource{};

    Task::run(executor.get(), source.token(), std::chrono::milliseconds(500), std::chrono::milliseconds(500), [&start, &counter, &source] {
        std::cout << "Hello World! " << std::endl;

        const auto end = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "Elapsed: " << elapsedMs << " ms\n";

        // reset start
        start = std::chrono::steady_clock::now();

        if (++counter == 5) {
            source.cancel();
        }
    });

    while (true) {
        executor->process(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        if (source.isCancellationRequested()) {
            auto finish = std::chrono::steady_clock::now();

            if (const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count(); elapsedMs > 2000) {
                break;
            }
        }
    }

    return 0;
}