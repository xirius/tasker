#ifndef INC_532A9E9148F343228FB5256DE9C99443
#define INC_532A9E9148F343228FB5256DE9C99443

#include <vanilo/core/Traits.h>
#include <vanilo/core/Utility.h>

namespace vanilo::core::binder {
    namespace internal {

        /// ArityChecker
        /// ========================================================================================
        template <typename Func, typename... Args>
        struct ArityChecker
        {
        };

        template <typename Return, typename... Args, typename... BoundArgs>
        struct ArityChecker<Return (*)(Args...), BoundArgs...>
        {
            static_assert(sizeof...(BoundArgs) == sizeof...(Args), "Wrong number of arguments for function");
        };

        template <typename Result, typename Class, typename... BoundArgs>
        struct ArityChecker<Result Class::*, BoundArgs...>
        {
            static_assert(
                sizeof...(BoundArgs) == traits::FunctionTraits<Result Class::*>::Arity + 1,
                "Wrong number of arguments for pointer-to-member");
        };

        /// Bind | Type of the function object returned from bind().
        /// ========================================================================================

        template <typename Signature>
        struct Bind;

        template <typename Func, typename... Args>
        struct BindHelper: public ArityChecker<typename std::decay<Func>::type, Args...>
        {
            using FuncType = typename std::decay<Func>::type;
            using Type     = internal::Bind<FuncType(typename std::decay<Args>::type...)>;
        };

        template <typename Functor, typename... BoundArgs>
        class Bind<Functor(BoundArgs...)>
        {
            template <typename... Args>
            using IndexSequence = std::make_index_sequence<sizeof...(Args)>;

            template <typename Arg>
            using UnwrapperType = decltype(RefUnwrapper<typename std::remove_cv<Arg>::type>()(std::declval<Arg&>()));

            template <typename Func, typename... Args>
            // using ResultType = typename std::result_of<Func&(UnwrapperType<Args>&&...)>::type;
            using ResultType = typename traits::FunctionTraits<Func>::ReturnType;

          public:
            Bind(const Bind& other)     = default;
            Bind(Bind&& other) noexcept = default;

            template <typename... Args>
            explicit Bind(const Functor& functor, Args&&... args): _functor{functor}, _boundArgs{std::forward<Args>(args)...}
            {
            }

            template <typename... Args>
            explicit Bind(Functor&& functor, Args&&... args): _functor{std::move(functor)}, _boundArgs{std::forward<Args>(args)...}
            {
            }

            template <typename Func, typename... Args>
            auto rebindPrepend(Func&& func, Args&&... args)
            {
                constexpr auto isMember  = traits::FunctionTraits<Functor>::IsMemberFnPtr;
                constexpr auto argsCount = std::tuple_size_v<std::tuple<BoundArgs...>>;
                using NewIndices         = std::make_index_sequence<argsCount - isMember>;
                using SelectedIndices    = typename OffsetSequence<isMember, std::make_index_sequence<argsCount - isMember>>::Type;

                return this->rebind(
                    std::forward<Func>(func), std::forward_as_tuple(std::forward<Args>(args)...),
                    TupleHelper::select(std::move(_boundArgs), SelectedIndices{}), IndexSequence<Args...>{}, NewIndices{});
            }

            template <typename... Args, typename Result = ResultType<Functor, BoundArgs...>>
            Result operator()(Args&&... args)
            {
                return this->call<Result>(
                    std::forward_as_tuple(std::forward<Args>(args)...), IndexSequence<BoundArgs...>{}, IndexSequence<Args...>{});
            }

          private:
            template <typename Result, typename... Args, std::size_t... Indexes1, std::size_t... Indexes2>
            Result call(std::tuple<Args...> args, std::index_sequence<Indexes1...>, std::index_sequence<Indexes2...>)
            {
                return std::__invoke(
                    _functor, RefUnwrapper<BoundArgs>()(std::get<Indexes1>(_boundArgs))...,
                    RefUnwrapper<Args>()(std::get<Indexes2>(args))...);
            }

            template <typename Func, typename... Args1, typename... Args2, std::size_t... Indexes1, std::size_t... Indexes2>
            auto rebind(
                Func&& func,
                std::tuple<Args1...>&& args1,
                std::tuple<Args2...>&& args2,
                std::index_sequence<Indexes1...>,
                std::index_sequence<Indexes2...>)
            {
                using BindType = typename internal::BindHelper<Func, Args1..., Args2...>::Type;
                return BindType(
                    std::forward<Func>(func), std::forward<Args1>(std::get<Indexes1>(args1))...,
                    std::forward<Args2>(std::get<Indexes2>(args2))...);
            }

            Functor _functor;
            std::tuple<BoundArgs...> _boundArgs;
        };
    } // namespace internal

    template <typename Func, typename... Args>
    inline typename internal::BindHelper<Func, Args...>::Type bind(Func&& func, Args&&... args)
    {
        using BindType = typename internal::BindHelper<Func, Args...>::Type;
        return BindType(std::forward<Func>(func), std::forward<Args>(args)...);
    }
} // namespace vanilo::core::binder

#endif // INC_532A9E9148F343228FB5256DE9C99443