#include <vanilo/core/Tracer.h>
#include <vanilo/tasker/Tasker.h>

#include <thread>

using namespace vanilo::tasker;
using namespace vanilo::concurrent;

int main()
{
    auto finished = false;
    const auto executor = LocalThreadExecutor::create();
    auto start = std::chrono::steady_clock::now();

    Task::run(executor.get(), CancellationToken{}, std::chrono::seconds(1), [&finished, &start] {
        std::cout << "Hello World! " << std::endl;
        finished = true;

        const auto end = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "Elapsed: " << elapsedMs << " ms\n";
    });

    while (!finished) {
        executor->process(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}