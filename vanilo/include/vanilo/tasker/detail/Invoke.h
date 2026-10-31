#ifndef VANILO_TASKER_DETAIL_INVOKE_H
#define VANILO_TASKER_DETAIL_INVOKE_H

#include <concepts>
#include <exception>
#include <functional>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

// How tasks and handlers are called. A task is called with, in this order: the object (for a member function),
// the chain's std::stop_token (if the function takes one there), the bound arguments, and the previous task's
// result (spread into separate arguments if it is a std::tuple the function doesn't take as a whole). Bound
// std::reference_wrapper arguments are passed as the references they wrap.

namespace vanilo::tasker::detail {

    /// Bound arguments
    /// ============================================================================================

    template <typename T>
    struct Unwrapped
    {
        using Type = T&;
    };

    template <typename T>
    struct Unwrapped<std::reference_wrapper<T>>
    {
        using Type = T&;
    };

    template <typename T>
    using UnwrappedType = typename Unwrapped<T>::Type;

    template <typename T>
    constexpr UnwrappedType<T> unwrap(T& value) noexcept
    {
        return value;
    }

    template <typename T>
    constexpr T& unwrap(std::reference_wrapper<T>& value) noexcept
    {
        return value.get();
    }

    template <typename T>
    struct IsTuple: std::false_type
    {
    };

    template <typename... T>
    struct IsTuple<std::tuple<T...>>: std::true_type
    {
    };

    /// Call shapes
    /// ============================================================================================

    /**
     * Whether F (stored as F, called as an lvalue) can be called with the bound arguments Bound (a std::tuple of
     * the stored types), the inputs I, and the token if WithToken. Result is the call's result type.
     */
    template <typename F, typename Bound, bool WithToken, typename... I>
    struct Shape: std::false_type
    {
    };

    template <typename F, typename... B, typename... I>
    requires std::is_invocable_v<F&, UnwrappedType<B>..., I...>
    struct Shape<F, std::tuple<B...>, false, I...>: std::true_type
    {
        using Result = std::invoke_result_t<F&, UnwrappedType<B>..., I...>;
    };

    // The token is the first argument, or the second one for a member function (after the object).
    template <typename F, typename... B, typename... I>
    requires(!std::is_member_pointer_v<F>) && std::is_invocable_v<F&, const std::stop_token&, UnwrappedType<B>..., I...>
    struct Shape<F, std::tuple<B...>, true, I...>: std::true_type
    {
        using Result = std::invoke_result_t<F&, const std::stop_token&, UnwrappedType<B>..., I...>;
    };

    template <typename F, typename Object, typename... B, typename... I>
    requires std::is_member_pointer_v<F> &&
             std::is_invocable_v<F&, UnwrappedType<Object>, const std::stop_token&, UnwrappedType<B>..., I...>
    struct Shape<F, std::tuple<Object, B...>, true, I...>: std::true_type
    {
        using Result = std::invoke_result_t<F&, UnwrappedType<Object>, const std::stop_token&, UnwrappedType<B>..., I...>;
    };

    template <bool WithToken, typename F, typename... B, typename... I>
    decltype(auto) callShape(F& function, std::tuple<B...>& bound, const std::stop_token& token, I&&... inputs)
    {
        return std::apply([&](auto&... arguments) -> decltype(auto) {
            if constexpr (!WithToken) {
                return std::invoke(function, unwrap(arguments)..., std::forward<I>(inputs)...);
            }
            else if constexpr (!std::is_member_pointer_v<F>) {
                return std::invoke(function, token, unwrap(arguments)..., std::forward<I>(inputs)...);
            }
            else {
                return [&](auto& object, auto&... rest) -> decltype(auto) {
                    return std::invoke(function, unwrap(object), token, unwrap(rest)..., std::forward<I>(inputs)...);
                }(arguments...);
            }
        }, bound);
    }

    /// Task invocation
    /// ============================================================================================

    enum class InputForm
    {
        NotViable,
        Empty,  // no input (the first task, or the previous task returned void)
        Value,  // the input as an rvalue
        LValue, // the input as an lvalue
        Spread, // the elements of a std::tuple input, as rvalues
    };

    template <typename F, typename Bound, bool WithToken, typename Input>
    struct SpreadShape: std::false_type
    {
    };

    template <typename F, typename Bound, bool WithToken, typename... E>
    struct SpreadShape<F, Bound, WithToken, std::tuple<E...>>: Shape<F, Bound, WithToken, E&&...>
    {
    };

    /**
     * How a task F with the bound arguments Bound is called with the previous result Input (void for none).
     */
    template <typename F, typename Bound, typename Input>
    struct Invocation
    {
      private:
        template <bool WithToken>
        static constexpr InputForm formWith() noexcept
        {
            if constexpr (std::is_void_v<Input>) {
                return Shape<F, Bound, WithToken>::value ? InputForm::Empty : InputForm::NotViable;
            }
            else if constexpr (Shape<F, Bound, WithToken, Input&&>::value) {
                return InputForm::Value;
            }
            else if constexpr (Shape<F, Bound, WithToken, Input&>::value) {
                return InputForm::LValue;
            }
            else if constexpr (SpreadShape<F, Bound, WithToken, Input>::value) {
                return InputForm::Spread;
            }
            else {
                return InputForm::NotViable;
            }
        }

        static constexpr InputForm TokenForm = formWith<true>();
        static constexpr InputForm PlainForm = formWith<false>();

      public:
        static constexpr bool Viable = TokenForm != InputForm::NotViable || PlainForm != InputForm::NotViable;
        static constexpr bool TakesToken = TokenForm != InputForm::NotViable;
        static constexpr InputForm Form = TakesToken ? TokenForm : PlainForm;

      private:
        static auto resultType() noexcept
        {
            if constexpr (!Viable) {
                return std::type_identity<void>{};
            }
            else if constexpr (std::is_void_v<Input>) {
                return std::type_identity<typename Shape<F, Bound, TakesToken>::Result>{};
            }
            else if constexpr (Form == InputForm::Value) {
                return std::type_identity<typename Shape<F, Bound, TakesToken, Input&&>::Result>{};
            }
            else if constexpr (Form == InputForm::LValue) {
                return std::type_identity<typename Shape<F, Bound, TakesToken, Input&>::Result>{};
            }
            else {
                return std::type_identity<typename SpreadShape<F, Bound, TakesToken, Input>::Result>{};
            }
        }

      public:
        /**
         * The task's result as passed on: decayed, so a task returning a reference passes a copy.
         */
        using Result = std::decay_t<typename decltype(resultType())::type>;

        /**
         * Calls the task. input points to the previous result (unused when Input is void).
         */
        template <typename InputPointer>
        static decltype(auto) call(F& function, Bound& bound, const std::stop_token& token, InputPointer input)
        {
            if constexpr (std::is_void_v<Input>) {
                return callShape<TakesToken>(function, bound, token);
            }
            else if constexpr (Form == InputForm::Value) {
                return callShape<TakesToken>(function, bound, token, std::move(*input));
            }
            else if constexpr (Form == InputForm::LValue) {
                return callShape<TakesToken>(function, bound, token, *input);
            }
            else {
                return std::apply([&](auto&&... elements) -> decltype(auto) {
                    return callShape<TakesToken>(function, bound, token, std::forward<decltype(elements)>(elements)...);
                }, std::move(*input));
            }
        }
    };

    /// Concepts
    /// ============================================================================================

    template <typename T>
    concept Storable = std::move_constructible<std::decay_t<T>> && std::constructible_from<std::decay_t<T>, T>;

    /**
     * F, with the arguments Args bound to it, can be a task receiving Input (void for none).
     */
    template <typename T>
    concept Passable = std::is_void_v<T> || std::move_constructible<T>;

    template <typename F, typename Input, typename... Args>
    concept TaskFunction =
        Storable<F> && (Storable<Args> && ...) && Invocation<std::decay_t<F>, std::tuple<std::decay_t<Args>...>, Input>::Viable &&
        Passable<typename Invocation<std::decay_t<F>, std::tuple<std::decay_t<Args>...>, Input>::Result>;

    /**
     * Like TaskFunction, for a periodic task: each run is a copy of the task, so it must be copyable.
     */
    template <typename F, typename Input, typename... Args>
    concept PeriodicTaskFunction =
        TaskFunction<F, Input, Args...> && std::copy_constructible<std::decay_t<F>> && (std::copy_constructible<std::decay_t<Args>> && ...);

    template <typename F, typename Input, typename... Args>
    using TaskResult = typename Invocation<std::decay_t<F>, std::tuple<std::decay_t<Args>...>, Input>::Result;

    /// Error handlers
    /// ============================================================================================

    template <typename F, typename = void>
    struct FirstParameter
    {
        using Type = void; // unknown: a generic lambda or an overloaded function object
    };

    template <typename R, typename P, typename... Rest>
    struct FirstParameter<R (*)(P, Rest...)>
    {
        using Type = P;
    };

    template <typename R, typename P, typename... Rest>
    struct FirstParameter<R (*)(P, Rest...) noexcept>
    {
        using Type = P;
    };

    template <typename M>
    struct MemberFirstParameter
    {
        using Type = void;
    };

#define VANILO_MEMBER_FIRST_PARAMETER(QUALIFIERS)                   \
    template <typename R, typename C, typename P, typename... Rest> \
    struct MemberFirstParameter<R (C::*)(P, Rest...) QUALIFIERS>    \
    {                                                               \
        using Type = P;                                             \
    };

    VANILO_MEMBER_FIRST_PARAMETER()
    VANILO_MEMBER_FIRST_PARAMETER(const)
    VANILO_MEMBER_FIRST_PARAMETER(noexcept)
    VANILO_MEMBER_FIRST_PARAMETER(const noexcept)
    VANILO_MEMBER_FIRST_PARAMETER(&)
    VANILO_MEMBER_FIRST_PARAMETER(const&)
    VANILO_MEMBER_FIRST_PARAMETER(& noexcept)
    VANILO_MEMBER_FIRST_PARAMETER(const& noexcept)

#undef VANILO_MEMBER_FIRST_PARAMETER

    template <typename F>
    struct FirstParameter<F, std::void_t<decltype(&F::operator())>>: MemberFirstParameter<decltype(&F::operator())>
    {
    };

    /**
     * What an error handler F receives: Exception is std::exception_ptr (any error) or the exception type it
     * catches, taken from its first parameter (std::exception for a generic lambda).
     */
    template <typename F>
    struct HandlerTraits
    {
      private:
        using Declared = std::remove_cvref_t<typename FirstParameter<std::decay_t<F>>::Type>;

      public:
        using Exception = std::conditional_t<std::is_void_v<Declared>, std::exception, Declared>;
        static constexpr bool TakesPointer = std::is_same_v<Exception, std::exception_ptr>;

      private:
        using Argument = std::conditional_t<TakesPointer, const std::exception_ptr&, Exception&>;

      public:
        static constexpr bool TakesToken = std::is_invocable_v<std::decay_t<F>&, Argument, const std::stop_token&>;
        static constexpr bool Viable =
            (std::is_class_v<Exception> || TakesPointer) && (TakesToken || std::is_invocable_v<std::decay_t<F>&, Argument>);

      private:
        static auto resultType() noexcept
        {
            if constexpr (!Viable) {
                return std::type_identity<void>{};
            }
            else if constexpr (TakesToken) {
                return std::type_identity<std::invoke_result_t<std::decay_t<F>&, Argument, const std::stop_token&>>{};
            }
            else {
                return std::type_identity<std::invoke_result_t<std::decay_t<F>&, Argument>>{};
            }
        }

      public:
        using Result = typename decltype(resultType())::type;

        /**
         * @return True if the handler is called for the error.
         */
        static bool accepts(const std::exception_ptr& error) noexcept
        {
            if constexpr (TakesPointer) {
                return true;
            }
            else {
                try {
                    std::rethrow_exception(error);
                }
                catch (const Exception&) {
                    return true;
                }
                catch (...) {
                    return false;
                }
            }
        }

        /**
         * Calls the handler with the error, which it accepts.
         */
        static decltype(auto) call(std::decay_t<F>& handler, const std::exception_ptr& error, const std::stop_token& token)
        {
            if constexpr (TakesPointer) {
                return callWith(handler, error, token);
            }
            else {
                try {
                    std::rethrow_exception(error);
                }
                catch (Exception& exception) {
                    return callWith(handler, exception, token);
                }
            }
        }

      private:
        template <typename A>
        static decltype(auto) callWith(std::decay_t<F>& handler, A& argument, const std::stop_token& token)
        {
            if constexpr (TakesToken) {
                return std::invoke(handler, argument, token);
            }
            else {
                return std::invoke(handler, argument);
            }
        }
    };

    /**
     * F can be an onError handler.
     */
    template <typename F>
    concept ErrorHandlerFunction = Storable<F> && HandlerTraits<F>::Viable;

    /**
     * F can be a recover handler for a chain whose result is R: it returns the replacement value.
     */
    template <typename F, typename R>
    concept RecoveryFunction =
        ErrorHandlerFunction<F> &&
        (std::is_void_v<R> ? std::is_void_v<typename HandlerTraits<F>::Result> : std::convertible_to<typename HandlerTraits<F>::Result, R>);

} // namespace vanilo::tasker::detail

#endif // VANILO_TASKER_DETAIL_INVOKE_H
