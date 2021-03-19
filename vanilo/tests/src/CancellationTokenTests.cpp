#include <vanilo/concurrent/CancellationToken.h>

#include <catch2/catch.hpp>

using namespace vanilo::concurrent;

TEST_CASE("Equality operator should return false for two distinct tokens", "[token]")
{
    CancellationToken token1;
    CancellationToken token2;

    REQUIRE_FALSE(token1 == token2);
}

TEST_CASE("Tokens must be copyable and equality operator should return true for them", "[token]")
{
    CancellationToken token1;
    CancellationToken token2;

    token1 = token2;

    REQUIRE(token1 == token2);
}

SCENARIO("Token state must change with cancellation", "[token]")
{
    CancellationToken token1;

    GIVEN("Token in initial state")
    {
        CancellationToken token2 = token1;

        WHEN("No cancellation has been performed")
        {
            THEN("isCancellationRequested should return false")
            {
                REQUIRE_FALSE(token1.isCancellationRequested());
                REQUIRE_FALSE(token2.isCancellationRequested());
            }
        }

        WHEN("Token is canceled")
        {
            token2.cancel();

            THEN("isCancellationRequested should return true")
            {
                REQUIRE(token1.isCancellationRequested());
                REQUIRE(token2.isCancellationRequested());
            }
        }
    }
}