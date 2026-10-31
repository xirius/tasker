#ifndef INC_A276C213E55E4D9A9BDCED4035E01132
#define INC_A276C213E55E4D9A9BDCED4035E01132

#include <vanilo/Export.h>

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <source_location>

namespace vanilo::core {

    /// Debugging facilities
    /// ============================================================================================

    /**
     * Writes a printf-style message, prefixed with its source location, to stderr. Use it through
     * VANILO_TRACE, which compiles to nothing in release builds (NDEBUG). The class itself is always
     * available, so a release library can be used from a debug build and vice versa.
     */
    class VANILO_EXPORT Tracer final
    {
      public:
        /**
         * @param location The location written before the message: by default, where the tracer is created.
         */
        explicit Tracer(std::source_location location = std::source_location::current()) noexcept;

        /**
         * @param filename The file name written before the message.
         * @param line The line number written before the message.
         */
        Tracer(const char* filename, unsigned line) noexcept;

        template <typename... Args>
        void operator()(const char* format, const Args&... args) const noexcept
        {
            char buffer[BufferSize];
            const int count = std::snprintf(buffer, BufferSize, "%s(%u): ", _filename, _line);

            if (count < 0) {
                return;
            }

            // snprintf returns the untruncated length, so only append if the prefix fitted.
            if (static_cast<std::size_t>(count) < BufferSize) {
                if constexpr (sizeof...(args) > 0) {
                    std::snprintf(buffer + count, BufferSize - count, format, args...);
                }
                else {
                    std::snprintf(buffer + count, BufferSize - count, "%s", format);
                }
            }

            std::cerr << buffer << std::endl; // snprintf always null-terminates, even when truncating
        }

      private:
        static constexpr std::size_t BufferSize = 256;

        const char* _filename;
        unsigned _line;
    };

} // namespace vanilo::core

#if defined NDEBUG
#define VANILO_TRACE(...) ((void)0)
#define VANILO_ASSERT(expression) ((void)0)
#define VANILO_VERIFY(expression) ((void)(expression))
#else
#define VANILO_TRACE vanilo::core::Tracer{}
#define VANILO_ASSERT(expression) assert(expression)
#define VANILO_VERIFY(expression) assert(expression)
#endif

#endif // INC_A276C213E55E4D9A9BDCED4035E01132
