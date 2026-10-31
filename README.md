# Vanilo

A C++20 library for running work on executors: a work-stealing thread pool, or a thread you drive yourself.
Tasks are chained into pipelines that pass results along, run on any executor, recover from errors, can be
delayed, repeated, joined and canceled, and report their outcome through a handle.

```cpp
#include <vanilo/tasker/Tasker.h>

using namespace vanilo::tasker;

auto pool = ThreadPoolExecutor::create();

auto answer = Task::run(pool.get(), [] { return 6; })
                  .then(pool.get(), [](int value) { return value * 7; })
                  .submit();

answer.get(); // 42
```

What it gives you:

- A thread pool with one queue per thread and work stealing, and a `ManualExecutor` that runs tasks on a thread of
  your own, like an event loop.
- Chains of tasks (`then`, `onError`, `recover`), each step on the executor you choose, with a `TaskHandle` to wait
  for the result or cancel.
- Cooperative cancellation with the standard `std::stop_token`.
- Delayed and periodic tasks: at a fixed rate, at a fixed rate that skips while a run is unfinished, or with a fixed
  delay.
- `whenAll` and `whenAny` to run chains together.
- Safe shutdown: destroying an executor cancels its tasks, and a chain running elsewhere is told instead of
  touching a dead executor.
- Errors nobody handled go to a handler you can replace, rather than disappearing.

The current release is `v1.0-rc2`, the release candidate of 1.0. The repository is called `tasker`; the library, its
CMake package and its target are called Vanilo.

- [Getting it](#getting-it)
- [Executors](#executors)
- [Running tasks](#running-tasks)
- [Results and chains](#results-and-chains)
- [Errors](#errors)
- [Cancellation](#cancellation)
- [Delayed and periodic tasks](#delayed-and-periodic-tasks)
- [Running chains together](#running-chains-together)
- [Task objects](#task-objects)
- [Shutting down](#shutting-down)
- [Rules worth knowing](#rules-worth-knowing)
- [Utilities](#utilities)
- [Building and testing](#building-and-testing)
- [License](#license)

## Getting it

You need CMake 3.25 or newer and a C++20 compiler. The whole test suite passes on x86-64 Linux with:

| Compiler | Tested | Doesn't build |
|---|---|---|
| GCC | 11, 12, 13, 14, 15 | 10 |
| Clang with libstdc++ | 16, 17, 18, 19, 23 | 15 |
| Clang with libc++ | 18, 19, with `-fexperimental-library` (for `std::stop_token`) | 18, 19 without that flag |

Only Linux is tested. The CMake target links `atomic` unconditionally, so Windows and macOS need a change there
first.

With `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(tasker
    GIT_REPOSITORY https://github.com/xirius/tasker.git
    GIT_TAG v1.0-rc2
    FIND_PACKAGE_ARGS NAMES Vanilo CONFIG)
FetchContent_MakeAvailable(tasker)

target_link_libraries(my_app PRIVATE Vanilo::Vanilo)
```

The tests, examples, install rules and strict warnings are off when the library is part of another project. It
follows your `BUILD_SHARED_LIBS`, so it is static unless you set that. To keep it static when you do, set it locally
(the library is always position-independent, so it can go into a shared library of yours):

```cmake
block()
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(tasker)
endblock()
```

An installed copy (`cmake --install build/release --prefix <prefix>`) is found with `find_package(Vanilo CONFIG)` and
has the same `Vanilo::Vanilo` target.

The API is in these headers:

| Include | Gives you | Namespace |
|---|---|---|
| `<vanilo/tasker/Tasker.h>` | `Task`, `TaskBuilder`, `TaskHandle`, the executors, `FixedRate`, `Overrun`, `FixedDelay`, `whenAll`, `whenAny`, `setUnhandledExceptionHandler` | `vanilo::tasker` |
| | `OperationCanceledException`, `throwIfStopRequested` | `vanilo::concurrent` |
| `<vanilo/concurrent/ShutdownGate.h>` | [`ShutdownGate`](#shutdowngate) | `vanilo::concurrent` |
| `<vanilo/core/Tracer.h>` | [`VANILO_TRACE`, `VANILO_ASSERT`, `VANILO_VERIFY`](#debug-macros) | macros |

The examples below assume these lines, and a `pool` and a `loop` created as in [Executors](#executors):

```cpp
#include <vanilo/tasker/Tasker.h>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
```

## Executors

Every task runs on an executor. There are two:

| Executor | Runs tasks on | Use it for |
|---|---|---|
| `ThreadPoolExecutor::create(n)` | `n` worker threads (default: one per hardware thread) | parallel or blocking work |
| `ManualExecutor::create()` | the thread that calls `runPending()` or `runUntil()` | keeping all your state on one thread, like an event loop |

```cpp
auto pool = ThreadPoolExecutor::create(4);
pool->resize(8).wait(); // grow or shrink later

auto loop = ManualExecutor::create();
loop->runPending(10);   // runs up to 10 queued tasks, returns how many are left
```

A `ManualExecutor` is the way to make other threads hand work to one thread. A typical event loop runs until a stop
is requested:

```cpp
std::stop_source stop;

// From any thread: queue work for the loop thread.
Task::post(loop.get(), [] { /* runs on the loop thread */ });

// On the loop thread: run tasks until stop.request_stop() is called.
loop->runUntil(stop.get_token());
```

`runUntil()` waits for new tasks until a stop is requested. Tasks that are already queued at that point still run
before it returns. `runPending()` doesn't wait: it stops when the queue is empty, and it also runs the tasks that its
tasks queue. Tasks run in the order they were queued.

The pool gives each thread its own queue. Tasks queued from a pool thread (continuations, fan-out) go to its queue,
tasks from other threads are spread over the queues, and idle threads take work from busy ones before they go to
sleep. So separate tasks on a pool have no order; the tasks of one chain always run one after the other.

| Call | Does |
|---|---|
| `executor->pendingCount()` | number of tasks waiting in the queue (either executor) |
| `pool->resize(n)` | changes the number of threads; the returned `std::future<void>` is ready once the removed threads have stopped |
| `pool->threadCount()`, `pool->threadIds()`, `pool->containsThread(id)` | what the pool's threads are |
| `ThreadPoolExecutor::defaultThreadCount()` | number of hardware threads, at least 1 |

A thread removed by `resize()` finishes its current task first, and the other threads take over its queue. A pool
can have no threads at all (`create(0)`, `resize(0)`): its tasks then wait until it grows.

## Running tasks

There are two ways to run something:

- `Task::post(executor, function, arguments...)` queues a single function. It's the cheapest way: no chain, no
  handle, and a small function is stored without allocating. An exception that escapes it is reported to the
  [unhandled-exception handler](#errors).
- `Task::run(executor, function, arguments...)` starts a chain that you extend with `then`, `onError` and `recover`,
  and queue with `submit()`, which returns a [`TaskHandle`](#results-and-chains).

Both have forms with a stop token, and `Task::run` has forms with a delay or an interval:

| Call | Starts |
|---|---|
| `Task::post(executor, f, args...)` | a single function |
| `Task::post(executor, token, f, args...)` | a single function that a stop through `token` skips |
| `Task::run(executor, f, args...)` | a chain |
| `Task::run(executor, token, f, args...)` | a chain that a stop through `token` cancels |
| `Task::run(executor, delay, f, args...)` | a chain whose first task is queued after `delay` |
| `Task::run(executor, token, delay, f, args...)` | a delayed chain that a stop through `token` cancels |
| `Task::run(executor, delay, interval, f, args...)` | a [periodic](#delayed-and-periodic-tasks) chain, first run after `delay` |
| `Task::run(executor, token, delay, interval, f, args...)` | a periodic chain that a stop through `token` ends |

Arguments are copied into the task; use `std::ref` for anything the task should change in place:

```cpp
int counter = 0;
Task::post(loop.get(), [](int& value, int step) { value += step; }, std::ref(counter), 5);
loop->runPending(); // counter == 5
```

Lambdas (generic ones too), free functions, function objects and member functions all work; a one-shot task may
be move-only. For a member function, pass the object (by pointer to use an existing one) before the arguments:

```cpp
struct Thermostat
{
    void setTarget(double celsius) { target = celsius; }
    double target = 0;
};

Thermostat thermostat;
Task::post(loop.get(), &Thermostat::setTarget, &thermostat, 21.5);
```

A chain is only queued by `submit()`. The builder methods are called on the temporary that `Task::run()` returns,
and the compiler warns when the result of a builder is dropped, so a forgotten `submit()` doesn't go unnoticed.
To build a chain in steps, move the builder: `auto chain = Task::run(...); auto handle = std::move(chain).submit();`.
A builder can only be used once; using it again throws `std::logic_error`.

## Results and chains

`then(executor, function)` adds a task that receives the previous result, after its own bound arguments. Each task
can run on a different executor:

```cpp
auto tooWarm = Task::run(pool.get(), [] { return readSensor(); })             // on the pool
                   .then(loop.get(), [](double celsius) { return celsius > 21; }) // on the loop thread
                   .submit();
```

A task is called with, in this order: the object (for a member function), the chain's `std::stop_token` (if it takes
[one](#cancellation)), its bound arguments, and the previous result.

A task returning `void` is followed by a task without that parameter. A task returning a `std::tuple` can be
followed by a function that takes the tuple, or one that takes its elements as separate parameters. Results are
moved from task to task, so they may be move-only; a task that returns a reference passes on a copy.

`submit()` returns a `TaskHandle<T>` for the result of the last task. Like a `std::future`, it is move-only:

| Method | Does |
|---|---|
| `get()` | waits, then returns the result or rethrows the error (once: the handle is empty afterwards) |
| `wait()`, `waitFor(timeout)`, `waitUntil(deadline)` | wait for the chain to finish; the timed ones return false if it hasn't |
| `isReady()` | tells if it has finished |
| `cancel()` | requests the [cancellation](#cancellation) of the chain, without waiting for it |
| `valid()` | tells if the handle refers to a chain |

The handle can be dropped: the chain still runs. On an empty handle (default-constructed, moved from, or after
`get()`), `get()` and the waits throw `std::future_error`, `isReady()` is false and `cancel()` does nothing.

## Errors

When a task throws, the rest of its chain doesn't run, and the error is delivered:

- to the nearest `onError` handler that takes it, from the failing task on to the end of the chain (only that one);
- to the first `recover` handler that takes it: its result replaces the failed result, and the chain goes on;
- otherwise to the handle, whose `get()` rethrows it, even when an `onError` handler ran.

`onError` and `recover` apply to the tasks before them (up to the previous `recover`):

```cpp
Task::run(pool.get(), [] { return download(); })
    .then(pool.get(), [](const std::string& body) { parse(body); })
    .onError([](const std::exception& ex) { log(ex.what()); }) // the errors of both tasks
    .submit();

auto temperature = Task::run(pool.get(), [] { return readSensor(); })
                       .recover([](const std::exception&) { return 20.0; }) // use a default instead
                       .then(loop.get(), [](double celsius) { return celsius; })
                       .submit();
```

A `recover` handler returns what the task before it returns (nothing for a `void` task). If it throws, that error
goes on down the chain.

A handler is called for the exceptions of its parameter type (a generic one takes a `std::exception`), or for any
error if it takes a `std::exception_ptr`. It may take the chain's `std::stop_token` as its second parameter:

```cpp
Task::run(loop.get(), &Device::connect, &device)
    .onError([](const TimeoutError&) { /* only timeouts */ })
    .onError(pool.get(), [&device](const std::exception& ex) { device.markOffline(ex.what()); })
    .submit();
```

A handler runs on the executor given to it, otherwise on the executor of the task before it: inline if the error is
delivered there, or if that executor is shutting down. A handler on another executor is only queued there, so the
handle can be ready before the handler has run.

Errors that nobody handles go to the process-wide unhandled-exception handler:

- an exception that escapes a posted function, a [`Task` object](#task-objects) or an `onError` handler (one that
  escapes a `recover` handler goes on down the chain instead);
- the error of a chain that no `onError` handler received, once its handle is gone without `get()` having been
  called;
- the error of a periodic run without an `onError` handler for it.

It writes them to stderr by default. Replace it to log them your way; it may be called from any thread, including
the one that drops a handle:

```cpp
setUnhandledExceptionHandler([](const std::exception_ptr& exception) {
    try {
        std::rethrow_exception(exception);
    }
    catch (const std::exception& ex) {
        spdlog::error("Task failed: {}", ex.what());
    }
});
```

`setUnhandledExceptionHandler` returns the previous handler. An empty handler ignores these errors, and an exception
that escapes the handler is ignored too. Cancellations (`OperationCanceledException`) are never reported there.

## Cancellation

Cancellation is cooperative and uses the standard `std::stop_source` and `std::stop_token`. Each chain has its own
stop source: `handle.cancel()` requests a stop, and so does a stop of the token given to `Task::run()`. Every task of
the chain checks it before it starts: a canceled task doesn't run, and its chain receives an
`OperationCanceledException`.

```cpp
auto handle = Task::run(loop.get(), [] { /* never runs */ }).submit();
handle.cancel();
```

A task that wants to stop halfway takes the chain's token as its first parameter; it is passed automatically (after
the object, for a member function):

```cpp
std::stop_source source;

Task::run(pool.get(), source.get_token(), [](const std::stop_token& token) {
    for (auto& chunk : chunks) {
        throwIfStopRequested(token); // throws OperationCanceledException
        process(chunk);
    }
}).submit();

source.request_stop(); // from anywhere
```

A task that is already running and doesn't check its token runs to its end. To react to a stop request instead of
checking, register a `std::stop_callback` on the token.

A cancellation is delivered like any other error, which has two consequences. `OperationCanceledException` derives
from `std::runtime_error`, so a `const std::exception&` handler receives it too. And a `recover` handler that takes
it replaces the canceled result: the tasks after the `recover` still don't run, but if there are none, `get()` returns
the recovered value. A handler that takes a more specific type leaves cancellations alone.

`Task::post()` takes a token too: the function doesn't run if a stop was requested by the time it would. A posted
function that takes a token receives that one, or an empty token if none was given.

## Delayed and periodic tasks

Give a delay, or a delay and an interval. The waiting happens on an internal timer thread; the tasks themselves run
on the executors you chose. Delays are for `Task::run`; `Task::post` has none.

```cpp
std::stop_source stop;

Task::run(loop.get(), stop.get_token(), 5s, [] { /* once, in 5 s */ }).submit();

auto ticker = Task::run(loop.get(), 0s, 10s, [] { /* every 10 s */ })
                  .onError([](const std::exception& ex) { log(ex.what()); })
                  .submit();

ticker.cancel();      // ends the series
stop.request_stop();  // cancels the delayed task, at once
```

There are three ways to repeat. They differ in what happens when a run is due and the previous one hasn't finished:

- `FixedRate{10s}` is the default; a plain `10s` means the same. A run is queued every 10 s, whatever the runs before
  it are doing. On a thread pool, runs can overlap if one takes longer than the interval. If the executor can't keep
  up, for example a `ManualExecutor` that doesn't run for a while, the runs wait in its queue and then run one after
  the other.
- `FixedRate{10s, Overrun::Skip}` keeps the same rhythm, but skips a period while the previous run's whole chain
  hasn't finished, so runs never overlap or pile up. A run after a delay can come soon before the next one.
- `FixedDelay{10s}` starts the next run 10 s after the previous run's whole chain has finished, so runs never overlap
  or pile up, and the rhythm moves with how long each run takes.

```cpp
Task::run(pool.get(), 0s, FixedRate{10s, Overrun::Skip}, [] { return poll(); })
    .then(loop.get(), [](Status status) { show(status); })
    .submit();

Task::run(pool.get(), 0s, FixedDelay{10s}, [] { return poll(); })
    .then(loop.get(), [](Status status) { show(status); })
    .submit();
```

This is how each one runs with a first run at 10 s, on a `ManualExecutor` that is stuck from 12 s to 47 s:

| Interval | Runs at (seconds) |
|---|---|
| `FixedRate{10s}` | 10, 47, 47, 47, 50, 60 |
| `FixedRate{10s, Overrun::Skip}` | 10, 47, 50, 60 |
| `FixedDelay{10s}` | 10, 47, 57, 67 |

- With `Overrun::Skip` and `FixedDelay`, a run that never finishes holds up the series. That includes a chain with a
  step queued on an executor that isn't running.
- A fixed rate also skips the periods that the timer thread itself missed.
- The interval must be positive; `Task::run()` throws `std::invalid_argument` otherwise.
- Each run is a fresh copy of the chain, so its functions, handlers and bound arguments must be copyable, and what a
  `mutable` lambda changes is gone by the next run. Keep state that lasts outside the chain. A run whose copy throws
  is skipped and reported.
- A failing run doesn't end the series: its error goes to its handlers, or is reported.
- The handle is a `TaskHandle<void>`, whatever the last task returns. It is ready once the series has ended (canceled,
  or its executor destroyed), and `get()` then throws `OperationCanceledException`.
- When a series is canceled, only a run that was already queued reports the cancellation.

## Running chains together

`whenAll` and `whenAny` start a chain that runs other (unsubmitted, one-shot) chains at the same time. You go on from
there like from any chain: with `then`, `onError`, `recover` and `submit()`.

```cpp
auto report = whenAll(Task::run(pool.get(), [] { return readTemperature(); }),
                      Task::run(pool.get(), [] { return readHumidity(); }))
                  .then(loop.get(), [](double temperature, double humidity) { return format(temperature, humidity); })
                  .submit();

auto fastest = whenAny(Task::run(pool.get(), [] { return askPrimary(); }),
                       Task::run(pool.get(), [] { return askBackup(); }))
                   .submit();
```

- `whenAll` goes on with a `std::tuple` of the non-void results (nothing if they're all void). The first failure
  stops the other chains and fails the join.
- `whenAny` goes on with the first success, as a `std::variant` whose `index()` tells which chain won
  (`std::monostate` for a void result), and stops the others. It fails with the first error only if all fail.
- Both take a `std::vector<TaskBuilder<T>>` too, for any number of chains: `whenAll` then gives a `std::vector<T>` in
  the builders' order (nothing if `T` is void), `whenAny` a `std::pair` of the winner's index and its result (only the
  index if `T` is void).

```cpp
std::vector<TaskBuilder<double>> readings;
for (const auto& sensor : sensors) {
    readings.push_back(Task::run(pool.get(), [&sensor] { return sensor.read(); }));
}

auto all = whenAll(std::move(readings)).submit(); // a TaskHandle<std::vector<double>>
```

An empty vector makes `whenAll` go on at once, and `whenAny` throw `std::invalid_argument`.

The joined chains keep their delays, handlers and recoveries. Canceling the join's handle stops them.

Neither join waits for the chains it stops: they may still be finishing a task when the join goes on, so what they
use must outlive them. A handler right after a join runs on the thread where the error came from, unless you give it
an executor.

## Task objects

Below chains, an executor queues `Task` objects, for when you want your own class rather than a function. `cancel()`
is called instead of `run()` if the executor shuts down first, and an exception that escapes `run()` is reported to
the [unhandled-exception handler](#errors):

```cpp
class FlushTask final: public Task
{
  public:
    void run() override { flush(); }
    void cancel() noexcept override { /* the executor shut down before it ran */ }
};

pool->submit(std::make_unique<FlushTask>());
```

## Shutting down

Destroying an executor doesn't leave chains hanging:

- A pool first lets its threads finish the tasks they are running, and waits for them.
- Tasks that are still queued are canceled: their handles and `onError` handlers receive an
  `OperationCanceledException`, and a `Task` object gets `cancel()`. A posted function is just dropped.
- Tasks handed to it afterwards, such as the continuation of a chain running elsewhere, are canceled the same way: a
  chain holds its executors' shared state, never a dangling executor. A null executor counts as one that has shut
  down.
- A delayed chain or a periodic series is canceled when its delay or next run is due, not when the executor goes.

The executor object itself must outlive the calls made on it: stop the producers (for example network callbacks)
before destroying it. A pool may be destroyed by one of its own tasks; that thread ends after the task.

When the program exits, the timer thread stops and cancels the chains that are still waiting for it.

## Rules worth knowing

- **Where code runs.** A task runs on its executor. A handler runs on its executor (see [Errors](#errors)).
  A `ManualExecutor` only runs anything inside `runPending()` and `runUntil()`.
- **Lifetimes.** Tasks run later, often on another thread, and `cancel()` doesn't wait for them. What a task
  captures by reference must outlive the chain.
- **Waiting inside a pool task** for another pool task works, as long as the pool has another thread to run it.
  Don't wait for a handle on the thread that runs its `ManualExecutor`: the task can't run while that thread waits.
- **Handles aren't shared.** Like a `std::future`, use a handle from one thread at a time.
- **Token parameters.** A function whose first parameter takes a `std::stop_token` receives the chain's token, so a
  variadic generic lambda (`[](auto&&...)`) receives it too.

## Utilities

These two are independent of tasks, and `Tasker.h` doesn't include them.

### ShutdownGate

A small tool to shut a resource down while other threads may be using it. `close()` lets no one in any more, and
returns once everyone inside has left:

```cpp
ShutdownGate gate;

// Users of the resource, on any thread:
if (const auto entry = gate.enter()) {
    use(resource);
}

// The owner:
gate.close();
```

`tryEnter()` and `leave()` do the same without the `Entry` object, and `isClosed()` tells if the gate is closed.

### Debug macros

`<vanilo/core/Tracer.h>` has three macros for debug builds. In release builds (`NDEBUG`), `VANILO_TRACE` and
`VANILO_ASSERT` compile to nothing, and `VANILO_VERIFY` still evaluates its expression but ignores the result.

```cpp
VANILO_TRACE("%d tasks left", count); // to stderr, printf-style, with the file and line
VANILO_ASSERT(count >= 0);
VANILO_VERIFY(closeFile() == 0);
```

## Building and testing

The presets use Ninja and build into `build/<preset>`:

| Preset | What |
|---|---|
| `debug`, `release` | Normal builds |
| `asan` | AddressSanitizer (with leak detection) and UBSan |
| `tsan` | ThreadSanitizer |
| `coverage` | Clang source-based coverage (needs `clang++`, `llvm-profdata` and `llvm-cov`) |
| `benchmark` | Release build of the benchmarks |

```sh
cmake --workflow --preset debug                               # configure, build, test
cmake --workflow --preset tsan                                # tests under ThreadSanitizer
cmake --preset coverage && cmake --build --preset coverage --target tasker-coverage
                                                              # report, HTML in build/coverage/coverage/html
cmake --preset benchmark && cmake --build --preset benchmark
./build/benchmark/bin/Vanilo.benchmarks                       # the benchmarks
./build/debug/bin/tests/Vanilo.tests "[whenAll]"              # one tag
./build/debug/bin/Example01                                   # the example
```

The tests use Catch2 3 and the benchmarks Google Benchmark: an installed copy if there is one, otherwise CMake
downloads them. The library itself only needs threads and `libatomic`.

CMake options ("top level" means ON when this is the main project and OFF when it is part of another one):

| Option | Default | Meaning |
|---|---|---|
| `TASKER_BUILD_TESTS` | top level | Build the unit tests |
| `TASKER_BUILD_EXAMPLES` | top level | Build the examples |
| `TASKER_WARNINGS_AS_ERRORS` | top level | Treat warnings as errors (the library's own targets build with `-Wall -Wextra -Wpedantic`) |
| `TASKER_INSTALL` | top level | Install rules and the `Vanilo` CMake package |
| `TASKER_BUILD_BENCHMARKS` | OFF | Build the benchmarks |
| `TASKER_SANITIZER` | empty | `address` (with UBSan), `thread` or empty |
| `TASKER_COVERAGE` | OFF | Coverage instrumentation (Clang) and the `tasker-coverage` target |
| `BUILD_SHARED_LIBS` | ON at top level | Shared or static library |

Library files built as Debug, RelWithDebInfo and MinSizeRel carry a suffix: `libVanilo.d.so`, `libVanilo.rd.so`,
`libVanilo.mr.so`.

The examples in this README are compiled and run by `vanilo/tests/src/ReadmeExamplesTests.cpp`.

### Project structure

- `vanilo/include/vanilo/` – public headers (`tasker/Tasker.h` includes the tasker API; `tasker/detail/` holds the
  templates behind it)
- `vanilo/private/vanilo/`, `vanilo/src/vanilo/` – implementation
- `vanilo/tests/src/` – Catch2 tests, including regression tests for every fixed bug
- `benchmarks/` – the benchmarks
- `examples/` – a small periodic-task example
- `cmake/VaniloConfig.cmake.in` – template of the installed CMake package

## License

MIT, see [LICENSE](LICENSE).
