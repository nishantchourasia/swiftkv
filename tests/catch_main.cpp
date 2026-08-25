// Catch2's main lives in its own translation unit so the 643 KB header is
// compiled once, not once per test binary.
#define CATCH_CONFIG_MAIN
#include "catch.hpp"
