#ifndef INC_CF1B33A15FDE47BDA967EACB24A90BED
#define INC_CF1B33A15FDE47BDA967EACB24A90BED

#include <vanilo/Export.h>
#include <vanilo/tasker/CancellationToken.h>

#include <cassert>
#include <future>
#include <memory>

namespace vanilo {
namespace tasker {

    class TaskExecutor;

    /// Task interface
    /// ========================================================================

    class Task
    {
    };

    namespace details {

        /**
         * Provides an optional facility to store a value or an exception in the promise-future channel.
         * @tparam T result type
         */
        template <typename T>
        class VANILO_EXPORT OptionalPromise
        {
          public:
            ~OptionalPromise()
            {
                if (_valid) {
                    promise()->~promise();
                }
            }

            bool isValid() const noexcept
            {
                return _valid;
            }

            template <typename R = T>
            auto getFuture() -> typename std::enable_if<std::is_void<R>::value, std::future<R>>::type
            {
                assert(!_valid && "Future can only be taken once!");
                _valid = true;
                new (&_data) std::promise<R>{};
                return promise()->get_future();
            }

            template <typename R = T>
            auto getFuture() -> typename std::enable_if<!std::is_void<R>::value, std::future<R>>::type
            {
                assert(!_valid && "Future can only be taken once!");
                _valid = true;
                new (&_data) std::promise<R>{};
                return promise()->get_future();
            }

            template <typename R = T, typename std::enable_if<std::is_void<R>::value, bool>::type = true>
            void setValue()
            {
                assert(_valid && "Future must be taken first!");
                promise()->set_value();
            }

            template <typename Q = T, typename std::enable_if<!std::is_void<Q>::value, bool>::type = true>
            void setValue(Q&& value)
            {
                assert(_valid && "Future must be taken first!");
                promise()->set_value(std::forward<T>(value));
            }

            void setException(std::exception_ptr exceptionPtr)
            {
                assert(_valid && "Future must be taken first!");
                promise()->set_exception(exceptionPtr);
            }

          private:
            using storage_t = typename std::aligned_storage<sizeof(std::promise<T>), alignof(std::promise<T>)>::type;

            std::promise<T>* promise()
            {
                return reinterpret_cast<std::promise<T>*>(&_data);
            }

            storage_t _data{};
            bool _valid{false};
        };
    } // namespace details

} // namespace tasker
} // namespace vanilo

#endif // INC_CF1B33A15FDE47BDA967EACB24A90BED