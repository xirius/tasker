#ifndef INC_D8C0265AC7204A5F8B06B620B9F01B70
#define INC_D8C0265AC7204A5F8B06B620B9F01B70

#include <vanilo/concurrent/CancellationToken.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace vanilo::concurrent {

    template <typename T>
    class ConcurrentQueue
    {
      public:
        ConcurrentQueue() = default;

        ConcurrentQueue(const ConcurrentQueue& other) = delete;

        ConcurrentQueue(ConcurrentQueue&& other) noexcept = delete;

        ~ConcurrentQueue()
        {
            close();
            while (_waiters.load(std::memory_order_acquire) > 0) {
                std::this_thread::yield();
            }
        }

        /**
         * Checks if an element matching the predicate exists in the queue.
         * @param predicate The predicate to check for existence.
         * @return True if a matching element was found; false otherwise.
         */
        bool contains(const std::function<bool(const T&)>& predicate) const
        {
            std::lock_guard lock{_mutex};
            return std::any_of(_queue.begin(), _queue.end(), predicate);
        }

        /**
         * Adds a new element to the end of the queue. The element is constructed in-place.
         * @tparam Args Type of the arguments.
         * @param args  Arguments to forward to the constructor of the element
         * @return True if the element was added; false if the queue is invalid.
         */
        template <typename... Args>
        bool enqueue(Args&&... args)
        {
            std::lock_guard lock{_mutex};

            if (_closed) {
                return false;
            }

            _queue.emplace_back(std::forward<Args>(args)...);
            _condition.notify_one();
            return true;
        }

        /**
         * Adds the given element value to the end of the queue.
         * @param value The value of the element to push.
         * @return True if the element was added; false if the queue is invalid.
         */
        bool enqueue(const T& value)
        {
            std::lock_guard lock{_mutex};

            if (_closed) {
                return false;
            }

            _queue.push_back(value);
            _condition.notify_one();
            return true;
        }

        /**
         * Adds the given element value to the end of the queue.
         * @param value The value of the element to push.
         * @return True if the element was added; false if the queue is invalid.
         */
        bool enqueue(T&& value)
        {
            std::lock_guard lock{_mutex};

            if (_closed) {
                return false;
            }

            _queue.push_back(std::move(value));
            _condition.notify_one();
            return true;
        }

        /**
         * Attempts to retrieve the first element from the queue (non-blocking). The element is removed from the queue.
         * @param out The retrieved element from the queue.
         * @return True if an element was successfully written to the out parameter, false otherwise.
         */
        bool tryDequeue(T& out)
        {
            std::lock_guard lock{_mutex};

            if (_queue.empty() || _closed) {
                return false;
            }

            out = std::move(_queue.front());
            _queue.pop_front();

            return true;
        }

        /**
         * Retrieves the first element from the queue (blocking). The element is removed from the queue.
         * This method blocks until an element is available or the queue is closed.
         * @param out The retrieved element from the queue.
         * @return True if an element was successfully written to the out parameter; false if the queue is closed.
         */
        bool waitDequeue(T& out)
        {
            WaiterGuard waiter{_waiters};
            std::unique_lock lock{_mutex};

            // Using the condition in the predicate ensures that spurious wake-ups with a valid
            // but empty queue will not proceed, so only need to check for validity before proceeding.
            _condition.wait(lock, [this] { return !_queue.empty() || _closed; });

            if (_closed) {
                return false;
            }

            out = std::move(_queue.front());
            _queue.pop_front();

            return true;
        }

        /**
         * Retrieves the first element from the queue (blocking). The element is removed from the queue.
         * This method blocks until an element is available, the queue is closed, or the cancellation token is
         * in the canceled state.
         *
         * If the cancellation token is in the canceled state but the queue is not empty, an element
         * will be retrieved and the method will return true.
         *
         * If the queue is closed, it is always empty (as close() drains the queue) and the method will return false.
         *
         * @param token The cancellation token.
         * @param out The retrieved element from the queue.
         * @return True if an element was successfully written to the out parameter; false if the queue is closed
         *         or cancellation was requested and the queue is empty.
         */
        bool waitDequeue(const CancellationToken& token, T& out)
        {
            WaiterGuard waiter{_waiters};
            // token.subscribe must be called outside the std::unique_lock lock{_mutex}.
            // If token.subscribe is ever moved inside the lock to "protect" it, a deadlock will occur
            // because the callback would attempt to acquire the same mutex recursively.
            bool canceled = false;
            auto subscription = token.subscribe([this, &canceled] {
                std::unique_lock lock{_mutex};
                canceled = true;
                _condition.notify_all();
            });

            std::unique_lock lock{_mutex};
            // Using the condition in the predicate ensures that spurious wake-ups with a valid
            // but empty queue will not proceed, so only need to check for validity before proceeding.
            _condition.wait(lock, [this, &canceled] { return !_queue.empty() || canceled || _closed; });

            if (_queue.empty() && (canceled || _closed)) {
                return false;
            }

            out = std::move(_queue.front());
            _queue.pop_front();

            return true;
        }

        /**
         * Checks if the queue is empty.
         * @return True if the queue is empty, false otherwise.
         */
        bool empty() const
        {
            std::lock_guard lock{_mutex};
            return _queue.empty();
        }

        /**
         * Removes all elements from the queue.
         */
        void clear()
        {
            std::lock_guard lock{_mutex};
            _queue.clear();
            _condition.notify_all();
        }

        /**
         * Returns the number of elements in the queue.
         * @return The number of elements in the queue.
         */
        size_t size() const
        {
            std::lock_guard lock{_mutex};
            return _queue.size();
        }

        /**
         * Closes the queue. Used to ensure no conditions are being waited on in the waitDequeue method when
         * a thread or the application is trying to exit. It is recommended to not use the queue after this method
         * has been called, as further operations will return false or fail.
         * @return The list of remaining elements in the queue.
         */
        std::vector<T> close()
        {
            std::lock_guard lock{_mutex};
            std::vector<T> remaining;

            if (_closed) {
                return remaining;
            }

            remaining.reserve(_queue.size());
            std::move(_queue.begin(), _queue.end(), std::back_inserter(remaining));
            _queue.clear();

            _closed = true;
            _condition.notify_all();
            return remaining;
        }

        /**
         * Returns whether the queue is closed.
         * @return True if the queue is closed, false otherwise.
         */
        bool isClosed() const
        {
            std::lock_guard lock{_mutex};
            return _closed;
        }

        /**
         * Returns a list of the transformed items from the queue.
         * @tparam TOut The type of the resulting list elements.
         * @param selector The transformation function to apply to each element.
         * @return A vector containing the transformed items.
         */
        template <typename TOut>
        std::vector<TOut> toList(const std::function<TOut(const T&)>& selector) const
        {
            std::lock_guard lock{_mutex};
            std::vector<TOut> list;
            std::for_each(_queue.begin(), _queue.end(), [&list, &selector](auto& item) { list.emplace_back(selector(item)); });
            return list;
        }

      private:
        struct WaiterGuard
        {
            std::atomic<int>& _counter;
            explicit WaiterGuard(std::atomic<int>& counter): _counter(counter)
            {
                _counter.fetch_add(1, std::memory_order_acq_rel);
            }

            ~WaiterGuard()
            {
                _counter.fetch_sub(1, std::memory_order_acq_rel);
            }
        };

        bool _closed{false};
        std::atomic<int> _waiters{0};
        std::condition_variable _condition;
        mutable std::mutex _mutex;
        std::deque<T> _queue;
    };

} // namespace vanilo::concurrent

#endif // INC_D8C0265AC7204A5F8B06B620B9F01B70