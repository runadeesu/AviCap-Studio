#include "core/i18n.h"

#include <atomic>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace avc {

namespace {

std::atomic<Language> g_language{Language::English};

// English -> Japanese. Keys are the exact English UI strings.
const std::unordered_map<std::string_view, const char*>& japanese() {
    static const std::unordered_map<std::string_view, const char*> table = {
        // Menus
        {"File", "ファイル"},
        {"Edit", "編集"},
        {"Clip", "クリップ"},
        {"Sequence", "シーケンス"},
        {"Marker", "マーカー"},
        {"View", "表示"},
        {"Window", "ウィンドウ"},
        {"Help", "ヘルプ"},
        {"New Project", "新規プロジェクト"},
        {"Open Project...", "プロジェクトを開く..."},
        {"Open Recent", "最近使用したプロジェクト"},
        {"Save", "保存"},
        {"Save As...", "名前を付けて保存..."},
        {"Import Media...", "メディアを読み込み..."},
        {"Import Media", "メディアを読み込み"},
        {"Import Subtitles...", "字幕を読み込み..."},
        {"Export Subtitles...", "字幕を書き出し..."},
        {"Export...", "書き出し..."},
        {"Export", "書き出し"},
        {"Exit", "終了"},
        {"Undo", "元に戻す"},
        {"Redo", "やり直し"},
        {"Cut", "切り取り"},
        {"Copy", "コピー"},
        {"Paste", "貼り付け"},
        {"Paste Insert", "挿入ペースト"},
        {"Duplicate", "複製"},
        {"Delete", "削除"},
        {"Ripple Delete", "リップル削除"},
        {"Select All", "すべて選択"},
        {"Deselect All", "選択解除"},
        {"Split at Playhead", "再生ヘッドで分割"},
        {"Split", "分割"},
        {"Lift", "リフト"},
        {"Extract", "抽出"},
        {"Insert", "インサート"},
        {"Overwrite", "上書き"},
        {"Replace", "置き換え"},
        {"Group", "グループ化"},
        {"Ungroup", "グループ解除"},
        {"Link", "リンク"},
        {"Unlink", "リンク解除"},
        {"Enable", "有効化"},
        {"Disable", "無効化"},
        {"Enable / Disable", "有効 / 無効"},
        {"Nest as Compound Clip", "複合クリップにまとめる"},
        {"Open Compound Clip", "複合クリップを開く"},
        {"Speed / Duration...", "速度・デュレーション..."},
        {"Reverse", "逆再生"},
        {"Freeze Frame", "フリーズフレーム"},
        {"Add Marker", "マーカーを追加"},
        {"Settings...", "設定..."},
        {"Settings", "設定"},
        {"Keyboard Shortcuts...", "キーボードショートカット..."},
        {"Keyboard Shortcuts", "キーボードショートカット"},
        {"About AviCap Studio", "AviCap Studio について"},
        {"Open Log Folder", "ログフォルダを開く"},
        {"Reset Layout", "レイアウトをリセット"},
        {"Fullscreen Viewer", "ビューアを全画面表示"},
        // Panels
        {"Media", "メディア"},
        {"Project", "プロジェクト"},
        {"Files", "ファイル"},
        {"Sounds", "サウンド"},
        {"Effects", "エフェクト"},
        {"Transitions", "トランジション"},
        {"Titles", "テキスト"},
        {"Viewer", "ビューア"},
        {"Inspector", "インスペクタ"},
        {"Timeline", "タイムライン"},
        {"Audio Mixer", "オーディオミキサー"},
        {"Meters", "メーター"},
        {"Scopes", "スコープ"},
        {"Export Queue", "書き出しキュー"},
        {"AI Assistant", "AI アシスタント"},
        {"Diagnostics", "診断"},
        {"Background Tasks", "バックグラウンドタスク"},
        {"Color", "カラー"},
        // Media browser
        {"Search", "検索"},
        {"Sort", "並べ替え"},
        {"Filter", "フィルタ"},
        {"Favorites", "お気に入り"},
        {"Recent", "最近使用"},
        {"Name", "名前"},
        {"Duration", "デュレーション"},
        {"Date", "日付"},
        {"Type", "種類"},
        {"All", "すべて"},
        {"Video", "ビデオ"},
        {"Audio", "オーディオ"},
        {"Image", "画像"},
        {"Text", "テキスト"},
        {"Subtitle", "字幕"},
        {"Adjustment", "調整"},
        {"Media Offline", "メディアオフライン"},
        {"Locate...", "場所を指定..."},
        {"Relink Media...", "メディアを再リンク..."},
        {"Relink All in Folder...", "フォルダ内で一括再リンク..."},
        {"Create Proxy", "プロキシを作成"},
        {"Use Proxies", "プロキシを使用"},
        {"Add to Timeline", "タイムラインに追加"},
        {"Reveal in Explorer", "エクスプローラーで表示"},
        {"Remove from Project", "プロジェクトから削除"},
        {"Drop media files here or use Import.", "ここにメディアをドロップするか、読み込みを使用してください。"},
        // Viewer / transport
        {"Play", "再生"},
        {"Pause", "一時停止"},
        {"Stop", "停止"},
        {"Loop", "ループ"},
        {"Quality", "画質"},
        {"Full", "フル"},
        {"Half", "1/2"},
        {"Quarter", "1/4"},
        {"Eighth", "1/8"},
        {"Auto", "自動"},
        {"Fit", "全体表示"},
        {"Safe Margins", "セーフマージン"},
        // Inspector
        {"Transform", "トランスフォーム"},
        {"Position", "位置"},
        {"Scale", "スケール"},
        {"Rotation", "回転"},
        {"Anchor", "アンカー"},
        {"Opacity", "不透明度"},
        {"Blend Mode", "描画モード"},
        {"Crop", "クロップ"},
        {"Flip Horizontal", "左右反転"},
        {"Flip Vertical", "上下反転"},
        {"Speed", "速度"},
        {"Volume", "音量"},
        {"Gain", "ゲイン"},
        {"Pan", "パン"},
        {"Mute", "ミュート"},
        {"Solo", "ソロ"},
        {"Lock", "ロック"},
        {"Hide", "非表示"},
        {"Font", "フォント"},
        {"Size", "サイズ"},
        {"Weight", "太さ"},
        {"Fill", "塗り"},
        {"Stroke", "縁取り"},
        {"Shadow", "影"},
        {"Background", "背景"},
        {"Glow", "グロー"},
        {"Gradient", "グラデーション"},
        {"Alignment", "配置"},
        {"Tracking", "字間"},
        {"Line Spacing", "行間"},
        {"Add Effect", "エフェクトを追加"},
        {"Remove", "削除"},
        {"Reset", "リセット"},
        {"Keyframe", "キーフレーム"},
        {"No clip selected", "クリップが選択されていません"},
        // Timeline
        {"Add Video Track", "ビデオトラックを追加"},
        {"Add Audio Track", "オーディオトラックを追加"},
        {"Add Text Track", "テキストトラックを追加"},
        {"Delete Track", "トラックを削除"},
        {"Snapping", "スナップ"},
        {"Zoom to Fit", "全体表示"},
        {"Selection Tool", "選択ツール"},
        {"Blade Tool", "ブレードツール"},
        {"Ripple Tool", "リップルツール"},
        {"Roll Tool", "ロールツール"},
        {"Slip Tool", "スリップツール"},
        {"Slide Tool", "スライドツール"},
        {"Hand Tool", "手のひらツール"},
        {"Add Text", "テキストを追加"},
        // AI
        {"Remove Silence", "無音部分をカット"},
        {"Detect Silence", "無音を検出"},
        {"Auto Captions", "自動字幕"},
        {"Detect Scenes", "シーンを検出"},
        {"Detect Beats", "ビートを検出"},
        {"Smart Highlights", "スマートハイライト"},
        {"Preview", "プレビュー"},
        {"Apply", "適用"},
        {"Cancel", "キャンセル"},
        {"Threshold", "しきい値"},
        {"Minimum Silence", "最小無音長"},
        {"Padding Before", "前の余白"},
        {"Padding After", "後の余白"},
        {"Ask AviCap to edit...", "AviCap に編集を指示..."},
        // Export
        {"Preset", "プリセット"},
        {"Resolution", "解像度"},
        {"Frame Rate", "フレームレート"},
        {"Codec", "コーデック"},
        {"Encoder", "エンコーダー"},
        {"Bitrate", "ビットレート"},
        {"Rate Control", "レート制御"},
        {"Audio Codec", "音声コーデック"},
        {"Audio Bitrate", "音声ビットレート"},
        {"Sample Rate", "サンプルレート"},
        {"Output File", "出力ファイル"},
        {"Add to Queue", "キューに追加"},
        {"Start Export", "書き出し開始"},
        {"Pause", "一時停止"},
        {"Resume", "再開"},
        {"Retry", "再試行"},
        {"Progress", "進行状況"},
        {"Elapsed", "経過時間"},
        {"Remaining", "残り時間"},
        {"Verified", "検証済み"},
        // Dialogs
        {"Recover Project", "プロジェクトを復元"},
        {"Discard", "破棄"},
        {"Recover", "復元"},
        {"AviCap Studio did not shut down correctly. Recover unsaved changes?",
         "AviCap Studio が正しく終了しませんでした。保存されていない変更を復元しますか？"},
        {"Unsaved Changes", "未保存の変更"},
        {"Save changes before closing?", "閉じる前に変更を保存しますか？"},
        {"Don't Save", "保存しない"},
        {"OK", "OK"},
        {"Close", "閉じる"},
        {"Yes", "はい"},
        {"No", "いいえ"},
        {"General", "一般"},
        {"Interface", "インターフェース"},
        {"Playback", "再生"},
        {"Performance", "パフォーマンス"},
        {"GPU", "GPU"},
        {"Proxy", "プロキシ"},
        {"Cache", "キャッシュ"},
        {"AI", "AI"},
        {"Keyboard", "キーボード"},
        {"Privacy", "プライバシー"},
        {"Language", "言語"},
        {"Clean Cache Now", "今すぐキャッシュを削除"},
#include "core/i18n_ja.inc"
    };
    return table;
}

}  // namespace

void setLanguage(Language lang) { g_language = lang; }
Language currentLanguage() { return g_language.load(); }

Language languageFromSetting(std::string_view setting) {
    if (setting == "ja") return Language::Japanese;
    if (setting == "en") return Language::English;
#if defined(_WIN32)
    const LANGID id = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) == LANG_JAPANESE) return Language::Japanese;
#endif
    return Language::English;
}

const char* tr(const char* english) {
    if (g_language.load(std::memory_order_relaxed) == Language::Japanese) {
        const auto& t = japanese();
        auto it = t.find(english);
        if (it != t.end()) return it->second;
    }
    return english;
}

bool hasTranslation(std::string_view english, Language lang) {
    if (lang == Language::English) return true;
    return japanese().count(english) != 0;
}

}  // namespace avc
