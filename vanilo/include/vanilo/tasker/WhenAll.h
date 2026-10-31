#ifndef VANILO_TASKER_WHEN_ALL_H
#define VANILO_TASKER_WHEN_ALL_H

#include <vanilo/tasker/TaskBuilder.h>

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vanilo::tasker {

    namespace detail {

        template <typename R>
        using StoredResult = std::conditional_t<std::is_void_v<R>, std::monostate, R>;

        /// Result types
        /// ========================================================================================

        // The positions of the non-void types among R...
        template <std::size_t I, typename Found, typename... R>
        struct NonVoidPositions
        {
            using Type = Found;
        };

        template <std::size_t I, std::size_t... Found, typename First, typename... Rest>
        struct NonVoidPositions<I, std::index_sequence<Found...>, First, Rest...>
            : NonVoidPositions<
                  I + 1,
                  std::conditional_t<std::is_void_v<First>, std::index_sequence<Found...>, std::index_sequence<Found..., I>>,
                  Rest...>
        {
        };

        template <typename Positions, typename... R>
        struct TupleOfPositions;

        template <std::size_t... P, typename... R>
        struct TupleOfPositions<std::index_sequence<P...>, R...>
        {
            using Type = std::tuple<std::tuple_element_t<P, std::tuple<R...>>...>;
        };

        template <typename... R>
        using AllPositions = typename NonVoidPositions<0, std::index_sequence<>, R...>::Type;

        /**
         * The result of whenAll(builders...): a std::tuple of the non-void results, or void if there are none.
         */
        template <typename... R>
        using WhenAllResult = std::
            conditional_t<AllPositions<R...>::size() == 0, void, typename TupleOfPositions<AllPositions<R...>, StoredResult<R>...>::Type>;

        /// The state of a join
        /// ========================================================================================

        /**
         * The state of a whenAll or whenAny: the chains it started, and the rest of its own chain, which goes on
         * once the outcome is known. Policy says what that outcome is and how the results are kept.
         */
        template <typename Policy>
        class JoinState final: public std::enable_shared_from_this<JoinState<Policy>>
        {
          public:
            /**
             * The control of one of the joined chains: its stop source is linked to the join's chain, and its
             * outcome goes to the join.
             */
            class Slot final: public ChainControl
            {
              public:
                Slot(JoinState& join, const std::size_t index, std::stop_token own, std::stop_token outer)
                    : ChainControl{std::move(own), std::move(outer)}, _join{join}, _index{index}
                {
                }

                void complete(void* value) noexcept override
                {
                    _join.completed(_index, value);
                }

                void fail(const std::exception_ptr& error, bool /*handled*/) noexcept override
                {
                    _join.failed(_index, error);
                }

              private:
                JoinState& _join;
                std::size_t _index;
            };

            JoinState(std::unique_ptr<Node> join, std::shared_ptr<ChainControl> chain, const std::size_t count)
                : _join{std::move(join)}, _chain{std::move(chain)}, _policy{count}, _remaining{count}
            {
            }

            /**
             * Starts the joined chains.
             */
            void start(std::vector<ChainParts> parts) noexcept
            {
                const auto outer = _chain->token();
                for (std::size_t index = 0; index < parts.size(); ++index) {
                    _slots.emplace_back(*this, index, std::move(parts[index].token), outer);
                }

                if (parts.empty()) {
                    if constexpr (!Policy::FirstWins) {
                        finish(); // nothing to wait for (whenAny refuses an empty range)
                    }
                    return;
                }

                const auto self = this->shared_from_this();
                for (std::size_t index = 0; index < parts.size(); ++index) {
                    // The chain shares the join's state, through the slot's control.
                    std::shared_ptr<ChainControl> control{self, &_slots[index]};
                    try {
                        startChain(std::move(parts[index]), std::move(control));
                    }
                    catch (...) {
                        failed(index, std::current_exception());
                    }
                }
            }

          private:
            void completed(const std::size_t index, void* value) noexcept
            {
                if constexpr (Policy::FirstWins) {
                    if (!_decided.exchange(true, std::memory_order_acq_rel)) {
                        stopOthers();
                        try {
                            auto result = _policy.winner(index, value);
                            Node::forward(_join->takeNext(), std::move(_chain), &result, nullptr);
                        }
                        catch (...) {
                            Node::fail(std::move(_join), 0, std::move(_chain), std::current_exception(), nullptr, false);
                        }
                    }
                    finished();
                }
                else {
                    try {
                        _policy.store(index, value);
                    }
                    catch (...) {
                        failed(index, std::current_exception());
                        return;
                    }
                    if (finished()) {
                        finish();
                    }
                }
            }

            void failed(const std::size_t index, const std::exception_ptr& error) noexcept
            {
                if constexpr (Policy::FirstWins) {
                    // Only fails once every chain has failed, with the first error.
                    if (!_firstErrorSet.exchange(true, std::memory_order_acq_rel)) {
                        _firstError = error;
                    }
                    if (finished() && !_decided.exchange(true, std::memory_order_acq_rel)) {
                        Node::fail(std::move(_join), 0, std::move(_chain), _firstError, nullptr, false);
                    }
                }
                else {
                    (void)index;
                    // Fails fast: the other chains are stopped.
                    if (!_decided.exchange(true, std::memory_order_acq_rel)) {
                        stopOthers();
                        Node::fail(std::move(_join), 0, std::move(_chain), error, nullptr, false);
                    }
                    finished();
                }
            }

            /**
             * Counts a finished chain.
             * @return True for the last one.
             */
            bool finished() noexcept
            {
                return _remaining.fetch_sub(1, std::memory_order_acq_rel) == 1;
            }

            /**
             * All the chains succeeded (whenAll): pass the results on.
             */
            void finish() noexcept
            {
                if (_decided.exchange(true, std::memory_order_acq_rel)) {
                    return; // a chain failed
                }
                try {
                    if constexpr (std::is_void_v<typename Policy::Result>) {
                        Node::forward(_join->takeNext(), std::move(_chain), nullptr);
                    }
                    else {
                        auto result = _policy.results();
                        Node::forward(_join->takeNext(), std::move(_chain), &result, nullptr);
                    }
                }
                catch (...) {
                    Node::fail(std::move(_join), 0, std::move(_chain), std::current_exception(), nullptr, false);
                }
            }

            void stopOthers() noexcept
            {
                for (auto& slot : _slots) {
                    slot.requestStop();
                }
            }

            std::unique_ptr<Node> _join; // the join's node: its handlers and the rest of the chain
            std::shared_ptr<ChainControl> _chain;
            std::deque<Slot> _slots;
            Policy _policy;
            std::atomic<std::size_t> _remaining;
            std::atomic<bool> _decided{false};
            std::atomic<bool> _firstErrorSet{false};
            std::exception_ptr _firstError;
        };

        /**
         * The first node of a whenAll or whenAny chain. Starting it starts the joined chains.
         */
        template <typename Policy>
        class JoinNode final: public Node
        {
          public:
            explicit JoinNode(std::vector<ChainParts> parts) noexcept: Node{nullptr}, _parts{std::move(parts)}
            {
            }

            void start(std::unique_ptr<Node> self, std::shared_ptr<ChainControl> chain) noexcept override
            {
                if (chain->stopRequested()) {
                    Node::fail(std::move(self), 0, std::move(chain), canceledException(), nullptr, false);
                    return;
                }

                std::shared_ptr<JoinState<Policy>> state;
                auto parts = std::move(_parts);
                try {
                    state = std::make_shared<JoinState<Policy>>(std::move(self), chain, parts.size());
                }
                catch (...) {
                    Node::fail(std::move(self), 0, std::move(chain), std::current_exception(), nullptr, false);
                    return;
                }
                state->start(std::move(parts));
            }

          protected:
            void execute() override
            {
                std::terminate(); // a join is started, never queued
            }

            [[nodiscard]] std::unique_ptr<Node> clone() const override
            {
                std::terminate(); // joins aren't periodic
            }

          private:
            std::vector<ChainParts> _parts;
        };

        /// Policies
        /// ========================================================================================

        template <typename... R>
        class AllOf
        {
            using Stored = std::tuple<std::optional<StoredResult<R>>...>;

          public:
            static constexpr bool FirstWins = false;
            using Result = WhenAllResult<R...>;

            explicit AllOf(std::size_t /*count*/) noexcept
            {
            }

            void store(const std::size_t index, void* value)
            {
                storeAt(index, value, std::index_sequence_for<R...>{});
            }

            Result results()
            {
                return resultsAt(AllPositions<R...>{});
            }

          private:
            template <std::size_t... I>
            void storeAt(const std::size_t index, void* value, std::index_sequence<I...>)
            {
                ((index == I ? (emplace<I>(value), 0) : 0), ...);
            }

            template <std::size_t I>
            void emplace(void* value)
            {
                using T = std::tuple_element_t<I, std::tuple<R...>>;
                if constexpr (std::is_void_v<T>) {
                    std::get<I>(_values).emplace();
                }
                else {
                    std::get<I>(_values).emplace(std::move(*static_cast<T*>(value)));
                }
            }

            template <std::size_t... P>
            Result resultsAt(std::index_sequence<P...>)
            {
                return Result{std::move(*std::get<P>(_values))...};
            }

            Stored _values;
        };

        template <typename R>
        class AllOfRange
        {
          public:
            static constexpr bool FirstWins = false;
            using Result = std::conditional_t<std::is_void_v<R>, void, std::vector<StoredResult<R>>>;

            explicit AllOfRange(const std::size_t count): _values(count)
            {
            }

            void store(const std::size_t index, void* value)
            {
                if constexpr (std::is_void_v<R>) {
                    _values[index].emplace();
                }
                else {
                    _values[index].emplace(std::move(*static_cast<R*>(value)));
                }
            }

            Result results()
            {
                if constexpr (!std::is_void_v<R>) {
                    Result results;
                    results.reserve(_values.size());
                    for (auto& value : _values) {
                        results.push_back(std::move(*value));
                    }
                    return results;
                }
            }

          private:
            std::vector<std::optional<StoredResult<R>>> _values;
        };

        template <typename... R>
        class AnyOf
        {
          public:
            static constexpr bool FirstWins = true;
            using Result = std::variant<StoredResult<R>...>;

            explicit AnyOf(std::size_t /*count*/) noexcept
            {
            }

            Result winner(const std::size_t index, void* value)
            {
                return winnerAt(index, value, std::index_sequence_for<R...>{});
            }

          private:
            template <std::size_t... I>
            Result winnerAt(const std::size_t index, void* value, std::index_sequence<I...>)
            {
                std::optional<Result> result;
                ((index == I ? (makeAt<I>(result, value), 0) : 0), ...);
                return std::move(*result);
            }

            template <std::size_t I>
            static void makeAt(std::optional<Result>& result, void* value)
            {
                using T = std::tuple_element_t<I, std::tuple<R...>>;
                if constexpr (std::is_void_v<T>) {
                    result.emplace(std::in_place_index<I>);
                }
                else {
                    result.emplace(std::in_place_index<I>, std::move(*static_cast<T*>(value)));
                }
            }
        };

        template <typename R>
        class AnyOfRange
        {
          public:
            static constexpr bool FirstWins = true;
            using Result = std::conditional_t<std::is_void_v<R>, std::size_t, std::pair<std::size_t, StoredResult<R>>>;

            explicit AnyOfRange(std::size_t /*count*/) noexcept
            {
            }

            Result winner(const std::size_t index, void* value)
            {
                if constexpr (std::is_void_v<R>) {
                    return index;
                }
                else {
                    return Result{index, std::move(*static_cast<R*>(value))};
                }
            }
        };

        template <typename Policy>
        TaskBuilder<typename Policy::Result> join(std::vector<ChainParts> parts)
        {
            ChainParts joined;
            joined.append(std::make_unique<JoinNode<Policy>>(std::move(parts)));
            return TaskBuilder<typename Policy::Result>{std::move(joined)};
        }

        template <typename R>
        std::vector<ChainParts> takeParts(std::vector<TaskBuilder<R>>& builders)
        {
            std::vector<ChainParts> parts;
            parts.reserve(builders.size());
            for (auto& builder : builders) {
                partsOf(builder).checkUsable();
                parts.push_back(std::move(partsOf(builder)));
            }
            return parts;
        }

        template <typename... R>
        std::vector<ChainParts> takeParts(TaskBuilder<R>&... builders)
        {
            (partsOf(builders).checkUsable(), ...);
            std::vector<ChainParts> parts;
            parts.reserve(sizeof...(R));
            (parts.push_back(std::move(partsOf(builders))), ...);
            return parts;
        }

    } // namespace detail

    /// whenAll and whenAny
    /// ============================================================================================

    /**
     * Starts a chain that runs the given (unsubmitted) chains at the same time, each on its executors, and goes on
     * with all their results: a std::tuple of the non-void results (void if all are void), which a then() function
     * can also take as separate parameters. If one fails, the others are stopped and the join fails with its error
     * (a handler right after the join runs where that error came from, unless it is given an executor).
     */
    template <typename... R>
    requires(sizeof...(R) > 0)
    TaskBuilder<detail::WhenAllResult<R...>> whenAll(TaskBuilder<R>&&... builders)
    {
        return detail::join<detail::AllOf<R...>>(detail::takeParts(builders...));
    }

    /**
     * Like whenAll(builders...), for any number of chains with the same result: goes on with a std::vector of
     * their results, in the order of the builders (void if R is void).
     */
    template <typename R>
    TaskBuilder<typename detail::AllOfRange<R>::Result> whenAll(std::vector<TaskBuilder<R>> builders)
    {
        return detail::join<detail::AllOfRange<R>>(detail::takeParts(builders));
    }

    /**
     * Starts a chain that runs the given chains at the same time and goes on with the result of the first one that
     * succeeds, as a std::variant whose index tells which one (std::monostate for a void result). The others are
     * stopped. If they all fail, the join fails with the first error.
     */
    template <typename... R>
    requires(sizeof...(R) > 0)
    TaskBuilder<std::variant<detail::StoredResult<R>...>> whenAny(TaskBuilder<R>&&... builders)
    {
        return detail::join<detail::AnyOf<R...>>(detail::takeParts(builders...));
    }

    /**
     * Like whenAny(builders...), for any number of chains with the same result: goes on with the index of the
     * first chain that succeeded and its result (only the index if R is void).
     * @throws std::invalid_argument if there are no builders.
     */
    template <typename R>
    TaskBuilder<typename detail::AnyOfRange<R>::Result> whenAny(std::vector<TaskBuilder<R>> builders)
    {
        if (builders.empty()) {
            throw std::invalid_argument{"whenAny needs at least one task."};
        }
        return detail::join<detail::AnyOfRange<R>>(detail::takeParts(builders));
    }

} // namespace vanilo::tasker

#endif // VANILO_TASKER_WHEN_ALL_H
