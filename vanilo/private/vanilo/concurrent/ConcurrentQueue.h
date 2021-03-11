#ifndef INC_D8C0265AC7204A5F8B06B620B9F01B70
#define INC_D8C0265AC7204A5F8B06B620B9F01B70

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace vanilo::concurrent {

    template <typename T>
    class ConcurrentQueue
    {
      public:
        ConcurrentQueue() = default;

        ConcurrentQueue(const ConcurrentQueue<T>& other) = delete;

        ConcurrentQueue(ConcurrentQueue<T>&& other) noexcept = delete;

        ~ConcurrentQueue()
        {
            if (_valid) {
                invalidate();
            }
        }

        /**
         * Adds a new element to the end of the queue. The element is constructed in-place.
         * @tparam Args Type of the arguments.
         * @param args  Arguments to forward to the constructor of the element
         * @return The value or reference (if any).
         */
        template <typename... Args>
        auto enqueue(Args&&... args)
        {
            std::lock_guard<std::mutex> lock{_mutex};
            auto result = _queue.emplace(std::forward<Args>(args)...);
            _condition.notify_one();
            return result;
        }

        /**
         * Adds the given element value to the end of the queue.
         * @param value The value of the element to push.
         */
        void enqueue(const T& value)
        {
            std::lock_guard<std::mutex> lock{_mutex};
            _queue.push(value);
            _condition.notify_one();
        }

        /**
         * Adds the given element value to the end of the queue.
         * @param value The value of the element to push.
         */
        void enqueue(T&& value)
        {
            std::lock_guard<std::mutex> lock{_mutex};
            _queue.push(std::move(value));
            _condition.notify_one();
        }

        /**
         * Attempts to retrieve the first element from the queue (non blocking). The element is removed from the queue.
         * @param out The retrieved element from the queue.
         * @return True if an element was successfully written to the out parameter, false otherwise.
         */
        bool tryDequeue(T& out)
        {
            std::lock_guard<std::mutex> lock{_mutex};

            if (_queue.empty() || !_valid) {
                return false;
            }

            out = std::move(_queue.front());
            _queue.pop();

            return true;
        }

        /**
         * Retrieves the first element from the queue (blocking). The element is removed from the queue.
         * This method blocks until an element is available or unless clear is called or the instance is destructed.
         * @param out The retrieved element from the queue.
         * @return True if an element was successfully written to the out parameter, false otherwise.
         */
        bool waitDequeue(T& out)
        {
            std::unique_lock<std::mutex> lock{_mutex};

            // Using the condition in the predicate ensures that spurious wake ups with a valid
            // but empty queue will not proceed, so only need to check for validity before proceeding.
            _condition.wait(lock, [this]() { return !_queue.empty() || !_valid; });

            if (!_valid) {
                return false;
            }

            out = std::move(_queue.front());
            _queue.pop();

            return true;
        }

        /**
         * Checks if the queue has no elements.
         * @return True if the queue has no elements, false otherwise.
         */
        bool empty() const
        {
            std::lock_guard<std::mutex> lock{_mutex};
            return _queue.empty();
        }

        /**
         * Removes all the elements from the queue.
         */
        void clear()
        {
            std::lock_guard<std::mutex> lock{_mutex};

            while (!_queue.empty()) {
                _queue.pop();
            }

            _condition.notify_all();
        }

        /**
         * Returns the number of elements in the queue.
         * @return The number of elements in the queue.
         */
        size_t size() const
        {
            std::lock_guard<std::mutex> lock{_mutex};
            return _queue.size();
        }

        /**
         * Invalidates the queue. Used to ensure no conditions are being waited on in the waitDequeue method when
         * a thread or the application is trying to exit. It is an undefined behaviour to continue use the queue
         * after this method has been called.
         */
        void invalidate()
        {
            std::lock_guard<std::mutex> lock{_mutex};
            _valid = false;
            _condition.notify_all();
        }

        /**
         * Returns whether or not the queue is valid.
         * @return True if the queue is valid, false otherwise.
         */
        bool isValid() const
        {
            return _valid.load();
        }

      private:
        std::atomic_bool _valid{true};
        std::condition_variable _condition;
        mutable std::mutex _mutex;
        std::queue<T> _queue;
    };

} // namespace vanilo::concurrent

#endif // INC_D8C0265AC7204A5F8B06B620B9F01B70
