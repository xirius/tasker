#ifndef INC_2270EF516D0140F1801160B8CE45015E
#define INC_2270EF516D0140F1801160B8CE45015E

#include <vanilo/tasker/Tasker.h>

#include <queue>

namespace vanilo::tasker {

    /// DefaultLocalThreadExecutor
    /// ============================================================================================

    class DefaultLocalThreadExecutor: public LocalThreadExecutor
    {
      public:
        [[nodiscard]] size_t count() const override;
        size_t process(size_t maxCount) override;
        void submit(std::unique_ptr<Task> task) override;

      private:
        std::unique_ptr<Task> nextTask(size_t& queueSize);

        std::queue<std::unique_ptr<Task>> _queue;
        std::atomic<size_t> _queueSize{0};
        mutable std::mutex _mutex;
    };

} // namespace vanilo::tasker

#endif // INC_2270EF516D0140F1801160B8CE45015E