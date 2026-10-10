# ビルド方法

## 必要なもの

| 用途 | ツール |
| --- | --- |
| Windows ネイティブ | Visual Studio 2022 Build Tools（C++ / Windows SDK）、CMake 3.25+、Ninja、Git、Inno Setup 6、vcpkg |
| Linux（開発・テスト） | GCC 13+ または Clang 17+、CMake、Ninja、FFmpeg 開発パッケージ（libavcodec など）、フォント（DejaVu、日本語は IPAex など） |
| Linux から Windows 版 | MinGW-w64（posix スレッド版）、Wine（インストーラー作成とスモークテスト用）、zip |

依存ライブラリは公式配布元（winget / vcpkg / 各プロジェクトのソース）からのみ取得します。
FFmpeg は LGPL 構成（`--enable-gpl` なし）でビルドします。

## Windows（MSVC）

```bat
powershell -ExecutionPolicy Bypass -File .\setup_dev.ps1   :: 初回のみ: ツールと vcpkg + FFmpeg
build_release.bat                                         :: ビルド・テスト・パッケージ作成
```

`release\` に次のファイルができます。

- `AviCapStudio.exe`（と DLL。その場で起動可能）
- `AviCapStudio-Setup.exe`（インストーラー）
- `AviCap-Studio-Windows-Portable.zip`（ポータブル版。設定は exe の隣の `UserData`）
- `SHA256SUMS.txt`

オプション: `build_release.bat --skip-tests` / `--skip-installer`。
GitHub Actions（`.github/workflows/build.yml`）でも同じスクリプトを実行します。

## Linux（開発・テスト用）

```sh
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/linux
bash tools/make_test_media.sh build/test_media   # テスト用の合成メディア（ffmpeg CLI が必要）
ctest --test-dir build/linux --output-on-failure
```

Linux ではデスクトップアプリ（Win32）はビルドされません。UI 層はヘッドレスでビルド・テストされ、
`avicap_uishots` で画面のスクリーンショットを PNG に書き出せます。

```sh
./build/linux/bin/avicap_uishots out/          # 1600x900 の各画面
./build/linux/bin/avicap_uishots out/ 1280 720
```

## Linux から Windows 版を作る（MinGW クロスビルド）

```sh
bash tools/deps/build_deps_mingw.sh            # 初回のみ: FFmpeg などを MinGW 向けにビルド
DISPLAY=:0 bash tools/package_windows.sh --smoke-test
```

`--smoke-test` を付けると、Wine 上で `AviCapStudio.exe --self-test`（読み込み・編集・保存・
書き出し・検証）を実行し、失敗したらエラーで止まります。インストーラーの作成には Wine に
インストールした Inno Setup 6 を使います（`ISCC_WINEPREFIX` / `ISCC_PATH` で変更可）。

## CMake オプション

| オプション | 既定 | 内容 |
| --- | --- | --- |
| `AVICAP_BUILD_APP` | Windows で ON | デスクトップアプリ |
| `AVICAP_BUILD_UI` | ON | UI 層（ヘッドレスでも可） |
| `AVICAP_BUILD_CLI` | ON | `avicap-cli`（probe / export / verify / golden-path） |
| `AVICAP_BUILD_TESTS` | ON | テスト |
| `AVICAP_WERROR` | OFF | 警告をエラーにする |

## うまくいかないとき

- 依存のインストールに失敗した場合も、セキュリティ設定を無効にしたり非公式のバイナリを
  取得したりはしません。エラーメッセージの指示（不足しているツール名）に従ってください。
- `release\` の中身が古い場合は `build\msvc-release`（または `build/mingw-release`）を消して再ビルドします。
