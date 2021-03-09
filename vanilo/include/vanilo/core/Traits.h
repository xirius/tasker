#ifndef INC_5CCBDE6C07F04D0090FDBF17F4FDADC7
#define INC_5CCBDE6C07F04D0090FDBF17F4FDADC7

#include <tuple>

namespace vanilo::core::traits {

    namespace internal {

        template <typename Signature, typename = void>
        struct FunctionTraitsBase;

    } // namespace internal

    /// Function traits
    /// ============================================================================================

    template <typename Signature>
    struct FunctionTraits: internal::FunctionTraitsBase<Signature>
    {
        static constexpr bool IsMemberFnPtr = std::is_member_function_pointer<Signature>::value;
    };

    namespace internal {

        struct VoidType
        {
            using type = void;
        };

        template <typename... Args>
        struct Arguments
        {
            template <std::size_t N>
            struct Element
            {
                using Type =
                    typename std::conditional<(N < sizeof...(Args)), std::tuple_element<N, std::tuple<Args...>>, VoidType>::type::type;
            };
        };

#define FUNCTION_TRAITS_TEMPLATE(REF)                                                 \
    template <typename TReturn, typename... Args>                                     \
    struct FunctionTraitsBase<TReturn (*)(Args...) REF>                               \
    {                                                                                 \
        using ReturnType   = TReturn;                                                 \
        using ClassType    = void;                                                    \
        using ArgsType     = typename std::tuple<Args...>;                            \
        using PureArgsType = typename std::tuple<typename std::decay<Args>::type...>; \
                                                                                      \
        template <std::size_t N>                                                      \
        using Arg = typename Arguments<Args...>::template Element<N>::Type;           \
                                                                                      \
        static constexpr auto Arity    = sizeof...(Args);                             \
        static constexpr bool IsLambda = false;                                       \
    };

        FUNCTION_TRAITS_TEMPLATE()
        FUNCTION_TRAITS_TEMPLATE(noexcept)

#undef FUNCTION_TRAITS_TEMPLATE

#define MEMBER_FUNCTION_TRAITS_TEMPLATE(CV, REF, L_VAL, R_VAL)                        \
    template <typename TReturn, typename TClass, typename... Args>                    \
    struct FunctionTraitsBase<TReturn (TClass::*)(Args...) CV REF>                    \
    {                                                                                 \
        using ReturnType   = TReturn;                                                 \
        using ClassType    = TClass;                                                  \
        using ArgsType     = typename std::tuple<Args...>;                            \
        using PureArgsType = typename std::tuple<typename std::decay<Args>::type...>; \
                                                                                      \
        template <std::size_t N>                                                      \
        using Arg = typename Arguments<Args...>::template Element<N>::Type;           \
                                                                                      \
        static constexpr auto Arity    = sizeof...(Args);                             \
        static constexpr bool IsLambda = false;                                       \
    };

#define FUNCTION_TRAITS(REF, L_VAL, R_VAL)                     \
    MEMBER_FUNCTION_TRAITS_TEMPLATE(, REF, LVAL, RVAL)         \
    MEMBER_FUNCTION_TRAITS_TEMPLATE(const, REF, LVAL, RVAL)    \
    MEMBER_FUNCTION_TRAITS_TEMPLATE(volatile, REF, LVAL, RVAL) \
    MEMBER_FUNCTION_TRAITS_TEMPLATE(const volatile, REF, LVAL, RVAL)

        FUNCTION_TRAITS(, true_type, true_type)
        FUNCTION_TRAITS(&, true_type, false_type)
        FUNCTION_TRAITS(&&, false_type, true_type)
        FUNCTION_TRAITS(noexcept, true_type, true_type)
        FUNCTION_TRAITS(&noexcept, true_type, false_type)
        FUNCTION_TRAITS(&&noexcept, false_type, true_type)

#undef FUNCTION_TRAITS
#undef MEMBER_FUNCTION_TRAITS_TEMPLATE

        /// Lambda function traits
        template <typename Signature>
        struct FunctionTraitsBase<Signature, std::void_t<decltype(&Signature::operator())>>
            : public FunctionTraitsBase<decltype(&Signature::operator())>
        {
            static constexpr bool IsLambda = true;
        };

    } // namespace internal
} // namespace vanilo::core::traits

#endif // INC_5CCBDE6C07F04D0090FDBF17F4FDADC7