# Third-party components / サードパーティ製コンポーネント

AviCap Studio uses the following third-party software. Versions are pinned in
the build scripts so every release can be reproduced from source. Full license
texts are shipped in the `licenses/` folder of every release.

| Component | Version | License | How it is used |
|---|---|---|---|
| [FFmpeg](https://ffmpeg.org/) (libavcodec, libavformat, libavutil, libswscale, libswresample, libavfilter) | n8.1.3 (MinGW build) / 8.1.2 (vcpkg 2026.07.29, MSVC build) | LGPL-2.1-or-later | Decoding, encoding, muxing. Built **without** `--enable-gpl` / `--enable-nonfree`, linked **dynamically** (replaceable DLLs). Build recipe: `tools/deps/build_deps_mingw.sh`, `vcpkg.json`. |
| [dav1d](https://code.videolan.org/videolan/dav1d) | 1.5.1 | BSD-2-Clause | AV1 software decoding (inside FFmpeg). |
| [zlib](https://zlib.net/) | 1.3.1 | zlib | Compression (inside FFmpeg). |
| [Intel oneVPL dispatcher (libvpl)](https://github.com/intel/libvpl) | v2.17.0 | MIT | Intel Quick Sync encode/decode (inside FFmpeg). |
| [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) | n13.0.19.0 | MIT | NVENC/NVDEC interface headers (no NVIDIA binaries are shipped; the driver provides them). |
| [AMD AMF headers](https://github.com/GPUOpen-LibrariesAndSDKs/AMF) | v1.5.3 | MIT | AMD hardware encoder interface headers (runtime comes with the AMD driver). |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking branch) | 1.92.9b | MIT | User interface. |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | Project files, settings, journal. |
| [stb](https://github.com/nothings/stb) (stb_truetype, stb_image, stb_image_write, stb_rect_pack) | 2024 | MIT or Public Domain (Unlicense) | Text rasterisation fallback, thumbnails, PNG screenshots. |
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT | Unit tests only (not shipped). |
| MinGW-w64 runtime (winpthreads, libgcc, libstdc++) | GCC 13 | MIT / ZPL / GCC Runtime Library Exception | Statically linked runtime of the MinGW build. |
| Microsoft Visual C++ runtime | VS 2022 | Microsoft redistributable terms | App-local runtime of the MSVC build. |

Platform APIs used through the Windows SDK (not redistributed): Direct3D 11,
DXGI, Direct2D, DirectWrite, WIC, Media Foundation, WASAPI, D3D11 Video
(DXVA), DbgHelp (loaded on demand for crash dumps).

## Tools used to build releases (not shipped)

* MinGW-w64 GCC, CMake, Ninja, Meson.
* [Inno Setup](https://jrsoftware.org/isinfo.php) 6.7.3 to build the installer
  (official signed binary, signature verified before use).
* vcpkg 2026.07.29 for the MSVC build.

## Fonts

No fonts are bundled. The UI and titles use fonts installed on Windows
(Meiryo UI / Yu Gothic UI / Segoe UI and any font the user selects).

## FFmpeg source code

See `licenses/FFMPEG-SOURCE.txt` in a release. The exact FFmpeg source used
for a release is available on request for at least three years after that
release, in addition to the public upstream repository.
