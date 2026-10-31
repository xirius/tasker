#include <vanilo/core/Tracer.h>

using namespace vanilo::core;

/// Tracer
/// ================================================================================================

Tracer::Tracer(const char* filename, const unsigned line) noexcept: _filename{filename}, _line{line}
{
}
