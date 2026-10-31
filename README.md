# Vanilo

A C++20 library for running work on executors: a work-stealing thread pool, or a thread you drive yourself.
Tasks are chained into pipelines that pass results along, run on any executor, recover from errors, can be
delayed, repeated, joined and canceled, and report their outcome through a handle. Nothing fails silently.

```cpp
#include <vanilo/tasker/Tasker.h>

using namespace vanilo::tasker;

auto pool = ThreadPoolExecutor::create();

auto answer = Task::run(pool.get(), [] { return 6; })
                  .then(pool.get(), [](int value) { return value * 7; })
                  .submit();

answer.get(); // 42
```

- [Getting it](#getting-it)
- [Executors](#executors)
- [Running tasks](#running-tasks)
- [Results and chains](#results-and-chains)
- [Errors](#errors)
- [Cancellation](#cancellation)
- [Delayed and periodic tasks](#delayed-and-periodic-tasks)
- [Running chains together](#running-chains-together)
- [Shutting down](#shutting-down)
- [Rules worth knowing](#rules-worth-knowing)
- [Performance](#performance)
- [Migrating from 1.0-b4](#migrating-from-10-b4)
- [Building and testing](#building-and-testing)

## Getting it

Requirements: CMake 3.25 or newer, and a C++20 compiler whose standard library has `<stop_token>`, concepts and
atomic waits: GCC 11 or newer, or Clang 16 or newer with libstdc++ 11+ or libc++ 18+, should do (MSVC 2022 too). It
is tested with GCC 15 and Clang 23. The library links `Threads::Threads` and `atomic`.

With `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(tasker
    GIT_REPOSITORY https://github.com/xirius/tasker.git
    GIT_TAG v1.0-rc1
    FIND_PACKAGE_ARGS NAMES Vanilo CONFIG)
FetchContent_MakeAvailable(tasker)

target_link_libraries(my_app PRIVATE Vanilo::Vanilo)
```

The tests, examples and strict warnings are off when the library is part of another project. The consumer decides
`BUILD_SHARED_LIBS`; to link it statically, wrap `FetchContent_MakeAvailable` in a `block()` that sets
`BUILD_SHARED_LIBS OFF` (the library is always position-independent).

An installed copy (`cmake --install build/release --prefix <prefix>`) is found with `find_package(Vanilo CONFIG)`.

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
before it returns.

The pool gives each thread its own queue. Tasks queued from a pool thread (continuations, fan-out) go to its queue,
tasks from other threads are spread over the queues, and idle threads take work from busy ones before they go to
sleep.

## Running tasks

There are two ways to run something:

- `Task::post(executor, function, arguments...)` queues a single function. It's the cheapest way: no chain, no
  handle, and a small function is stored without allocating. An exception that escapes it is reported to the
  [unhandled-exception handler](#errors).
- `Task::run(executor, function, arguments...)` starts a chain that you extend with `then`, `onError` and `recover`,
  and queue with `submit()`, which returns a [`TaskHandle`](#results-and-chains).

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

## Results and chains

`then(executor, function)` adds a task that receives the previous result, after its own bound arguments. Each task
can run on a different executor:

```cpp
auto tooWarm = Task::run(pool.get(), [] { return readSensor(); })             // on the pool
                   .then(loop.get(), [](double celsius) { return celsius > 21; }) // on the loop thread
                   .submit();
```

A task returning `void` is followed by a task without that parameter. A task returning a `std::tuple` can be
followed by a function that takes the tuple, or one that takes its elements as separate parameters.

`submit()` returns a `TaskHandle<T>` for the result of the last task. Like a `std::future`, it is move-only:

| Method | Does |
|---|---|
| `get()` | waits, then returns the result or rethrows the error (once) |
| `wait()`, `waitFor(timeout)`, `waitUntil(deadline)` | wait for the chain to finish |
| `isReady()` | tells if it has finished |
| `cancel()` | requests the [cancellation](#cancellation) of the chain |

The handle can be dropped: the chain still runs.

## Errors

When a task throws, the rest of its chain doesn't run, and the error is delivered:

- to the nearest `onError` handler that takes it, from the failing task on to the end of the chain;
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

A handler is called for the exceptions of its parameter type (a generic one takes a `std::exception`), or for any
error if it takes a `std::exception_ptr`. It may take the chain's `std::stop_token` as its second parameter:

```cpp
Task::run(loop.get(), &Device::connect, &device)
    .onError([](const TimeoutError&) { /* only timeouts */ })
    .onError(pool.get(), [&device](const std::exception& ex) { device.markOffline(ex.what()); })
    .submit();
```

A handler runs on the executor given to it, otherwise on the executor of the task before it: inline if the error is
delivered there, or if that executor is shutting down.

Errors that nobody handles go to the process-wide unhandled-exception handler:

- an exception that escapes a posted function or an `onError` handler (one that escapes a `recover` handler goes on
  down the chain instead);
- the error of a chain that no `onError` handler received, once its handle is gone without `get()` having been
  called;
- the error of a periodic run without an `onError` handler for it.

It writes them to stderr by default; replace it to log them your way (it may be called from any executor thread):

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

Cancellations (`OperationCanceledException`) are never reported there.

## Cancellation

Cancellation is cooperative and uses the standard `std::stop_source` and `std::stop_token`. Each chain has its own
stop source: `handle.cancel()` requests a stop, and so does a stop of the token given to `Task::run()`. Every task of
the chain checks it before it starts: a canceled task doesn't run, and its chain receives an
`OperationCanceledException` (handlers and handle, as for any error).

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

To react to a stop request, register a `std::stop_callback` on the token. `Task::post()` takes a token too: the
function doesn't run if a stop was requested by the time it would.

## Delayed and periodic tasks

Give a delay, or a delay and an interval. The waiting happens on an internal timer thread; the tasks themselves run
on the executors you chose:

```cpp
std::stop_source stop;

Task::run(loop.get(), stop.get_token(), 5s, [] { /* once, in 5 s */ }).submit();

auto ticker = Task::run(loop.get(), 0s, 10s, [] { /* every 10 s */ })
                  .onError([](const std::exception& ex) { log(ex.what()); })
                  .submit();

ticker.cancel();      // ends the series
stop.request_stop();  // cancels the delayed task, at once
```

The interval is a fixed rate by default (`FixedRate{10s}` says it explicitly): runs keep a steady rhythm, missed
periods are skipped rather than run in a burst, and on a thread pool runs can overlap if one takes longer than the
interval. With `FixedDelay{10s}`, the next run starts 10 s after the previous run's whole chain has finished, so
runs never overlap:

```cpp
Task::run(pool.get(), 0s, FixedDelay{10s}, [] { return poll(); })
    .then(loop.get(), [](Status status) { show(status); })
    .submit();
```

The interval must be positive. Each run is a fresh copy of the chain, so its functions must be copyable. A failing
run doesn't end the series: its error goes to its handlers, or is reported. The handle of a periodic task is ready
once the series has ended (canceled, or its executor destroyed); `get()` then throws `OperationCanceledException`.
When a series is canceled, only a run that was already queued reports the cancellation.

## Running chains together

`whenAll` and `whenAny` start a chain that runs other (unsubmitted) chains at the same time:

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
- Both take a `std::vector<TaskBuilder<T>>` too: `whenAll` then gives a `std::vector<T>` in the builders' order,
  `whenAny` the index of the winner and its result.

The joined chains keep their delays, handlers and recoveries. Canceling the join's handle stops them.

## Shutting down

Destroying an executor never loses tasks silently. Tasks that are still queued are canceled: their handles and
`onError` handlers receive an `OperationCanceledException` (a posted function is just dropped). Tasks handed to it
afterwards, such as the continuation of a chain running elsewhere, are canceled the same way: a chain holds its
executors' shared state, never a dangling executor.

The executor object itself must outlive the calls made on it: stop the producers (for example network callbacks)
before destroying it.

## Rules worth knowing

- **Where code runs.** A task runs on its executor. A handler runs on its executor (see [Errors](#errors)).
  A `ManualExecutor` only runs anything inside `runPending()` and `runUntil()`.
- **Waiting inside a pool task** for another pool task works, as long as the pool has another thread to run it.
  Don't wait for a handle on the thread that runs its `ManualExecutor`: the task can't run while that thread waits.
- **Token parameters.** A function whose first parameter takes a `std::stop_token` receives the chain's token, so a
  variadic generic lambda (`[](auto&&...)`) receives it too.
- **Debug output.** `<vanilo/core/Tracer.h>` provides `VANILO_TRACE`, `VANILO_ASSERT` and `VANILO_VERIFY`,
  which compile to nothing in release builds.
- **ShutdownGate.** `<vanilo/concurrent/ShutdownGate.h>` is a small utility to shut a resource down while other
  threads may be using it: `if (auto entry = gate.enter()) { ... }` on the users' side, `gate.close()` (which waits
  for the entries inside) on the owner's side.

## Performance

Measured with the `benchmark` preset (Google Benchmark, median of 5) on an AMD Ryzen AI 9 HX 370 (24 hardware
threads), against 1.0-b4 on the same machine:

| Benchmark | 1.0-b4 | 1.0 | |
|---|---:|---:|---:|
| One producer, pool of 1 / 4 / 16 threads | 3.9 / 1.1 / 0.39 M tasks/s | 9.2 / 3.1 / 3.5 M tasks/s | 2.3× / 2.8× / 9× |
| Four producers, pool of 4 / 16 threads | 0.94 / 0.46 M tasks/s | 4.7 / 6.8 M tasks/s | 5× / 15× |
| Tasks queueing tasks, pool of 4 / 16 threads | 1.8 / 1.2 M tasks/s | 21 / 21 M tasks/s | 12× / 18× |
| One task and its result (round trip) | 4.3 µs | 1.6 µs | 2.8× |
| Three-step chain | 7.0 µs | 4.4 µs | 1.6× |
| `ManualExecutor`: queue, then run | 14.6 M tasks/s | 47.7 M tasks/s | 3.3× |
| Canceled tasks (posted / chains) | 0.43 M tasks/s | 37 / 11 M tasks/s | 86× / 25× |
| Delayed tasks | 0.40 M tasks/s | 0.50 M tasks/s | 1.2× |

What makes the difference: per-thread queues with work stealing instead of one locked queue, waking a thread only
when one sleeps and never under a lock, small posted functions stored without allocating, a chain's state moved
from task to task instead of copied, cancellation without throwing, and tasks run in batches by a `ManualExecutor`.

## Migrating from 1.0-b4

| 1.0-b4 | 1.0 |
|---|---|
| `Task::run(executor, f)` (queued when the expression ends) | `Task::post(executor, f)`, or `Task::run(executor, f).submit()` |
| `.getFuture()`, a `std::future` | `.submit()`, a `TaskHandle` (`get`, `wait`, `waitFor`, `cancel`) |
| `.onException(handler)` | `.onError(handler)`: called for the handler's exception type; no bound task arguments (capture them) |
| (new) | `.recover(handler)`, `whenAll`, `whenAny`, `FixedDelay`, `TaskHandle::cancel()` |
| `CancellationTokenSource`, `CancellationToken` | `std::stop_source`, `std::stop_token` |
| `token.throwIfCancellationRequested()` | `throwIfStopRequested(token)` |
| `token.subscribe(callback)` | `std::stop_callback` |
| `CancellationToken::none()`, `CancellationToken{}` | `std::stop_token{}` |
| `LocalThreadExecutor`, `process(max)`, `process(token)` | `ManualExecutor`, `runPending(max)`, `runUntil(token)` |
| `executor->count()` | `executor->pendingCount()` |
| `ThreadPoolExecutor::DefaultThreadNumber` | `ThreadPoolExecutor::defaultThreadCount()` |
| `LifeCycleGuard` (`acquire`, `terminate`) | `ShutdownGate` (`enter`, `close`) |
| `<vanilo/concurrent/CancellationToken.h>` | `<vanilo/concurrent/Cancellation.h>` (`Tasker.h` includes everything) |

Behaviour that changed:

- C++20 is required.
- A chain is only queued by `submit()`; the compiler warns about a builder that is dropped.
- The error of a chain without an `onError` handler is reported once its handle is gone without `get()` (1.0-b4
  reported it when the chain had no future).
- A `const std::exception&` handler no longer receives non-standard exceptions (take a `std::exception_ptr`).
- A `Task` object submitted to an executor that shuts down gets `cancel()` instead of `cancel()` and then `run()`.
- A periodic series ends when its executor is destroyed.
- Generic lambdas and overloaded function objects work as tasks and handlers.

## Building and testing

The presets build into `build/<preset>`:

| Preset | What |
|---|---|
| `debug`, `release` | Normal builds |
| `asan` | AddressSanitizer (with leak detection) and UBSan |
| `tsan` | ThreadSanitizer |
| `coverage` | Clang source-based coverage |
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

CMake options (the `TASKER_*` ones, except the benchmarks, default to ON only when this is the top-level project):

| Option | Meaning |
|---|---|
| `TASKER_BUILD_TESTS` | Build the unit tests |
| `TASKER_BUILD_EXAMPLES` | Build the examples |
| `TASKER_BUILD_BENCHMARKS` | Build the benchmarks (Google Benchmark, OFF by default) |
| `TASKER_WARNINGS_AS_ERRORS` | Treat warnings as errors (the library's own targets build with `-Wall -Wextra -Wpedantic`) |
| `TASKER_INSTALL` | Install rules and the `Vanilo` CMake package |
| `TASKER_SANITIZER` | `address`, `thread` or empty |
| `TASKER_COVERAGE` | Coverage instrumentation (Clang) and the `tasker-coverage` target |
| `BUILD_SHARED_LIBS` | Shared or static library (default when top-level: shared) |

The examples in this README are compiled and run by `vanilo/tests/src/ReadmeExamplesTests.cpp`.

### Project structure

- `vanilo/include/vanilo/` – public headers (`tasker/Tasker.h` includes them all; `tasker/detail/` holds the
  templates behind them)
- `vanilo/private/vanilo/`, `vanilo/src/vanilo/` – implementation
- `vanilo/tests/src/` – Catch2 tests, including regression tests for every fixed bug
- `benchmarks/` – the benchmarks
- `examples/` – a small periodic-task example
- `cmake/VaniloConfig.cmake.in` – template of the installed CMake package

## License

MIT, see [LICENSE](LICENSE).
