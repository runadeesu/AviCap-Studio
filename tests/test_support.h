#pragma once
#include <filesystem>
#include <string>

namespace avc::test {

void initTestEnvironment();
void shutdownTestEnvironment();
// Fresh empty directory under the test scratch root.
std::filesystem::path makeTempDir(const std::string& name);
// Directory with generated media (see tools/make_test_media). Empty if unavailable.
std::filesystem::path testMediaDir();

}  // namespace avc::test
