#ifndef INC_5CCBDE6C07F04D0090FDBF17F4FDADC7
#define INC_5CCBDE6C07F04D0090FDBF17F4FDADC7

#include <tuple>

namespace vanilo::core::traits {

    namespace details {

        template <typename Signature, typename = void>
        struct FunctionTraitsBase;

    } // namespace details

    /// Function traits
    /// ========================================================================

    template <typename Signature>
    struct FunctionTraits: details::FunctionTraitsBase<Signature>
    {
        static constexpr bool IsMemberFnPtr = std::is_member_function_pointer<Signature>::value;
    };

    namespace details {

        template <typename TReturn, typename... Args>
        struct FunctionTraitsBase<TReturn (*)(Args...)>
        {
            static constexpr auto arity = sizeof...(Args);
            using ReturnType            = TReturn;
            using ClassType             = void;
            using ArgTypes              = typename std::tuple<Args...>;
        };

#define FUNCTION_TRAITS_TEMPLATE(CV, REF, L_VAL, R_VAL)             \
    template <typename TReturn, typename TClass, typename... Args>  \
    struct FunctionTraitsBase<TReturn (TClass::*)(Args...) CV REF>  \
    {                                                               \
        using ReturnType            = TReturn;                      \
        using ClassType             = TClass;                       \
        using ArgTypes              = typename std::tuple<Args...>; \
        static constexpr auto Arity = sizeof...(Args);              \
    };

#define FUNCTION_TRAITS(REF, L_VAL, R_VAL)              \
    FUNCTION_TRAITS_TEMPLATE(, REF, LVAL, RVAL)         \
    FUNCTION_TRAITS_TEMPLATE(const, REF, LVAL, RVAL)    \
    FUNCTION_TRAITS_TEMPLATE(volatile, REF, LVAL, RVAL) \
    FUNCTION_TRAITS_TEMPLATE(const volatile, REF, LVAL, RVAL)

        FUNCTION_TRAITS(, true_type, true_type)
        FUNCTION_TRAITS(&, true_type, false_type)
        FUNCTION_TRAITS(&&, false_type, true_type)
        FUNCTION_TRAITS(noexcept, true_type, true_type)
        FUNCTION_TRAITS(&noexcept, true_type, false_type)
        FUNCTION_TRAITS(&&noexcept, false_type, true_type)

#undef FUNCTION_TRAITS
#undef FUNCTION_TRAITS_TEMPLATE

        template <typename Signature>
        struct FunctionTraitsBase<Signature, std::void_t<decltype(&Signature::operator())>>
            : public FunctionTraitsBase<decltype(&Signature::operator())>
        {
        };

    } // namespace details
} // namespace vanilo::core::traits

#endif // INC_5CCBDE6C07F04D0090FDBF17F4FDADC7