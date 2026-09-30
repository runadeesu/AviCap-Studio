#include "tests/test_support.h"

#include <random>

#include "core/jobs.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"

namespace avc::test {

namespace {
std::filesystem::path g_root;
}

void initTestEnvironment() {
    std::error_code ec;
    std::random_device rd;
    g_root = std::filesystem::temp_directory_path() / ("avicap-tests-" + std::to_string(currentProcessId()) + "-" +
                                                        std::to_string(rd() & 0xffff));
    std::filesystem::create_directories(g_root, ec);
    LogConfig cfg;
    cfg.directory = g_root / "logs";
    cfg.minLevel = LogLevel::Debug;
    cfg.toStderr = getEnv("AVICAP_TEST_VERBOSE").has_value();
    log::init(cfg);
}

void shutdownTestEnvironment() {
    Jobs::shutdown();
    log::shutdown();
    std::error_code ec;
    if (!getEnv("AVICAP_KEEP_TEST_FILES")) std::filesystem::remove_all(g_root, ec);
}

std::filesystem::path makeTempDir(const std::string& name) {
    static int counter = 0;
    auto p = g_root / (name + "-" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

std::filesystem::path testMediaDir() {
    if (auto env = getEnv("AVICAP_TEST_MEDIA")) return pathFromUtf8(*env);
    return {};
}

}  // namespace avc::test
