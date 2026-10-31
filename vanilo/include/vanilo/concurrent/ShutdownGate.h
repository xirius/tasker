#ifndef VANILO_CONCURRENT_SHUTDOWN_GATE_H
#define VANILO_CONCURRENT_SHUTDOWN_GATE_H

#include <atomic>

namespace vanilo::concurrent {

    /**
     * A gate that lets operations in until it is closed, and whose close() waits for the operations that are
     * still inside. Use it to shut a resource down while other threads may be using it:
     *
     *     if (const auto entry = gate.enter()) {
     *         // use the resource
     *     }
     *
     *     gate.close(); // no new entries; returns once every entry has left
     */
    class ShutdownGate
    {
      public:
        class Entry;

        ShutdownGate() noexcept = default;
        ShutdownGate(const ShutdownGate&) = delete;
        ShutdownGate& operator=(const ShutdownGate&) = delete;

        /**
         * Enters the gate, unless it is closed.
         * @return True if entered: call leave() when done. False if the gate is closed.
         */
        bool tryEnter() noexcept
        {
            if (_closed.load(std::memory_order_acquire)) {
                return false;
            }

            // Increment, then re-check the flag. close() does the mirror image (set the flag, then read the
            // counter), so both sides need sequential consistency: with release/acquire alone each could read
            // the other's old value (store buffering) and a caller would enter after close() returned.
            _inside.fetch_add(1, std::memory_order_seq_cst);

            if (_closed.load(std::memory_order_seq_cst)) {
                leave();
                return false;
            }

            return true;
        }

        /**
         * Leaves the gate after a successful tryEnter().
         */
        void leave() noexcept
        {
            // The gate must not be read after the decrement: close() may see zero, return, and the gate be
            // destroyed. notify_all() only uses the address to find the waiters (it is cheap when there are none).
            if (_inside.fetch_sub(1, std::memory_order_seq_cst) == 1) {
                _inside.notify_all();
            }
        }

        /**
         * Closes the gate, so no new entries are let in, and waits until all current entries have left.
         * Closing a closed gate only waits.
         */
        void close() noexcept
        {
            _closed.store(true, std::memory_order_seq_cst); // see tryEnter()

            for (int inside = _inside.load(std::memory_order_seq_cst); inside > 0; inside = _inside.load(std::memory_order_seq_cst)) {
                _inside.wait(inside, std::memory_order_seq_cst);
            }
        }

        /**
         * @return True if the gate is closed.
         */
        [[nodiscard]] bool isClosed() const noexcept
        {
            return _closed.load(std::memory_order_acquire);
        }

        /**
         * Enters the gate for the lifetime of the returned entry.
         * @return An entry that converts to true if the gate was entered, or to false if it is closed.
         */
        [[nodiscard]] Entry enter() noexcept;

      private:
        std::atomic<int> _inside{0};
        std::atomic<bool> _closed{false};
    };

    /**
     * RAII entry of a ShutdownGate: leaves the gate when destroyed.
     */
    class ShutdownGate::Entry
    {
        friend class ShutdownGate;

      public:
        Entry() noexcept = default;

        Entry(Entry&& other) noexcept: _gate{other._gate}
        {
            other._gate = nullptr;
        }

        Entry& operator=(Entry&& other) noexcept
        {
            if (this != &other) {
                if (_gate) {
                    _gate->leave();
                }
                _gate = other._gate;
                other._gate = nullptr;
            }
            return *this;
        }

        ~Entry()
        {
            if (_gate) {
                _gate->leave();
            }
        }

        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;

        explicit operator bool() const noexcept
        {
            return _gate != nullptr;
        }

      private:
        explicit Entry(ShutdownGate* gate) noexcept: _gate{gate}
        {
        }

        ShutdownGate* _gate{nullptr};
    };

    inline ShutdownGate::Entry ShutdownGate::enter() noexcept
    {
        return tryEnter() ? Entry{this} : Entry{};
    }

} // namespace vanilo::concurrent

#endif // VANILO_CONCURRENT_SHUTDOWN_GATE_H
