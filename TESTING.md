# テスト

## 実行方法

```sh
bash tools/make_test_media.sh build/test_media   # 合成テストメディア（初回のみ、ffmpeg CLI が必要）
ctest --test-dir build/linux --output-on-failure  # unit_tests と stress_tests
./build/linux/bin/avicap_tests -tc="*subtitle*"   # 名前で絞り込み（doctest）
```

Windows では `build_release.bat` がビルド後に同じテストを実行します（`--skip-tests` で省略）。
Linux から作った Windows 版は `tools/package_windows.sh --smoke-test` で Wine 上の自己テストを実行します。

## 範囲（139 テストケース）

| ファイル | 内容 |
| --- | --- |
| `test_core.cpp` | 時間・有理数、ジョブ、設定、文字コード判別、ファイル I/O |
| `test_timeline.cpp` | 編集操作（分割・リップル・トリム・リンク・スナップ・速度）、取り消し、評価、1000 クリップのストレス |
| `test_project.cpp` | 保存・読み込み・マイグレーション・ジャーナル復元・再リンク・元メディアを消さないこと |
| `test_decode.cpp` / `test_cache.cpp` / `test_proxy.cpp` | デコード、フレームキャッシュ、サムネイル・波形キャッシュ、プロキシ |
| `test_gpu.cpp` / `test_compositor.cpp` / `test_d3d11.cpp` | カーネル、合成、エフェクト、トランジション（D3D11 は Windows/Wine のみ） |
| `test_text.cpp` | テキストのラスタライズ |
| `test_audio.cpp` | ミキサー・DSP・波形 |
| `test_ai.cpp` / `test_ai_plan.cpp` | 無音・ビート・シーン解析、自然文 → プラン、Claude 連携（モック通信） |
| `test_subtitles.cpp` | SRT / WebVTT |
| `test_i18n.cpp` | すべての UI 文字列に日本語訳があること |
| `test_golden_path.cpp` | 読み込み → 編集 → 保存 → 書き出し → 出力ファイルの検証 |
| `test_ui.cpp` | ヘッドレスの実 UI（ImGui フレーム・マウス / キーボード入力の合成）でのワークフロー、クラッシュ復元、AI プレビュー / 適用、サウンドライブラリ、字幕、比率、エフェクトのプレビュー画像 |

## 画面の確認

`avicap_uishots <出力フォルダー>` で主要な画面（はじめに・編集・ワークスペース・ライブラリ各ページ・
クリップ種別ごとのインスペクター・テーマカラー・設定）とエフェクトのプレビュー一覧を PNG に書き出します。
ウィンドウや GPU は不要です。

## 実機で確認していないもの

- Windows 実機の GPU（NVIDIA / AMD / Intel）での D3D11 描画・D3D11VA デコード・ハードウェアエンコード
- WASAPI 出力（実際のオーディオデバイス）
- Claude API への実接続（テストはモック）

詳しくは [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) を参照してください。
