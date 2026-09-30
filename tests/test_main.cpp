#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include "core/log.h"
#include "tests/test_support.h"

int main(int argc, char** argv) {
    avc::test::initTestEnvironment();
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    const int result = ctx.run();
    avc::test::shutdownTestEnvironment();
    return result;
}
