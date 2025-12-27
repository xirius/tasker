#ifndef INC_2270EF516D0140F1801160B8CE45015E
#define INC_2270EF516D0140F1801160B8CE45015E

#include <vanilo/concurrent/ConcurrentQueue.h>
#include <vanilo/tasker/Tasker.h>

namespace vanilo::tasker {

    /// DefaultLocalThreadExecutor
    /// ============================================================================================

    class DefaultLocalThreadExecutor final: public LocalThreadExecutor
    {
      public:
        [[nodiscard]] size_t count() const override;
        size_t process(size_t maxCount) override;
        size_t process(const CancellationToken& token) override;
        void submit(std::unique_ptr<Task> task) override;

      private:
        concurrent::ConcurrentQueue<std::unique_ptr<Task>> _queue;
        mutable std::mutex _mutex;
    };

} // namespace vanilo::tasker

#endif // INC_2270EF516D0140F1801160B8CE45015E