#pragma once
// Automated end-to-end check of the real application window:
//   import -> timeline edits -> title/subtitle -> effects/colour -> preview
//   -> playback -> save -> export -> verify, with screenshots.
// Run as: AviCapStudio.exe --self-test <output-dir> [--media <file>]...
// Results go to <output-dir>/selftest.json (exit code 0 = all steps passed).

#include <string>
#include <vector>

#include "ui/platform_win32.h"

namespace avc::studio {

ui::FrameDriver makeSelfTest(const std::string& outDir, const std::vector<std::string>& media);

}  // namespace avc::studio
