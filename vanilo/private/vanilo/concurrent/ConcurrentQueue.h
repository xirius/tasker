#ifndef INC_D8C0265AC7204A5F8B06B620B9F01B70
#define INC_D8C0265AC7204A5F8B06B620B9F01B70

#include <vanilo/concurrent/RingBuffer.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stop_token>
#include <vector>

namespace vanilo::concurrent {

    /**
     * A thread-safe FIFO queue that consumers can wait on. Waiting can be interrupted through a
     * std::stop_token, and close() wakes every waiter. Producers only notify when a consumer waits,
     * and they do it after releasing the lock.
     */
    template <typename T>
    class ConcurrentQueue
    {
      public:
        ConcurrentQueue() = default;

        ConcurrentQueue(const ConcurrentQueue& other) = delete;
        ConcurrentQueue& operator=(const ConcurrentQueue& other) = delete;

        /**
         * Closes the queue and waits until the threads blocked in it have left.
         */
        ~ConcurrentQueue()
        {
            close();
            std::unique_lock lock{_mutex};
            _condition.wait(lock, [this] { return _waiters == 0; });
        }

        /**
         * Checks if an element matching the predicate exists in the queue.
         * @param predicate The predicate to check for existence.
         * @return True if a matching element was found; false otherwise.
         */
        bool contains(const std::function<bool(const T&)>& predicate) const
        {
            std::lock_guard lock{_mutex};
            for (std::size_t i = 0; i < _queue.size(); ++i) {
                if (predicate(_queue[i])) {
                    return true;
                }
            }
            return false;
        }

        /**
         * Adds a new element to the end of the queue. The element is constructed in-place.
         * @param args Arguments to forward to the constructor of the element.
         * @return True if the element was added; false if the queue is closed.
         */
        template <typename... Args>
        bool enqueue(Args&&... args)
        {
            return push(false, std::forward<Args>(args)...);
        }

        /**
         * Adds the given element to the end of the queue.
         * @return True if the element was added; false if the queue is closed.
         */
        bool enqueue(const T& value)
        {
            return push(false, value);
        }

        /**
         * Adds the given element to the end of the queue.
         * @return True if the element was added; false if the queue is closed (the value is left untouched).
         */
        bool enqueue(T&& value)
        {
            return push(false, std::move(value));
        }

        /**
         * Adds the given element to the front of the queue.
         * @return True if the element was added; false if the queue is closed.
         */
        bool enqueueFront(const T& value)
        {
            return push(true, value);
        }

        /**
         * Adds the given element to the front of the queue.
         * @return True if the element was added; false if the queue is closed (the value is left untouched).
         */
        bool enqueueFront(T&& value)
        {
            return push(true, std::move(value));
        }

        /**
         * Retrieves the first element without waiting.
         * @param out Receives the element.
         * @return True if an element was written to out; false if the queue is empty or closed.
         */
        bool tryDequeue(T& out)
        {
            std::lock_guard lock{_mutex};

            if (_queue.empty() || _closed) {
                return false;
            }

            out = _queue.popFront();
            return true;
        }

        /**
         * Retrieves up to maxCount elements without waiting.
         * @param out Receives the elements, in queue order.
         * @param maxCount The maximum number of elements to retrieve.
         * @return The number of elements retrieved.
         */
        template <typename OutputIterator>
        std::size_t tryDequeueBulk(OutputIterator out, const std::size_t maxCount)
        {
            std::lock_guard lock{_mutex};
            return _closed ? 0 : takeLocked(out, maxCount);
        }

        /**
         * Retrieves the first element, waiting until there is one or the queue is closed.
         * @param out Receives the element.
         * @return True if an element was written to out; false if the queue is closed.
         */
        bool waitDequeue(T& out)
        {
            std::unique_lock lock{_mutex};
            const WaiterScope waiter{*this};

            _condition.wait(lock, [this] { return !_queue.empty() || _closed; });

            if (_closed) {
                return false;
            }

            out = _queue.popFront();
            return true;
        }

        /**
         * Retrieves the first element, waiting until there is one, the queue is closed, or a stop is
         * requested through the token. Elements that are already queued are still retrieved after a stop
         * request; a closed queue is always empty, as close() takes its elements.
         *
         * @param token Interrupts the wait.
         * @param out Receives the element.
         * @return True if an element was written to out; false if the queue is closed, or a stop was requested
         *         and the queue is empty.
         */
        bool waitDequeue(const std::stop_token& token, T& out)
        {
            std::unique_lock lock{_mutex};
            const WaiterScope waiter{*this};

            _condition.wait(lock, token, [this] { return !_queue.empty() || _closed; });

            if (_queue.empty()) {
                return false;
            }

            out = _queue.popFront();
            return true;
        }

        /**
         * Like waitDequeue(token, out), but retrieves up to maxCount elements at once.
         * @return The number of elements retrieved: zero if the queue is closed, or a stop was requested and
         *         the queue is empty.
         */
        template <typename OutputIterator>
        std::size_t waitDequeueBulk(const std::stop_token& token, OutputIterator out, const std::size_t maxCount)
        {
            std::unique_lock lock{_mutex};
            const WaiterScope waiter{*this};

            _condition.wait(lock, token, [this] { return !_queue.empty() || _closed; });
            return takeLocked(out, maxCount);
        }

        /**
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
        }

        /**
         * @return The number of elements in the queue.
         */
        std::size_t size() const
        {
            std::lock_guard lock{_mutex};
            return _queue.size();
        }

        /**
         * Closes the queue: further operations fail or return false, and the threads waiting in it wake up.
         * @return The elements that were still in the queue (none if it was already closed).
         */
        std::vector<T> close()
        {
            std::vector<T> remaining;
            {
                std::lock_guard lock{_mutex};

                if (_closed) {
                    return remaining;
                }

                remaining.reserve(_queue.size());
                while (!_queue.empty()) {
                    remaining.push_back(_queue.popFront());
                }
                _closed = true;
            }

            _condition.notify_all();
            return remaining;
        }

        /**
         * @return True if the queue is closed, false otherwise.
         */
        bool isClosed() const
        {
            std::lock_guard lock{_mutex};
            return _closed;
        }

        /**
         * Returns the transformed items of the queue.
         * @tparam TOut The type of the resulting list elements.
         * @param selector The transformation function to apply to each element.
         * @return A vector containing the transformed items.
         */
        template <typename TOut>
        std::vector<TOut> toList(const std::function<TOut(const T&)>& selector) const
        {
            std::lock_guard lock{_mutex};
            std::vector<TOut> list;
            list.reserve(_queue.size());
            for (std::size_t i = 0; i < _queue.size(); ++i) {
                list.emplace_back(selector(_queue[i]));
            }
            return list;
        }

      private:
        // Counts the threads blocked in the queue, under the lock, so the destructor can wait for them to leave.
        struct WaiterScope
        {
            explicit WaiterScope(ConcurrentQueue& queue) noexcept: _queue{queue}
            {
                ++_queue._waiters;
            }

            ~WaiterScope()
            {
                // The lock is still held here (the scope ends before the lock), so the destructor can't
                // destroy the queue between this decrement and the notification.
                if (--_queue._waiters == 0 && _queue._closed) {
                    _queue._condition.notify_all();
                }
            }

            WaiterScope(const WaiterScope&) = delete;
            WaiterScope& operator=(const WaiterScope&) = delete;

          private:
            ConcurrentQueue& _queue;
        };

        template <typename... Args>
        bool push(const bool front, Args&&... args)
        {
            bool wake;
            {
                std::lock_guard lock{_mutex};

                if (_closed) {
                    return false;
                }

                if (front) {
                    _queue.emplaceFront(std::forward<Args>(args)...);
                }
                else {
                    _queue.emplaceBack(std::forward<Args>(args)...);
                }
                wake = _waiters > 0;
            }

            if (wake) {
                _condition.notify_one();
            }
            return true;
        }

        template <typename OutputIterator>
        std::size_t takeLocked(OutputIterator out, const std::size_t maxCount)
        {
            const std::size_t count = std::min(maxCount, _queue.size());
            for (std::size_t i = 0; i < count; ++i) {
                *out++ = _queue.popFront();
            }
            return count;
        }

        mutable std::mutex _mutex;
        std::condition_variable_any _condition;
        RingBuffer<T> _queue;
        int _waiters{0};
        bool _closed{false};
    };

} // namespace vanilo::concurrent

#endif // INC_D8C0265AC7204A5F8B06B620B9F01B70
