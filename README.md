# Vanilo Tasker

A small C++17 library for running work on executors: thread pools or a thread you drive yourself.
Tasks can be chained, return results through `std::future`, be delayed or repeated, and be canceled
cooperatively. Errors travel along a chain to the nearest handler, and nothing fails silently.

```cpp
#include <vanilo/tasker/Tasker.h>

using namespace vanilo::tasker;

auto pool = ThreadPoolExecutor::create();

auto answer = Task::run(pool.get(), [] { return 6; })
                  .then(pool.get(), [](int value) { return value * 7; })
                  .getFuture();

answer.get(); // 42
```

- [Getting it](#getting-it)
- [Executors](#executors)
- [Running tasks](#running-tasks)
- [Results and chains](#results-and-chains)
- [Errors](#errors)
- [Cancellation](#cancellation)
- [Delayed and periodic tasks](#delayed-and-periodic-tasks)
- [Shutting down](#shutting-down)
- [Rules worth knowing](#rules-worth-knowing)
- [Building and testing](#building-and-testing)

## Getting it

Requirements: CMake 3.25 or newer and a C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+). The library
links `Threads::Threads` and `atomic`.

With `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(tasker
    GIT_REPOSITORY https://github.com/xirius/tasker.git
    GIT_TAG v1.0-b4
    FIND_PACKAGE_ARGS NAMES Vanilo CONFIG)
FetchContent_MakeAvailable(tasker)

target_link_libraries(my_app PRIVATE Vanilo::Vanilo)
```

Tasker's tests, examples and strict warnings are off when it's part of another project. The consumer decides
`BUILD_SHARED_LIBS`; to link it statically, wrap `FetchContent_MakeAvailable` in a `block()` that sets
`BUILD_SHARED_LIBS OFF` (Vanilo is always position-independent).

An installed tasker (`cmake --install build/release --prefix <prefix>`) is found with `find_package(Vanilo CONFIG)`.

## Executors

Every task runs on an executor. There are two:

| Executor | Runs tasks on | Use it for |
|---|---|---|
| `ThreadPoolExecutor::create(n)` | `n` worker threads (default: one per CPU core) | parallel or blocking work |
| `LocalThreadExecutor::create()` | the thread that calls `process()` | keeping all your state on one thread, like an event loop |

```cpp
auto pool = ThreadPoolExecutor::create(4);
pool->resize(8).wait(); // grow or shrink later

auto local = LocalThreadExecutor::create();
local->process(10);     // runs up to 10 queued tasks, returns how many are left
```

A `LocalThreadExecutor` is the way to make other threads hand work to one thread. A typical event
loop runs until a token is canceled:

```cpp
CancellationTokenSource stop;

// From any thread: queue work for the loop thread.
Task::run(local.get(), [] { /* runs on the loop thread */ });

// On the loop thread: process tasks until stop.cancel() is called.
local->process(stop.token());
```

`process(token)` waits for new tasks while the token isn't canceled. Tasks that are already queued when it's
canceled still run before it returns.

## Running tasks

`Task::run(executor, function, arguments...)` queues a task. Arguments are copied into the task; use
`std::ref` for anything the task should change in place:

```cpp
int counter = 0;
Task::run(local.get(), [](int& value, int step) { value += step; }, std::ref(counter), 5);
local->process(1); // counter == 5
```

Free functions, lambdas, function objects and member functions all work. For a member function, pass the
object (by pointer to use an existing one) before the arguments:

```cpp
struct Thermostat
{
    void setTarget(double celsius) { target = celsius; }
    double target = 0;
};

Thermostat thermostat;
Task::run(local.get(), &Thermostat::setTarget, &thermostat, 21.5);
```

The task is queued when the expression `Task::run(...)...` ends (when its builder is destroyed), so
everything chained to it (`then`, `onException`, `getFuture`) is set up first. Don't keep the builder in a
variable: the task would only be queued when the variable goes out of scope.

## Results and chains

`getFuture()` gives a `std::future` for the result of the last task in a chain. `then(executor, function)`
adds a task that receives the previous result. Each task can run on a different executor:

```cpp
auto future = Task::run(pool.get(), [] { return readSensor(); })              // on the pool
                  .then(local.get(), [](double celsius) { return celsius > 21; }) // on the loop thread
                  .getFuture();
```

A task returning `void` is followed by a task without parameters.

## Errors

When a task throws, the rest of its chain doesn't run, and the exception is delivered:

- to the nearest `onException` handler, from the failing task on to the end of the chain;
- to the future at the end of the chain (if there is one), even when a handler ran;
- if there is neither, to the process-wide unhandled-exception handler.

`onException` applies to the task right before it:

```cpp
Task::run(pool.get(), [] { return download(); })
    .then(pool.get(), [](const std::string& body) { parse(body); })
    .onException([](const std::exception& ex) { log(ex.what()); }); // handles errors of both tasks
```

A handler runs on the executor given to `onException`, otherwise on the executor of the task it's attached
to. It may also take the chain's cancellation token, and the task's own arguments after it:

```cpp
Task::run(local.get(), token, [](const CancellationToken&, Device& device) { device.connect(); }, std::ref(device))
    .onException(pool.get(), [](const std::exception& ex, const CancellationToken&, Device& device) {
        device.markOffline(ex.what());
    });
```

Exceptions nobody handles, including exceptions thrown by a handler, go to the unhandled-exception handler.
It writes them to stderr by default; replace it to log them your way (it may be called from any executor
thread):

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

Cancellation is cooperative, through a `CancellationTokenSource` and the `CancellationToken`s it hands out.
Pass a token to `Task::run`, and every task of the chain checks it before it starts: a canceled task doesn't
run, and its chain receives an `OperationCanceledException` (handler and future, as for any error).

A task that wants to stop halfway takes the token as its first parameter; it is passed automatically:

```cpp
CancellationTokenSource source;

Task::run(pool.get(), source.token(), [](const CancellationToken& token) {
    for (auto& chunk : chunks) {
        token.throwIfCancellationRequested();
        process(chunk);
    }
});

source.cancel(); // from anywhere
```

To react to a cancellation, subscribe a callback. The subscription is removed when its object is destroyed;
`unsubscribe()` waits for a callback that is running on another thread, so the callback's captures stay valid:

```cpp
auto subscription = source.token().subscribe([&] { connection.abort(); });
```

`CancellationToken{}` (or `CancellationToken::none()`) never cancels.

## Delayed and periodic tasks

Give a delay, or a delay and an interval. The waiting happens on an internal scheduler thread; the task
itself runs on the executor you chose:

```cpp
CancellationTokenSource stop;

Task::run(local.get(), stop.token(), std::chrono::seconds{5}, [] { /* once, in 5 s */ });

Task::run(local.get(), stop.token(), std::chrono::seconds{0}, std::chrono::seconds{10}, [] { /* every 10 s */ })
    .onException([](const std::exception& ex) { log(ex.what()); });

stop.cancel(); // ends both
```

Periodic tasks keep a steady rhythm: missed periods are skipped rather than run in a burst. The interval must
be positive. Each run is a fresh copy of the task, so the function must be copyable. When a periodic task is
canceled, only a run that was already queued reports the cancellation; ending the series itself is silent.

## Shutting down

Destroying an executor (or the scheduler) never loses tasks silently. Tasks that are still queued are canceled:
their futures and `onException` handlers receive an `OperationCanceledException`. A task submitted while an
executor is shutting down is canceled the same way.

An executor must outlive the code that submits tasks to it: stop the producers (for example network callbacks)
before destroying the executor.

## Rules worth knowing

- **Where code runs.** A task runs on its executor. A handler runs on its executor (see [Errors](#errors)).
  A `LocalThreadExecutor` only runs anything inside `process()`.
- **Periodic runs can overlap** on a thread pool if one takes longer than the interval. Use a
  `LocalThreadExecutor`, or a pool of one thread, when they must not.
- **Callables are inspected for their parameters**, so generic lambdas (`[](auto value)`) and overloaded
  function objects can't be used as tasks or handlers.
- **Debug output.** `<vanilo/core/Tracer.h>` provides `VANILO_TRACE`, `VANILO_ASSERT` and `VANILO_VERIFY`,
  which compile to nothing in release builds.

## Building and testing

The presets build into `build/<preset>`:

| Preset | What |
|---|---|
| `debug`, `release` | Normal builds |
| `asan` | AddressSanitizer (with leak detection) and UBSan |
| `tsan` | ThreadSanitizer |
| `coverage` | Clang source-based coverage |

```sh
cmake --workflow --preset debug                               # configure, build, test
cmake --workflow --preset tsan                                # tests under ThreadSanitizer
cmake --preset coverage && cmake --build --preset coverage --target tasker-coverage
                                                              # report, HTML in build/coverage/coverage/html
./build/debug/bin/tests/Vanilo.tests "[scheduler]"            # one tag
./build/debug/bin/Example01                                   # the example
```

CMake options (the `TASKER_*` ones default to ON only when tasker is the top-level project):

| Option | Meaning |
|---|---|
| `TASKER_BUILD_TESTS` | Build the unit tests |
| `TASKER_BUILD_EXAMPLES` | Build the examples |
| `TASKER_WARNINGS_AS_ERRORS` | Treat warnings as errors (tasker's own targets build with `-Wall -Wextra -Wpedantic`) |
| `TASKER_INSTALL` | Install rules and the `Vanilo` CMake package |
| `TASKER_SANITIZER` | `address`, `thread` or empty |
| `TASKER_COVERAGE` | Coverage instrumentation (Clang) and the `tasker-coverage` target |
| `BUILD_SHARED_LIBS` | Shared or static Vanilo (default when top-level: shared) |

The examples in this README are compiled and run by `vanilo/tests/src/ReadmeExamplesTests.cpp`.

### Project structure

- `vanilo/include/vanilo/` – public headers (`tasker/Tasker.h` is the one to include)
- `vanilo/private/vanilo/`, `vanilo/src/vanilo/` – implementation
- `vanilo/tests/src/` – Catch2 tests, including regression tests for every fixed bug
- `examples/` – a small periodic-task example
- `cmake/VaniloConfig.cmake.in` – template of the installed CMake package

## License

MIT, see [LICENSE](LICENSE).
