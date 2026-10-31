#include <vanilo/concurrent/ConcurrentQueue.h>
#include <vanilo/tasker/ExecutorCore.h>
#include <vanilo/tasker/Task.h>
#include <vanilo/tasker/TaskExecutor.h>

#include <algorithm>
#include <array>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace vanilo::tasker::detail;

namespace {

    /**
     * A job running a Task object.
     */
    struct TaskObjectJob
    {
        std::unique_ptr<Task> task;

        void run()
        {
            task->run();
        }

        void cancel() noexcept
        {
            task->cancel();
        }
    };

    constexpr std::size_t BatchSize = 32;

    /// ManualExecutor core
    /// ============================================================================================

    class ManualCore final: public ExecutorCore
    {
      public:
        bool tryEnqueue(Job& job) override
        {
            return _queue.enqueue(std::move(job));
        }

        [[nodiscard]] std::size_t pendingCount() const noexcept override
        {
            return _queue.size();
        }

        std::size_t runPending(const std::size_t maxCount)
        {
            std::array<Job, BatchSize> batch;
            std::size_t ran = 0;

            while (ran < maxCount) {
                const auto count = _queue.tryDequeueBulk(batch.begin(), std::min(BatchSize, maxCount - ran));
                if (count == 0) {
                    break;
                }
                for (std::size_t i = 0; i < count; ++i) {
                    runJob(batch[i]);
                }
                ran += count;
            }

            return _queue.size();
        }

        std::size_t runUntil(const std::stop_token& token)
        {
            std::array<Job, BatchSize> batch;

            while (const auto count = _queue.waitDequeueBulk(token, batch.begin(), BatchSize)) {
                for (std::size_t i = 0; i < count; ++i) {
                    runJob(batch[i]);
                }
            }

            return _queue.size();
        }

        void shutdown() noexcept
        {
            // The pending jobs will never run: cancel them, so their chains know.
            for (auto& job : _queue.close()) {
                job.cancel();
            }
        }

      private:
        ConcurrentQueue<Job> _queue;
    };

    ManualCore& manualCore(const TaskExecutor& executor) noexcept
    {
        return static_cast<ManualCore&>(*ExecutorAccess::core(executor));
    }

} // namespace

/// TaskExecutor
/// ================================================================================================

TaskExecutor::TaskExecutor(std::shared_ptr<ExecutorCore> core) noexcept: _core{std::move(core)}
{
}

TaskExecutor::~TaskExecutor() = default;

std::size_t TaskExecutor::pendingCount() const noexcept
{
    return _core->pendingCount();
}

void TaskExecutor::submit(std::unique_ptr<Task> task)
{
    if (!task) {
        return;
    }

    auto job = Job::make<TaskObjectJob>(std::move(task));
    if (!_core->tryEnqueue(job)) {
        job.cancel();
    }
}

/// ManualExecutor
/// ================================================================================================

struct ManualExecutor::Private
{
};

std::unique_ptr<ManualExecutor> ManualExecutor::create()
{
    return std::make_unique<ManualExecutor>(Private{});
}

ManualExecutor::ManualExecutor(Private): TaskExecutor{std::make_shared<ManualCore>()}
{
}

ManualExecutor::~ManualExecutor()
{
    manualCore(*this).shutdown();
}

std::size_t ManualExecutor::runPending(const std::size_t maxCount)
{
    return manualCore(*this).runPending(maxCount);
}

std::size_t ManualExecutor::runUntil(const std::stop_token token)
{
    return manualCore(*this).runUntil(token);
}
