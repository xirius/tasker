#ifndef INC_532A9E9148F343228FB5256DE9C99443
#define INC_532A9E9148F343228FB5256DE9C99443

#include <vanilo/core/Traits.h>
#include <vanilo/core/Utility.h>

namespace vanilo::core::binder {
    namespace internal {

        /**
         * Maps an argument to bind() into an actual argument to the bound function object.
         */
        template <typename Arg, bool = false>
        class BindUnwrapper;

        /**
         * If the argument is std::reference_wrapper<Arg>, returns the underlying reference.
         */
        template <typename Arg>
        class BindUnwrapper<std::reference_wrapper<Arg>, false>
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
        class BindUnwrapper<Arg, false>
        {
          public:
            template <typename CvArg>
            CvArg&& operator()(CvArg&& arg) const volatile
            {
                return std::forward<CvArg>(arg);
            }
        };

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
            using UnwrapperType = decltype(BindUnwrapper<typename std::remove_cv<Arg>::type>()(std::declval<Arg&>()));

            template <typename Func, typename... Args>
            using ResultType = typename std::result_of<Func&(UnwrapperType<Args>&&...)>::type;

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
                constexpr auto isMemberFn = traits::FunctionTraits<Functor>::IsMemberFnPtr;
                constexpr auto argsCount  = std::tuple_size_v<std::tuple<BoundArgs...>>;
                using NewIndices          = std::make_index_sequence<argsCount - isMemberFn>;
                using SelectedIndices     = typename OffsetSequence<isMemberFn, std::make_index_sequence<argsCount - isMemberFn>>::Type;

                return this->rebind(
                    std::forward<Func>(func), std::forward_as_tuple(std::forward<Args>(args)...),
                    TupleHelper::select(std::move(_boundArgs), SelectedIndices{}), IndexSequence<Args...>{}, NewIndices{});
            }

            template <typename Result = ResultType<Functor, BoundArgs...>>
            Result operator()()
            {
                return this->call<Result>(IndexSequence<BoundArgs...>{});
            }

          private:
            template <typename Result, std::size_t... Indexes>
            Result call(std::index_sequence<Indexes...>)
            {
                return std::__invoke(_functor, BindUnwrapper<BoundArgs>()(std::get<Indexes>(_boundArgs))...);
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