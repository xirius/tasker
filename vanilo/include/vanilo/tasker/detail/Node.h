#ifndef VANILO_TASKER_DETAIL_NODE_H
#define VANILO_TASKER_DETAIL_NODE_H

#include <vanilo/concurrent/Cancellation.h>
#include <vanilo/tasker/detail/Chain.h>
#include <vanilo/tasker/detail/Invoke.h>
#include <vanilo/tasker/detail/Job.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace vanilo::tasker::detail {

    class ExecutorCore;
    class Node;

    /// Handler
    /// ============================================================================================

    /**
     * An onError or recover handler, attached to the node it follows.
     */
    class VANILO_EXPORT Handler
    {
      public:
        /**
         * @param core The executor the handler runs on; null for the executor of its node.
         */
        explicit Handler(std::shared_ptr<ExecutorCore> core) noexcept;
        virtual ~Handler();

        Handler(const Handler&) = delete;
        Handler& operator=(const Handler&) = delete;

        [[nodiscard]] ExecutorCore* core() const noexcept
        {
            return _core.get();
        }

        /**
         * @return True if the handler is called for the error (the type it catches matches).
         */
        [[nodiscard]] virtual bool accepts(const std::exception_ptr& error) const noexcept = 0;

        /**
         * @return True for a recover handler, false for an onError handler.
         */
        [[nodiscard]] virtual bool recovers() const noexcept = 0;

        /**
         * onError: calls the handler.
         */
        virtual void observe(const std::exception_ptr& error, const std::stop_token& token);

        /**
         * recover: calls the handler and resumes the chain after owner (the node holding this handler at index)
         * with the replacement value; if the handler throws, that error is delivered from the next handler on.
         * delivering is the executor whose thread runs this.
         */
        virtual void resume(
            std::unique_ptr<Node> owner,
            std::size_t index,
            std::shared_ptr<ChainControl> chain,
            const std::exception_ptr& error,
            ExecutorCore* delivering) noexcept;

        /**
         * A copy, for each run of a periodic task.
         */
        [[nodiscard]] virtual std::unique_ptr<Handler> clone() const = 0;

      protected:
        [[nodiscard]] const std::shared_ptr<ExecutorCore>& coreReference() const noexcept
        {
            return _core;
        }

      private:
        std::shared_ptr<ExecutorCore> _core;
    };

    /// Node
    /// ============================================================================================

    /**
     * One task of a chain. A node owns the rest of the chain (next), runs on the executor it holds the core of,
     * and holds the chain's control while it runs or waits to run.
     *
     * When a node fails, or is canceled, the rest of the chain doesn't run and the error is delivered from that
     * node on: the nearest onError handler that accepts it is called, and the first recover handler that accepts
     * it resumes the chain with its replacement value. If no recover handler does, the error ends the chain.
     */
    class VANILO_EXPORT Node
    {
      public:
        explicit Node(std::shared_ptr<ExecutorCore> core) noexcept;
        virtual ~Node();

        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;

        /**
         * Runs the node (it was queued on its executor) and passes its result on, or delivers its failure.
         */
        static void run(std::unique_ptr<Node> self) noexcept;

        /**
         * Delivers the cancellation of a node that will never run (its executor shut down).
         */
        static void cancel(std::unique_ptr<Node> self) noexcept;

        /**
         * Starts a chain whose first node is self: queues it on its executor.
         */
        virtual void start(std::unique_ptr<Node> self, std::shared_ptr<ChainControl> chain) noexcept;

        /**
         * Passes a node's result to the next node and queues it, or completes the chain if there is none.
         * delivering is the executor whose thread passes it on.
         */
        template <typename R>
        static void forward(std::unique_ptr<Node> next, std::shared_ptr<ChainControl> chain, R* value, ExecutorCore* delivering) noexcept;

        /**
         * Like forward(), for a node that returns nothing.
         */
        static void forward(std::unique_ptr<Node> next, std::shared_ptr<ChainControl> chain, ExecutorCore* delivering) noexcept;

        /**
         * Delivers an error from node on, starting with its handler at firstHandler (see the class documentation).
         * handled tells whether an onError handler already received the error.
         */
        static void fail(
            std::unique_ptr<Node> node,
            std::size_t firstHandler,
            std::shared_ptr<ChainControl> chain,
            const std::exception_ptr& error,
            ExecutorCore* delivering,
            bool handled) noexcept;

        /**
         * Queues the node on its executor, or delivers its cancellation if the executor doesn't take it.
         */
        static void enqueue(std::unique_ptr<Node> node) noexcept;

        /**
         * Starts a chain whose first node is self, like start(), but drops it without delivering anything if its
         * executor doesn't take it (for the runs of a periodic task, which end the series instead).
         * @return False if the chain was dropped.
         */
        static bool tryStart(std::unique_ptr<Node> self, std::shared_ptr<ChainControl> chain) noexcept;

        [[nodiscard]] ExecutorCore* core() const noexcept
        {
            return _core.get();
        }

        void append(std::unique_ptr<Node> next) noexcept
        {
            _next = std::move(next);
        }

        [[nodiscard]] std::unique_ptr<Node> takeNext() noexcept
        {
            return std::move(_next);
        }

        void addHandler(std::unique_ptr<Handler> handler);

        [[nodiscard]] Handler& handler(std::size_t index) const noexcept;

        /**
         * A copy of this node and the nodes after it, with their handlers, for each run of a periodic task.
         */
        [[nodiscard]] std::unique_ptr<Node> cloneChain() const;

      protected:
        /**
         * Runs the task and passes its result on (see forwardResult). Throws what the task throws.
         */
        virtual void execute() = 0;

        /**
         * A copy of this node without the rest of the chain (handlers are copied by cloneChain).
         */
        [[nodiscard]] virtual std::unique_ptr<Node> clone() const = 0;

        [[nodiscard]] const std::shared_ptr<ExecutorCore>& coreReference() const noexcept
        {
            return _core;
        }

        [[nodiscard]] ChainControl& chain() const noexcept
        {
            return *_chain;
        }

        template <typename R>
        void forwardResult(R* value) noexcept
        {
            forward(std::move(_next), std::move(_chain), value, _core.get());
        }

        void forwardResult() noexcept
        {
            forward(std::move(_next), std::move(_chain), _core.get());
        }

      private:
        static void handOff(std::unique_ptr<Node> next, std::shared_ptr<ChainControl> chain, ExecutorCore* delivering) noexcept;

        std::unique_ptr<Node> _next;
        std::shared_ptr<ExecutorCore> _core;
        std::shared_ptr<ChainControl> _chain;
        std::vector<std::unique_ptr<Handler>> _handlers;
    };

    /**
     * A node that receives the previous node's result.
     */
    template <typename Input>
    class InputNode: public Node
    {
      public:
        using Node::Node;

        void setInput(Input&& value)
        {
            _input.emplace(std::move(value));
        }

      protected:
        [[nodiscard]] Input* input() noexcept
        {
            return &*_input;
        }

      private:
        std::optional<Input> _input;
    };

    template <>
    class InputNode<void>: public Node
    {
      public:
        using Node::Node;

      protected:
        [[nodiscard]] static std::nullptr_t input() noexcept
        {
            return nullptr;
        }
    };

    template <typename R>
    void Node::forward(std::unique_ptr<Node> next, std::shared_ptr<ChainControl> chain, R* value, ExecutorCore* delivering) noexcept
    {
        if (!next) {
            chain->complete(value);
            return;
        }

        try {
            static_cast<InputNode<R>&>(*next).setInput(std::move(*value));
        }
        catch (...) {
            fail(std::move(next), 0, std::move(chain), std::current_exception(), delivering, false);
            return;
        }

        handOff(std::move(next), std::move(chain), delivering);
    }

    /// Job of a node
    /// ============================================================================================

    struct NodeJob
    {
        std::unique_ptr<Node> node;

        void run() noexcept
        {
            Node::run(std::move(node));
        }

        void cancel() noexcept
        {
            Node::cancel(std::move(node));
        }
    };

    /// TaskNode
    /// ============================================================================================

    struct CloneTag
    {
    };

    /**
     * A node calling the function F with the bound arguments Bound (a std::tuple) and its Input. Copyable nodes,
     * for periodic tasks, can be cloned.
     */
    template <typename Input, typename F, typename Bound, bool Copyable>
    class TaskNode final: public InputNode<Input>
    {
        using Call = Invocation<F, Bound, Input>;

      public:
        using Result = typename Call::Result;

        template <typename Function, typename... Arguments>
        explicit TaskNode(std::shared_ptr<ExecutorCore> core, Function&& function, Arguments&&... arguments)
            : InputNode<Input>{std::move(core)}, _function(std::forward<Function>(function)), _bound(std::forward<Arguments>(arguments)...)
        {
        }

        TaskNode(CloneTag, const TaskNode& other): InputNode<Input>{other.coreReference()}, _function(other._function), _bound(other._bound)
        {
        }

      protected:
        void execute() override
        {
            std::stop_token token;
            if constexpr (Call::TakesToken) {
                token = this->chain().token();
            }

            if constexpr (std::is_void_v<Result>) {
                Call::call(_function, _bound, token, this->input());
                this->forwardResult();
            }
            else {
                Result value = Call::call(_function, _bound, token, this->input());
                this->forwardResult(&value);
            }
        }

        [[nodiscard]] std::unique_ptr<Node> clone() const override
        {
            if constexpr (Copyable) {
                return std::make_unique<TaskNode>(CloneTag{}, *this);
            }
            else {
                std::terminate(); // only chains of periodic tasks are cloned, and their nodes are copyable
            }
        }

      private:
        F _function;
        Bound _bound;
    };

    /// Handlers
    /// ============================================================================================

    template <typename F>
    bool handlerAccepts(const std::exception_ptr& error) noexcept
    {
        using Traits = HandlerTraits<F>;

        if (error == canceledException()) { // no need to rethrow it
            return Traits::TakesPointer || std::is_base_of_v<typename Traits::Exception, concurrent::OperationCanceledException>;
        }
        return Traits::accepts(error);
    }

    /**
     * An onError handler: called with the error, the chain still fails.
     */
    template <typename F, bool Copyable>
    class ObserverHandler final: public Handler
    {
        using Traits = HandlerTraits<F>;

      public:
        template <typename Function>
        ObserverHandler(std::shared_ptr<ExecutorCore> core, Function&& function)
            : Handler{std::move(core)}, _function(std::forward<Function>(function))
        {
        }

        [[nodiscard]] bool accepts(const std::exception_ptr& error) const noexcept override
        {
            return handlerAccepts<F>(error);
        }

        [[nodiscard]] bool recovers() const noexcept override
        {
            return false;
        }

        void observe(const std::exception_ptr& error, const std::stop_token& token) override
        {
            Traits::call(_function, error, token);
        }

        [[nodiscard]] std::unique_ptr<Handler> clone() const override
        {
            if constexpr (Copyable) {
                return std::make_unique<ObserverHandler>(coreReference(), _function);
            }
            else {
                std::terminate(); // only chains of periodic tasks are cloned
            }
        }

      private:
        F _function;
    };

    /**
     * A recover handler: its result replaces the result R of the node it follows, and the chain goes on.
     */
    template <typename R, typename F, bool Copyable>
    class RecoveryHandler final: public Handler
    {
        using Traits = HandlerTraits<F>;

      public:
        template <typename Function>
        RecoveryHandler(std::shared_ptr<ExecutorCore> core, Function&& function)
            : Handler{std::move(core)}, _function(std::forward<Function>(function))
        {
        }

        [[nodiscard]] bool accepts(const std::exception_ptr& error) const noexcept override
        {
            return handlerAccepts<F>(error);
        }

        [[nodiscard]] bool recovers() const noexcept override
        {
            return true;
        }

        void resume(
            std::unique_ptr<Node> owner,
            const std::size_t index,
            std::shared_ptr<ChainControl> chain,
            const std::exception_ptr& error,
            ExecutorCore* delivering) noexcept override
        {
            try {
                if constexpr (std::is_void_v<R>) {
                    Traits::call(_function, error, chain->token());
                    Node::forward(owner->takeNext(), std::move(chain), delivering);
                }
                else {
                    R value = Traits::call(_function, error, chain->token());
                    Node::forward(owner->takeNext(), std::move(chain), &value, delivering);
                }
            }
            catch (...) {
                Node::fail(std::move(owner), index + 1, std::move(chain), std::current_exception(), delivering, false);
            }
        }

        [[nodiscard]] std::unique_ptr<Handler> clone() const override
        {
            if constexpr (Copyable) {
                return std::make_unique<RecoveryHandler>(coreReference(), _function);
            }
            else {
                std::terminate(); // only chains of periodic tasks are cloned
            }
        }

      private:
        F _function;
    };

} // namespace vanilo::tasker::detail

#endif // VANILO_TASKER_DETAIL_NODE_H
