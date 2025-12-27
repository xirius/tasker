#include <vanilo/core/Tracer.h>
#include <vanilo/tasker/Tasker.h>

#include <thread>

using namespace vanilo::tasker;
using namespace vanilo::concurrent;

int main()
{
    auto finished = false;

    const auto executor = LocalThreadExecutor::create();

    Task::run(executor.get(), CancellationToken{}, std::chrono::seconds(1), [&finished] {
        std::cout << "Hello World! " << std::endl;
        finished = true;
    });

    /*
    while (!finished) {
        executor->process(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    */

    return 0;
}