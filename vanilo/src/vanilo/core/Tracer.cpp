#include <vanilo/core/Tracer.h>

using namespace vanilo::core;

#if !defined NDEBUG

/// --- Tracer class
/// ============================================================================

Tracer::Tracer(const char* filename, const unsigned line) noexcept
    :_filename{ filename }, _line{ line }
{
}

#endif