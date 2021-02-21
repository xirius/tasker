#ifndef INC_A276C213E55E4D9A9BDCED4035E01132
#define INC_A276C213E55E4D9A9BDCED4035E01132

#include <vanilo/Export.h>

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <type_traits>

namespace vanilo::core {

    /**
     * @return The size of the static array.
     */
    template <typename T, size_t N>
    constexpr size_t countOf(T (&)[N]) noexcept
    {
        return std::extent<T[N]>::value;
    }

    /// Debugging facilities
    /// ========================================================================

#if defined NDEBUG
#define TRACE(ignore, ...) ((void)0)
#define VERIFY(expression) expression
#else
#define VERIFY ASSERT
#define TRACE              \
    vanilo::core::Tracer   \
    {                      \
        __FILE__, __LINE__ \
    }

#define ASSERT(expr) \
    for (int s{1}; s-- && !(expr); assert(expr)) TRACE

    /**
     * Tracer helper class.
     */
    class VANILO_EXPORT Tracer final
    {
      public:
        Tracer(const char* filename, unsigned line) noexcept;

        template <typename... Args>
        void operator()(const char* format, const Args&... args) noexcept
        {
            char buffer[BUFFER_SIZE];
            auto count = snprintf(buffer, countOf(buffer), "%s(%d): ", _filename, _line);

            ASSERT(count >= 0 && count < static_cast<int>(countOf(buffer)));

            count = (sizeof...(args) ? append(buffer, count, format, args...) : append(buffer, count, format));

            ASSERT(count >= 0 && count < static_cast<int>(countOf(buffer)));

            std::cerr << buffer << std::endl;
        }

      private:
        static constexpr size_t BUFFER_SIZE = 256;

        template <typename... Args>
        int append(char* buffer, int count, const char* format, const Args&... args) const noexcept
        {
            return snprintf(buffer + count, BUFFER_SIZE - count, format, args...);
        }

        static int append(char* buffer, const int count, const char* format) noexcept
        {
            return snprintf(buffer + count, BUFFER_SIZE - count, "%s", format);
        }

        const char* _filename;
        const unsigned _line;
    };

#endif

} // namespace vanilo::core

#endif // INC_A276C213E55E4D9A9BDCED4035E01132