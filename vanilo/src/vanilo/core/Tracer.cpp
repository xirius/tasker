#include <vanilo/core/Tracer.h>

using namespace vanilo::core;

/// Tracer
/// ================================================================================================

Tracer::Tracer(const std::source_location location) noexcept: _filename{location.file_name()}, _line{location.line()}
{
}

Tracer::Tracer(const char* filename, const unsigned line) noexcept: _filename{filename}, _line{line}
{
}
