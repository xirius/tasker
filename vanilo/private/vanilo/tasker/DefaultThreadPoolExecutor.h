#ifndef INC_0932568A7AA2436A9334D1B006A053A4
#define INC_0932568A7AA2436A9334D1B006A053A4

#include <vanilo/concurrent/ConcurrentQueue.h>
#include <vanilo/tasker/Tasker.h>

namespace vanilo::tasker {

    /// DefaultThreadPoolExecutor
    /// ============================================================================================

    class DefaultThreadPoolExecutor: public ThreadPoolExecutor
    {
      public:
        explicit DefaultThreadPoolExecutor(size_t numThreads);
        ~DefaultThreadPoolExecutor() override;

        [[nodiscard]] size_t count() const override;
        [[nodiscard]] size_t threadCount() const noexcept override;
        std::future<void> resize(size_t numThreads) override;
        void submit(std::unique_ptr<Task> task) override;

      private:
        void init(size_t numThreads);
        void invalidate();
        void worker();

        concurrent::ConcurrentQueue<std::unique_ptr<Task>> _queue;
        concurrent::ConcurrentQueue<std::thread> _threads;
        std::mutex _mutex;
    };

} // namespace vanilo::tasker

#endif // INC_0932568A7AA2436A9334D1B006A053A4
