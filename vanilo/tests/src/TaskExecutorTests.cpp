#include <vanilo/tasker/Tasker.h>

#include <catch2/catch.hpp>

using namespace vanilo::tasker;

class CustomTask: public Task
{
  public:
    explicit CustomTask(int& value): _value{value}
    {
    }

    void run() override
    {
        _value = 1;
    }

  private:
    int& _value;
};

SCENARIO("Test the flow of the QueuedTaskExecutor", "[local executor]")
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