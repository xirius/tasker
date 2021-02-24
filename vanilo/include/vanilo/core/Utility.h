#ifndef INC_17E2C0F764D04CBA82065789EC6CA7E5
#define INC_17E2C0F764D04CBA82065789EC6CA7E5

#include <tuple>

namespace vanilo::core {

    /// Index sequence helper.
    /// ========================================================================

    template <std::size_t N, typename Sequence>
    struct OffsetSequence;

    template <std::size_t N, std::size_t... Ints>
    struct OffsetSequence<N, std::index_sequence<Ints...>>
    {
        using Type = std::index_sequence<Ints + N...>;
    };

    /// Tuple utilities
    /// ========================================================================

    struct TupleHelper
    {
        template <typename Tuple, std::size_t... Ints>
        static auto select(Tuple&& tuple, std::index_sequence<Ints...>) -> std::tuple<std::tuple_element_t<Ints, Tuple>...>
        {
            return {std::get<Ints>(std::forward<Tuple>(tuple))...};
        }
    };

} // namespace vanilo::core

#endif // INC_17E2C0F764D04CBA82065789EC6CA7E5