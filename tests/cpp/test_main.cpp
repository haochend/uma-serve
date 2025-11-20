#include "gtest/gtest.h"

// This is the main entry point for Google Test.
// All individual test files should include this header and define their tests.
// This main function will discover and run all tests.
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
