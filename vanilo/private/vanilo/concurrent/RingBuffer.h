#ifndef VANILO_CONCURRENT_RING_BUFFER_H
#define VANILO_CONCURRENT_RING_BUFFER_H

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace vanilo::concurrent {

    /**
     * A growable circular buffer: a double-ended queue in one contiguous allocation, so pushing and
     * popping don't allocate once it has grown to its working size. Not thread-safe.
     */
    template <typename T>
    class RingBuffer
    {
      public:
        RingBuffer() noexcept = default;

        RingBuffer(const RingBuffer&) = delete;
        RingBuffer& operator=(const RingBuffer&) = delete;

        RingBuffer(RingBuffer&& other) noexcept
            : _slots{std::exchange(other._slots, nullptr)}, _capacity{std::exchange(other._capacity, 0)},
              _head{std::exchange(other._head, 0)}, _size{std::exchange(other._size, 0)}
        {
        }

        RingBuffer& operator=(RingBuffer&& other) noexcept
        {
            if (this != &other) {
                clear();
                deallocate();
                _slots = std::exchange(other._slots, nullptr);
                _capacity = std::exchange(other._capacity, 0);
                _head = std::exchange(other._head, 0);
                _size = std::exchange(other._size, 0);
            }
            return *this;
        }

        ~RingBuffer()
        {
            clear();
            deallocate();
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return _size == 0;
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return _size;
        }

        template <typename... Args>
        T& emplaceBack(Args&&... args)
        {
            reserveOneMore();
            T* slot = slotAt(_size);
            std::construct_at(slot, std::forward<Args>(args)...);
            ++_size;
            return *slot;
        }

        template <typename... Args>
        T& emplaceFront(Args&&... args)
        {
            reserveOneMore();
            const std::size_t head = (_head + _capacity - 1) & (_capacity - 1);
            T* slot = _slots + head;
            std::construct_at(slot, std::forward<Args>(args)...);
            _head = head;
            ++_size;
            return *slot;
        }

        /**
         * Removes the first element. The buffer must not be empty.
         */
        T popFront() noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            T* slot = _slots + _head;
            T value = std::move(*slot);
            std::destroy_at(slot);
            _head = (_head + 1) & (_capacity - 1);
            --_size;
            return value;
        }

        T& front() noexcept
        {
            return _slots[_head];
        }

        T& operator[](const std::size_t index) noexcept
        {
            return *slotAt(index);
        }

        const T& operator[](const std::size_t index) const noexcept
        {
            return *slotAt(index);
        }

        void clear() noexcept
        {
            for (std::size_t i = 0; i < _size; ++i) {
                std::destroy_at(slotAt(i));
            }
            _head = 0;
            _size = 0;
        }

      private:
        [[nodiscard]] T* slotAt(const std::size_t index) const noexcept
        {
            return _slots + ((_head + index) & (_capacity - 1));
        }

        void reserveOneMore()
        {
            if (_size < _capacity) {
                return;
            }

            // The capacity is a power of two, so positions wrap with a mask.
            const std::size_t capacity = _capacity ? _capacity * 2 : InitialCapacity;
            T* slots = static_cast<T*>(::operator new(capacity * sizeof(T), std::align_val_t{alignof(T)}));

            for (std::size_t i = 0; i < _size; ++i) {
                T* from = slotAt(i);
                std::construct_at(slots + i, std::move(*from)); // T must be nothrow-movable
                std::destroy_at(from);
            }

            deallocate();
            _slots = slots;
            _capacity = capacity;
            _head = 0;
        }

        void deallocate() noexcept
        {
            if (_slots) {
                ::operator delete(_slots, std::align_val_t{alignof(T)});
                _slots = nullptr;
            }
        }

        static_assert(std::is_nothrow_move_constructible_v<T>, "RingBuffer elements must be nothrow-movable");
        static constexpr std::size_t InitialCapacity = 16;

        T* _slots{nullptr};
        std::size_t _capacity{0};
        std::size_t _head{0};
        std::size_t _size{0};
    };

} // namespace vanilo::concurrent

#endif // VANILO_CONCURRENT_RING_BUFFER_H
