#ifndef INC_D8C0265AC7204A5F8B06B620B9F01B70
#define INC_D8C0265AC7204A5F8B06B620B9F01B70

#include <vanilo/concurrent/CancellationToken.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>

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
         * Checks for the existence of the value in the queue.
         * @param predicate The predicate to check for the existence.
         * @return True if the element was found; false otherwise.
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
         * @return The value or reference (if any).
         */
        template <typename... Args>
        auto enqueue(Args&&... args)
        {
            std::lock_guard lock{_mutex};
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
            std::lock_guard lock{_mutex};
            _queue.push(value);
            _condition.notify_one();
        }

        /**
         * Adds the given element value to the end of the queue.
         * @param value The value of the element to push.
         */
        void enqueue(T&& value)
        {
            std::lock_guard lock{_mutex};
            _queue.push_back(std::move(value));
            _condition.notify_one();
        }

        /**
         * Attempts to retrieve the first element from the queue (non-blocking). The element is removed from the queue.
         * @param out The retrieved element from the queue.
         * @return True if an element was successfully written to the out parameter, false otherwise.
         */
        bool tryDequeue(T& out)
        {
            std::lock_guard lock{_mutex};

            if (_queue.empty() || !_valid) {
                return false;
            }

            out = std::move(_queue.front());
            _queue.pop_front();

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
            std::unique_lock lock{_mutex};

            // Using the condition in the predicate ensures that spurious wake-ups with a valid
            // but empty queue will not proceed, so only need to check for validity before proceeding.
            _condition.wait(
                lock, [this]() {
                    return !_queue.empty() || !_valid;
                });

            if (!_valid) {
                return false;
            }

            out = std::move(_queue.front());
            _queue.pop_front();

            return true;
        }

        /**
         * Retrieves the first element from the queue (blocking). The element is removed from the queue.
         * This method blocks until an element is available or unless clear is called, cancellation token
         * is in canceled state or the instance is destructed.
         * @param token The cancellation token.
         * @param out The retrieved element from the queue.
         * @return True if an element was successfully written to the out parameter, false otherwise.
         */
        bool waitDequeue(CancellationToken& token, T& out)
        {
            bool canceled = false;
            auto subscription = token.subscribe(
                [this, &canceled]() {
                    std::unique_lock lock{_mutex};
                    canceled = true;
                    _condition.notify_all();
                });

            std::unique_lock lock{_mutex};
            // Using the condition in the predicate ensures that spurious wake-ups with a valid
            // but empty queue will not proceed, so only need to check for validity before proceeding.
            _condition.wait(
                lock, [this, &token, &canceled]() {
                    return !_queue.empty() || canceled || !_valid;
                });

            if (token.isCancellationRequested() || !_valid) {
                return false;
            }

            out = std::move(_queue.front());
            _queue.pop_front();

            return true;
        }

        /**
         * Checks if the queue has no elements.
         * @return True if the queue has no elements, false otherwise.
         */
        bool empty() const
        {
            std::lock_guard lock{_mutex};
            return _queue.empty();
        }

        /**
         * Removes all the elements from the queue.
         */
        void clear()
        {
            std::lock_guard lock{_mutex};

            while (!_queue.empty()) {
                _queue.pop_front();
            }

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
         * Invalidates the queue. Used to ensure no conditions are being waited on in the waitDequeue method when
         * a thread or the application is trying to exit. It is an undefined behaviour to continue use the queue
         * after this method has been called.
         * @return The list of remaining elements in the queue.
         */
        std::vector<T> invalidate()
        {
            std::lock_guard lock{_mutex};
            std::vector<T> remaining;

            while (!_queue.empty()) {
                remaining.push_back(std::move(_queue.front()));
                _queue.pop_front();
            }

            _valid = false;
            _condition.notify_all();
            return remaining;
        }

        /**
         * Returns whether the queue is valid.
         * @return True if the queue is valid, false otherwise.
         */
        bool isValid() const
        {
            return _valid.load();
        }

        /**
         * Returns the list of the transformed items of the queue.
         * @tparam TOut The type of the list elements
         * @param selector The transformation function on the queue elements.
         * @return The list of the transformed items of the queue.
         */
        template <typename TOut>
        std::vector<TOut> toList(const std::function<TOut(const T&)>& selector) const
        {
            std::lock_guard lock{_mutex};
            std::vector<TOut> list;
            std::for_each(
                _queue.begin(), _queue.end(), [&list, &selector](auto& item) {
                    list.emplace_back(selector(item));
                });
            return list;
        }

    private:
        std::atomic_bool _valid{true};
        std::condition_variable _condition;
        mutable std::mutex _mutex;
        std::deque<T> _queue;
    };
} // namespace vanilo::concurrent

#endif // INC_D8C0265AC7204A5F8B06B620B9F01B70