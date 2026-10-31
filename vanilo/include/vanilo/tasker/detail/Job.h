#ifndef VANILO_TASKER_DETAIL_JOB_H
#define VANILO_TASKER_DETAIL_JOB_H

#include <concepts>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace vanilo::tasker::detail {

    /**
     * What a Job holds: an object that is either run, or canceled when its executor shuts down before it could
     * run. Either call consumes it (the job destroys it afterwards).
     */
    template <typename Target>
    concept JobTarget = std::move_constructible<Target> && requires(Target& target) {
        target.run();
        { target.cancel() } noexcept;
    };

    /**
     * A unit of work in an executor's queue: a move-only, type-erased JobTarget. Targets of up to InlineSize bytes
     * are stored inline, so queueing them doesn't allocate; trivially copyable ones are moved with a memcpy.
     */
    class Job
    {
      public:
        static constexpr std::size_t InlineSize = 56;

        Job() noexcept = default;

        template <JobTarget Target, typename... Args>
        [[nodiscard]] static Job make(Args&&... args)
        {
            Job job;
            if constexpr (fitsInline<Target>()) {
                std::construct_at(reinterpret_cast<Target*>(job._storage), std::forward<Args>(args)...);
                job._operations = &InlineOperations<Target>::Table;
            }
            else {
                Target* target = new Target(std::forward<Args>(args)...);
                std::memcpy(job._storage, &target, sizeof(target));
                job._operations = &HeapOperations<Target>::Table;
            }
            return job;
        }

        Job(Job&& other) noexcept
        {
            take(other);
        }

        Job& operator=(Job&& other) noexcept
        {
            if (this != &other) {
                reset();
                take(other);
            }
            return *this;
        }

        ~Job()
        {
            reset();
        }

        Job(const Job&) = delete;
        Job& operator=(const Job&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return _operations != nullptr;
        }

        /**
         * Runs the target, which is destroyed afterwards even if it throws. The job is empty afterwards.
         */
        void run()
        {
            if (const auto operations = std::exchange(_operations, nullptr)) {
                operations->run(_storage);
            }
        }

        /**
         * Cancels the target, which is destroyed afterwards. The job is empty afterwards.
         */
        void cancel() noexcept
        {
            if (const auto operations = std::exchange(_operations, nullptr)) {
                operations->cancel(_storage);
            }
        }

      private:
        struct Operations
        {
            void (*run)(void* storage);
            void (*cancel)(void* storage) noexcept;
            void (*relocate)(void* from, void* to) noexcept; // nullptr: memcpy
            void (*destroy)(void* storage) noexcept;
        };

        template <typename Target>
        static constexpr bool fitsInline() noexcept
        {
            return sizeof(Target) <= InlineSize && alignof(Target) <= alignof(void*) && std::is_nothrow_move_constructible_v<Target>;
        }

        template <typename Target>
        struct InlineOperations
        {
            static Target& target(void* storage) noexcept
            {
                return *std::launder(static_cast<Target*>(storage));
            }

            static void run(void* storage)
            {
                struct Destroy
                {
                    void* storage;
                    ~Destroy()
                    {
                        std::destroy_at(&target(storage));
                    }
                } destroy{storage};
                target(storage).run();
            }

            static void cancel(void* storage) noexcept
            {
                target(storage).cancel();
                std::destroy_at(&target(storage));
            }

            static void relocate(void* from, void* to) noexcept
            {
                std::construct_at(static_cast<Target*>(to), std::move(target(from)));
                std::destroy_at(&target(from));
            }

            static void destroy(void* storage) noexcept
            {
                std::destroy_at(&target(storage));
            }

            static constexpr Operations Table{&run, &cancel, std::is_trivially_copyable_v<Target> ? nullptr : &relocate, &destroy};
        };

        template <typename Target>
        struct HeapOperations
        {
            static Target* target(void* storage) noexcept
            {
                Target* target;
                std::memcpy(&target, storage, sizeof(target));
                return target;
            }

            static void run(void* storage)
            {
                const std::unique_ptr<Target> owned{target(storage)};
                owned->run();
            }

            static void cancel(void* storage) noexcept
            {
                const std::unique_ptr<Target> owned{target(storage)};
                owned->cancel();
            }

            static void destroy(void* storage) noexcept
            {
                delete target(storage);
            }

            static constexpr Operations Table{&run, &cancel, nullptr, &destroy};
        };

        void take(Job& other) noexcept
        {
            if (!other._operations) {
                return;
            }
            if (other._operations->relocate) {
                other._operations->relocate(other._storage, _storage);
            }
            else {
                std::memcpy(_storage, other._storage, InlineSize);
            }
            _operations = std::exchange(other._operations, nullptr);
        }

        void reset() noexcept
        {
            if (const auto operations = std::exchange(_operations, nullptr)) {
                operations->destroy(_storage);
            }
        }

        alignas(void*) std::byte _storage[InlineSize];
        const Operations* _operations{nullptr};
    };

} // namespace vanilo::tasker::detail

#endif // VANILO_TASKER_DETAIL_JOB_H
