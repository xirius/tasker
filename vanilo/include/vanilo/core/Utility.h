#ifndef INC_17E2C0F764D04CBA82065789EC6CA7E5
#define INC_17E2C0F764D04CBA82065789EC6CA7E5

#include <tuple>

namespace vanilo::core {

    /// Index sequence helper.
    /// ============================================================================================

    template <std::size_t N, typename Sequence>
    struct OffsetSequence;

    template <std::size_t N, std::size_t... Ints>
    struct OffsetSequence<N, std::index_sequence<Ints...>>
    {
        using Type = std::index_sequence<Ints + N...>;
    };

    /// Tuple utilities
    /// ============================================================================================

    template <typename>
    struct IsTuple: std::false_type
    {
    };

    template <typename... T>
    struct IsTuple<std::tuple<T...>>: std::true_type
    {
    };

    /**
     * Function object used to unwraps the wrapped references.
     */
    template <typename Arg, bool = false>
    class RefUnwrapper;

    /**
     * If the argument is std::reference_wrapper<Arg>, returns the underlying reference.
     */
    template <typename Arg>
    class RefUnwrapper<std::reference_wrapper<Arg>, false>
    {
      public:
        template <typename CvRef>
        Arg& operator()(CvRef& arg) const volatile
        {
            return arg.get();
        }
    };

    /**
     *  If the argument is just a value, returns a reference to that value.
     *  The cv-qualifiers on the reference are determined by the caller.
     */
    template <typename Arg>
    class RefUnwrapper<Arg, false>
    {
      public:
        template <typename CvArg>
        CvArg&& operator()(CvArg&& arg) const volatile
        {
            return std::forward<CvArg>(arg);
        }
    };

    struct TupleHelper
    {
        template <typename Tuple, std::size_t... Ints>
        static constexpr auto select(Tuple&& tuple, std::index_sequence<Ints...>) -> std::tuple<std::tuple_element_t<Ints, Tuple>...>
        {
            return {std::get<Ints>(std::forward<Tuple>(tuple))...};
        }
    };

    template <typename Return>
    struct InvokeHelper
    {
        template <typename Callable, typename... Args, std::size_t... Indexes>
        static Return invoke(Callable& func, std::tuple<Args...>& args, std::index_sequence<Indexes...>)
        {
            return func(RefUnwrapper<Args>()(std::get<Indexes>(args))...);
        }
    };

    template <>
    struct InvokeHelper<void>
    {
        template <typename Callable, typename... Args, std::size_t... Indexes>
        static void invoke(Callable& func, std::tuple<Args...>& args, std::index_sequence<Indexes...>)
        {
            func(RefUnwrapper<Args>()(std::get<Indexes>(args))...);
        }
    };

} // namespace vanilo::core::utility

#endif // INC_17E2C0F764D04CBA82065789EC6CA7E5