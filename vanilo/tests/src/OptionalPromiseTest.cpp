#include <vanilo/tasker/Tasker.h>

#include <catch2/catch.hpp>

using namespace vanilo::tasker::details;

SCENARIO("Promise must change its state when future is taken", "[promise]")
{
    OptionalPromise<int> promise;

    GIVEN("Promise in initial state")
    {
        WHEN("No future has been taken")
        {
            THEN("The state is invalid")
            {
                REQUIRE_FALSE(promise.isValid());
            }
        }

        WHEN("Future has been taken")
        {
            auto future = promise.getFuture();

            THEN("The state is valid")
            {
                REQUIRE(promise.isValid());
            }
        }
    }
}