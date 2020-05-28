#include <vanilo/tasker/CancellationToken.h>
#include <vanilo/tasker/Tasker.h>

#include <catch2/catch.hpp>

using namespace vanilo::tasker;
using namespace vanilo::tasker::details;

SCENARIO("Task context should return false if no exception callback is set", "[task context]")
{
    CancellationToken token;
    TaskContext context{token};

    GIVEN("TaskContext in initial state")
    {
        WHEN("No exception callback has been set")
        {
            THEN("hasExceptionCallback should return false")
            {
                REQUIRE_FALSE(context.hasExceptionCallback());
            }
        }

        WHEN("Exception callback has been set")
        {
            std::string message;
            context.setExceptionCallback([&message](auto& ex) { message = ex.what(); });

            THEN("hasExceptionCallback should return true")
            {
                REQUIRE(context.hasExceptionCallback());
            }

            context.onException(std::runtime_error{"runtime error"});

            THEN("The exception callback has to be called")
            {
                REQUIRE(message == "runtime error");
            }
        }

        WHEN("No cancellation has been performed")
        {
            THEN("isCanceled should return false")
            {
                REQUIRE_FALSE(context.isCanceled());
            }
        }

        WHEN("Cancellation has been performed")
        {
            token.cancel();

            THEN("isCanceled should return true")
            {
                REQUIRE(context.isCanceled());
            }
        }
    }
}