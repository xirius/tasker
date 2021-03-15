#include <vanilo/tasker/Tasker.h>

#include <catch2/catch.hpp>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;

class CustomTask: public Task
{
  public:
    explicit CustomTask(int& value): _value{value}
    {
    }

    void cancel() noexcept override
    {
    }

    void run() override
    {
        _value = 1;
    }

  private:
    int& _value;
};

SCENARIO("Test the flow of the LocalThreadExecutor", "[local executor]")
{
    GIVEN("An initial executor")
    {
        int value1{0}, value2{0}, value3{0};
        auto task1 = std::make_unique<CustomTask>(value1);
        auto task2 = std::make_unique<CustomTask>(value2);
        auto task3 = std::make_unique<CustomTask>(value3);

        auto executor = LocalThreadExecutor::create();

        WHEN("No task are submitted")
        {
            THEN("The count should return 0")
            {
                REQUIRE(executor->count() == 0);
            }
        }

        WHEN("2 tasks are submitted")
        {
            executor->submit(std::move(task1));
            executor->submit(std::move(task2));
            executor->submit(std::move(task3));

            THEN("The count should return 3")
            {
                REQUIRE(executor->count() == 3);
            }
        }

        WHEN("Executor is asked to process 2 tasks")
        {
            executor->submit(std::move(task1));
            executor->submit(std::move(task2));
            executor->submit(std::move(task3));

            auto enqueued = executor->process(2);

            THEN("The number of task in the queue should be 1")
            {
                REQUIRE(enqueued == 1);
                REQUIRE(value1 == 1);
                REQUIRE(value2 == 1);
                REQUIRE(value3 == 0);
            }
        }
    }
}

SCENARIO("Test resizing of ThreadPoolExecutor", "[pool executor]")
{
    GIVEN("An initial executor with 0 threads")
    {
        auto executor = ThreadPoolExecutor::create(0);

        WHEN("Resized to 1")
        {
            executor->resize(1);

            THEN("The thread count should return 1")
            {
                REQUIRE(executor->threadCount() == 1);
            }
        }
    }

    GIVEN("An initial executor with 1 thread")
    {
        auto executor = ThreadPoolExecutor::create(1);

        WHEN("Resized to 0")
        {
            auto future = executor->resize(0);

            THEN("The thread count should return 0")
            {
                future.get();
                REQUIRE(executor->threadCount() == 0);
            }
        }
    }

    GIVEN("An initial executor with 10 threads")
    {
        auto executor = ThreadPoolExecutor::create(10);

        WHEN("Resized to 25")
        {
            auto future = executor->resize(5);

            THEN("The thread count should return 5")
            {
                future.get();
                REQUIRE(executor->threadCount() == 5);
            }
        }

        WHEN("Resized to 0")
        {
            auto future = executor->resize(0);

            THEN("The thread count should return 0")
            {
                future.get();
                REQUIRE(executor->threadCount() == 0);
            }
        }
    }
}

SCENARIO("Test cancellation of tasks when ThreadPoolExecutor is released", "[pool executor]")
{
    GIVEN("An initial executor with 0 threads")
    {
        auto executor    = ThreadPoolExecutor::create(0);
        bool isCancelled = false;
        bool nonExecuted = true;

        WHEN("Task is submitted and executor is then released")
        {
            Task::run(executor.get(), [&nonExecuted]() {
                nonExecuted = false;
            }).onException(executor.get(), [&isCancelled](std::exception& ex, CancellationToken& token) {
                isCancelled = token.isCanceled() && dynamic_cast<CancellationException*>(&ex) != nullptr;
            });

            executor.reset();

            THEN("The task should be canceled")
            {
                REQUIRE(executor == nullptr);
                REQUIRE(nonExecuted);
                REQUIRE(isCancelled);
            }
        }
    }
}