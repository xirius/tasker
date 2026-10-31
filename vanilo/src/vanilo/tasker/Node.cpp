#include <vanilo/tasker/ExecutorCore.h>
#include <vanilo/tasker/detail/Node.h>

using namespace vanilo::tasker::detail;

namespace {

    /**
     * Runs the job on the target executor, or inline when the target is the executor delivering it (or none),
     * or no longer takes jobs.
     */
    void runOn(ExecutorCore* target, ExecutorCore* delivering, Job job) noexcept
    {
        if (target && target != delivering) {
            try {
                if (target->tryEnqueue(job)) {
                    return;
                }
            }
            catch (...) {
                reportUnhandledException(std::current_exception());
            }
        }
        runJob(job);
    }

    /**
     * Calls an onError handler. If its executor shuts down before running it, it runs inline.
     */
    struct ObserverJob
    {
        std::unique_ptr<Handler> handler;
        std::exception_ptr error;
        std::stop_token token;

        void run() noexcept
        {
            try {
                handler->observe(error, token);
            }
            catch (...) {
                reportUnhandledException(std::current_exception());
            }
        }

        void cancel() noexcept
        {
            run();
        }
    };

    /**
     * Calls a recover handler, which resumes the chain. If its executor shuts down before running it, it runs
     * inline.
     */
    struct RecoveryJob
    {
        std::unique_ptr<Node> owner;
        std::size_t index;
        std::shared_ptr<ChainControl> chain;
        std::exception_ptr error;
        ExecutorCore* where;

        void run() noexcept
        {
            Handler& handler = owner->handler(index);
            handler.resume(std::move(owner), index, std::move(chain), error, where);
        }

        void cancel() noexcept
        {
            run();
        }
    };

} // namespace

/// Handler
/// ================================================================================================

Handler::Handler(std::shared_ptr<ExecutorCore> core) noexcept: _core{std::move(core)}
{
}

Handler::~Handler() = default;

void Handler::observe(const std::exception_ptr& /*error*/, const std::stop_token& /*token*/)
{
    // Only onError handlers are observed
}

void Handler::resume(
    std::unique_ptr<Node> owner,
    std::size_t index,
    std::shared_ptr<ChainControl> chain,
    const std::exception_ptr& error,
    ExecutorCore* delivering) noexcept
{
    // Only recover handlers resume a chain; any other handler passes the error on.
    Node::fail(std::move(owner), index + 1, std::move(chain), error, delivering, false);
}

/// Node
/// ================================================================================================

Node::Node(std::shared_ptr<ExecutorCore> core) noexcept: _core{std::move(core)}
{
}

Node::~Node() = default;

void Node::run(std::unique_ptr<Node> self) noexcept
{
    ExecutorCore* core = self->_core.get();

    if (self->_chain->stopRequested()) {
        auto chain = std::move(self->_chain);
        fail(std::move(self), 0, std::move(chain), canceledException(), core, false);
        return;
    }

    try {
        self->execute(); // passes the result on: the node doesn't hold the chain afterwards
    }
    catch (...) {
        auto chain = std::move(self->_chain);
        fail(std::move(self), 0, std::move(chain), std::current_exception(), core, false);
    }
}

void Node::cancel(std::unique_ptr<Node> self) noexcept
{
    ExecutorCore* core = self->_core.get();
    auto chain = std::move(self->_chain);
    fail(std::move(self), 0, std::move(chain), canceledException(), core, false);
}

void Node::start(std::unique_ptr<Node> self, std::shared_ptr<ChainControl> chain) noexcept
{
    if (chain->stopRequested()) {
        fail(std::move(self), 0, std::move(chain), canceledException(), nullptr, false);
        return;
    }

    self->_chain = std::move(chain);
    enqueue(std::move(self));
}

void Node::forward(std::unique_ptr<Node> next, std::shared_ptr<ChainControl> chain, ExecutorCore* delivering) noexcept
{
    if (!next) {
        chain->complete(nullptr);
        return;
    }

    handOff(std::move(next), std::move(chain), delivering);
}

void Node::handOff(std::unique_ptr<Node> next, std::shared_ptr<ChainControl> chain, ExecutorCore* delivering) noexcept
{
    if (chain->stopRequested()) {
        fail(std::move(next), 0, std::move(chain), canceledException(), delivering, false);
        return;
    }

    next->_chain = std::move(chain);
    enqueue(std::move(next));
}

void Node::enqueue(std::unique_ptr<Node> node) noexcept
{
    ExecutorCore* core = node->_core.get();
    Job job = Job::make<NodeJob>(std::move(node));

    if (core) {
        try {
            if (core->tryEnqueue(job)) {
                return;
            }
        }
        catch (...) {
            reportUnhandledException(std::current_exception()); // e.g. out of memory: the task is canceled
        }
    }

    // No executor, or it no longer takes tasks: the chain learns that the task was canceled.
    job.cancel();
}

bool Node::tryStart(std::unique_ptr<Node> self, std::shared_ptr<ChainControl> chain) noexcept
{
    ExecutorCore* core = self->_core.get();
    self->_chain = std::move(chain);
    Job job = Job::make<NodeJob>(std::move(self));

    if (core) {
        try {
            if (core->tryEnqueue(job)) {
                return true;
            }
        }
        catch (...) {
            reportUnhandledException(std::current_exception());
        }
    }
    return false; // the job is destroyed with the node: nothing is delivered
}

void Node::fail(
    std::unique_ptr<Node> node,
    std::size_t firstHandler,
    std::shared_ptr<ChainControl> chain,
    const std::exception_ptr& error,
    ExecutorCore* delivering,
    bool handled) noexcept
{
    while (node) {
        auto& handlers = node->_handlers;

        for (auto index = firstHandler; index < handlers.size(); ++index) {
            auto& handler = handlers[index];

            if (!handler || !handler->accepts(error)) {
                continue;
            }

            ExecutorCore* target = handler->core() ? handler->core() : node->core();

            if (handler->recovers()) {
                // The recovery resumes the chain after this node, so it takes the node (and the rest) with it.
                auto job = Job::make<RecoveryJob>(std::move(node), index, std::move(chain), error, target);
                runOn(target, delivering, std::move(job));
                return;
            }

            if (!handled) {
                handled = true;
                runOn(target, delivering, Job::make<ObserverJob>(std::move(handler), error, chain->token()));
            }
        }

        node = std::move(node->_next);
        firstHandler = 0;
    }

    chain->fail(error, handled);
}

void Node::addHandler(std::unique_ptr<Handler> handler)
{
    _handlers.push_back(std::move(handler));
}

Handler& Node::handler(const std::size_t index) const noexcept
{
    return *_handlers[index];
}

std::unique_ptr<Node> Node::cloneChain() const
{
    auto head = clone();
    Node* last = head.get();

    for (const Node* node = this; node != nullptr; node = node->_next.get()) {
        if (node != this) {
            last->_next = node->clone();
            last = last->_next.get();
        }
        for (const auto& handler : node->_handlers) {
            last->_handlers.push_back(handler->clone());
        }
    }
    return head;
}
