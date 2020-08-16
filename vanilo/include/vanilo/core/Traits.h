#ifndef INC_5CCBDE6C07F04D0090FDBF17F4FDADC7
#define INC_5CCBDE6C07F04D0090FDBF17F4FDADC7

#include <tuple>

namespace vanilo::core::traits {

    /// Function traits
    /// ========================================================================

    template <typename T, typename = void>
    struct function_traits;

    template <typename TReturn, typename... Args>
    struct function_traits<TReturn (*)(Args...)>
    {
        static constexpr auto arity = sizeof...(Args);
        using return_type           = TReturn;
        using class_type            = void;
        using args_type             = typename std::tuple<Args...>;
    };

    template <typename TReturn, typename TClass, typename... Args>
    struct function_traits<TReturn (TClass::*)(Args...)>
    {
        static constexpr auto arity = sizeof...(Args);
        using return_type           = TReturn;
        using class_type            = void;
        using args_type             = typename std::tuple<Args...>;
    };

    template <typename TReturn, typename TClass, typename... Args>
    struct function_traits<TReturn (TClass::*)(Args...) const> // const
    {
        static constexpr auto arity = sizeof...(Args);
        using return_type           = TReturn;
        using class_type            = TClass;
        using args_type             = typename std::tuple<Args...>;
    };

    template <typename T>
    struct function_traits<T, std::void_t<decltype(&T::operator())>>: public function_traits<decltype(&T::operator())>
    {
    };
} // namespace vanilo::core::traits

#endif // INC_5CCBDE6C07F04D0090FDBF17F4FDADC7