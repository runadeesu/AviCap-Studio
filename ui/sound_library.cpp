#include "ui/sound_library.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <nlohmann/json.hpp>

#include "core/file_io.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"
#include "decode/audio_reader.h"
#include "media/probe.h"
#include "core/i18n.h"
#include "ui/app.h"

namespace avc::ui {

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

bool isAudioFile(const fs::path& p) {
    std::string ext = toLower(pathToUtf8(p.extension()));
    const auto& exts = supportedAudioExtensions();
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

int64_t mtimeNs(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

int64_t fileClockNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(fs::file_time_type::clock::now().time_since_epoch()).count();
}

// Folder names decide the category; otherwise the length does (filled in later).
std::string categoryFromPath(const fs::path& p) {
    for (const auto& part : p) {
        const std::string s = toLower(pathToUtf8(part));
        if (s == "bgm" || s == "music" || s == "音楽" || s == "bgm素材") return "bgm";
        if (s == "sfx" || s == "se" || s == "効果音" || s == "sound effects" || s == "myinstants") return "sfx";
    }
    return {};
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

}  // namespace

// ------------------------------------------------------------------ previewer

SoundPreviewer::SoundPreviewer(bool headless) : headless_(headless) {}

SoundPreviewer::~SoundPreviewer() { stop(); }

void SoundPreviewer::play(const std::string& path, double maxSeconds) {
    stop();
    if (!out_) out_ = headless_ ? audio::createNullOutput(48000) : audio::createDefaultOutput();
    if (!out_) return;
    auto buf = std::make_shared<Buffer>();
    buf->rate = out_->sampleRate();
    const size_t maxFrames = static_cast<size_t>(maxSeconds * buf->rate);
    buf->samples.reserve(maxFrames * 2);  // no reallocation while the device reads
    {
        std::lock_guard lk(bufMutex_);
        buf_ = buf;
    }
    current_ = path;
    volumeAtomic_ = volume;
    decoder_ = std::thread([buf, path, maxFrames] {
        setCurrentThreadName("SoundPreview");
        auto r = openAudioReader(path, -1, AudioFormat{buf->rate, 2});
        if (!r) {
            std::lock_guard lk(buf->mutex);
            buf->error = r.errorMessage();
            buf->complete = true;
            return;
        }
        auto& reader = **r;
        const size_t total = std::min<size_t>(maxFrames, static_cast<size_t>(std::max<int64_t>(0, reader.lengthFrames())));
        const size_t chunk = static_cast<size_t>(buf->rate / 4);
        std::vector<float> tmp(chunk * 2);
        for (size_t pos = 0; pos < total && !buf->cancel; pos += chunk) {
            const size_t n = std::min(chunk, total - pos);
            reader.read(static_cast<int64_t>(pos), static_cast<int64_t>(n), tmp.data());
            std::lock_guard lk(buf->mutex);
            buf->samples.insert(buf->samples.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n * 2));
        }
        buf->complete = true;
    });
    out_->start([this](float* out, int frames) {
        std::shared_ptr<Buffer> b;
        {
            std::lock_guard lk(bufMutex_);
            b = buf_;
        }
        std::memset(out, 0, sizeof(float) * static_cast<size_t>(frames) * 2);
        if (!b) return;
        const float vol = volumeAtomic_.load();
        std::lock_guard lk(b->mutex);
        const size_t have = b->samples.size() / 2;
        size_t pos = b->readFrames.load();
        const size_t n = std::min<size_t>(static_cast<size_t>(frames), have > pos ? have - pos : 0);
        for (size_t i = 0; i < n * 2; ++i) out[i] = b->samples[pos * 2 + i] * vol;
        b->readFrames = pos + n;
    });
}

void SoundPreviewer::stop() {
    std::shared_ptr<Buffer> b;
    {
        std::lock_guard lk(bufMutex_);
        b = std::move(buf_);
        buf_.reset();
    }
    if (b) b->cancel = true;
    if (decoder_.joinable()) decoder_.join();
    if (out_) out_->stop();
    current_.clear();
}

void SoundPreviewer::tick() {
    volumeAtomic_ = volume;
    std::shared_ptr<Buffer> b;
    {
        std::lock_guard lk(bufMutex_);
        b = buf_;
    }
    if (!b || !b->complete) return;
    if (b->readFrames.load() >= b->samples.size() / 2) {
        // Finished: keep the error (if any) readable, release the device.
        std::string err;
        {
            std::lock_guard lk(b->mutex);
            err = b->error;
        }
        if (decoder_.joinable()) decoder_.join();
        if (out_) out_->stop();
        std::lock_guard lk(bufMutex_);
        if (err.empty()) {
            buf_.reset();
            current_.clear();
        } else {
            b->samples.clear();
        }
    }
}

bool SoundPreviewer::playing() const {
    std::lock_guard lk(bufMutex_);
    return buf_ && buf_->error.empty();
}

double SoundPreviewer::positionSec() const {
    std::lock_guard lk(bufMutex_);
    return buf_ ? static_cast<double>(buf_->readFrames.load()) / buf_->rate : 0.0;
}

std::string SoundPreviewer::lastError() const {
    std::lock_guard lk(bufMutex_);
    if (!buf_) return {};
    std::lock_guard lk2(buf_->mutex);
    return buf_->error;
}

// ------------------------------------------------------------------ library

SoundLibrary::SoundLibrary(App& app) : app_(app), previewer_(app.headless()) {}

SoundLibrary::~SoundLibrary() {
    *alive_ = false;
    previewer_.stop();
}

fs::path SoundLibrary::rootFolder() const {
    if (!app_.settings().sounds.rootFolder.empty()) return pathFromUtf8(app_.settings().sounds.rootFolder);
    fs::path music = userMusicDir();
    if (music.empty()) music = userDocumentsDir();
    return music / "AviCap Sounds";
}

std::vector<std::string> SoundLibrary::folders() const {
    std::vector<std::string> v{pathToUtf8(rootFolder())};
    for (const auto& f : app_.settings().sounds.libraryFolders)
        if (std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f);
    return v;
}

const SoundItem* SoundLibrary::find(const std::string& path) const {
    for (const auto& i : items_)
        if (i.path == path) return &i;
    return nullptr;
}

void SoundLibrary::rescan() {
    ++scanning_;
    loadSources();
    const auto dirs = folders();
    auto sources = sources_;
    std::weak_ptr<bool> alive = alive_;
    App* app = &app_;
    Jobs::io().submit("Scan sound library", JobPriority::Medium, [this, dirs, sources, alive, app](JobContext& ctx) {
        std::vector<SoundItem> found;
        for (const auto& d : dirs) {
            std::error_code ec;
            const fs::path root = pathFromUtf8(d);
            if (!fs::is_directory(root, ec)) continue;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end && found.size() < 5000 && !ctx.cancelled(); it.increment(ec)) {
                if (!it->is_regular_file(ec) || !isAudioFile(it->path())) continue;
                SoundItem s;
                s.path = pathToUtf8(it->path());
                s.name = pathToUtf8(it->path().stem());
                s.category = categoryFromPath(fs::relative(it->path(), root, ec));
                s.modifiedNs = mtimeNs(it->path());
                if (auto src = sources.find(pathToUtf8(it->path().filename())); src != sources.end()) {
                    s.source = src->second.first;
                    s.sourceUrl = src->second.second;
                }
                found.push_back(std::move(s));
            }
        }
        // Durations (cheap header probe) decide the category of unsorted files.
        for (auto& s : found) {
            if (ctx.cancelled()) break;
            if (auto info = probeMedia(s.path)) s.durationSec = info->duration.seconds();
            if (s.category.empty()) s.category = s.durationSec >= 30.0 ? "bgm" : "sfx";
        }
        std::sort(found.begin(), found.end(), [](const SoundItem& a, const SoundItem& b) { return a.name < b.name; });
        app->post([this, alive, found = std::move(found)]() mutable {
            if (!alive.lock()) return;
            items_ = std::move(found);
            --scanning_;
        });
    });
}

bool SoundLibrary::isFavorite(const std::string& path) const {
    const auto& f = app_.settings().sounds.favorites;
    return std::find(f.begin(), f.end(), path) != f.end();
}

void SoundLibrary::setFavorite(const std::string& path, bool on) {
    auto& f = app_.settings().sounds.favorites;
    std::erase(f, path);
    if (on) f.push_back(path);
    app_.saveSettings();
}

void SoundLibrary::markUsed(const std::string& path) {
    auto& r = app_.settings().sounds.recent;
    std::erase(r, path);
    r.insert(r.begin(), path);
    if (r.size() > 30) r.resize(30);
    app_.saveSettings();
}

std::vector<SoundItem> SoundLibrary::recentDownloads(double hours) const {
    std::vector<SoundItem> out;
    const fs::path dl = userDownloadsDir();
    std::error_code ec;
    if (dl.empty() || !fs::is_directory(dl, ec)) return out;
    const int64_t cutoff = fileClockNowNs() - static_cast<int64_t>(hours * 3600.0 * 1e9);
    int scanned = 0;
    for (fs::directory_iterator it(dl, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && scanned < 2000;
         it.increment(ec), ++scanned) {
        if (!it->is_regular_file(ec) || !isAudioFile(it->path())) continue;
        const int64_t m = mtimeNs(it->path());
        if (m < cutoff) continue;
        SoundItem s;
        s.path = pathToUtf8(it->path());
        s.name = pathToUtf8(it->path().stem());
        s.modifiedNs = m;
        s.category = "sfx";
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const SoundItem& a, const SoundItem& b) { return a.modifiedNs > b.modifiedNs; });
    return out;
}

Result<std::string> SoundLibrary::importFile(const std::string& path, bool music, const std::string& source, const std::string& sourceUrl) {
    const fs::path src = pathFromUtf8(path);
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) return Result<std::string>::error(tr("File not found"));
    if (!isAudioFile(src)) return Result<std::string>::error(tr("Not an audio file"));
    // Check it really decodes before adding it to the library.
    auto info = probeMedia(path);
    if (!info || !info->hasAudio()) return Result<std::string>::error(tr("The file has no audio"));
    fs::path dir = rootFolder() / (music ? "BGM" : "SFX");
    if (!source.empty()) dir /= pathFromUtf8(sanitizeFileName(source));
    fs::create_directories(dir, ec);
    if (ec) return Result<std::string>::error(ec.message());
    fs::path dst = dir / src.filename();
    for (int i = 2; fs::exists(dst, ec) && i < 1000; ++i)
        dst = dir / pathFromUtf8(pathToUtf8(src.stem()) + " (" + std::to_string(i) + ")" + pathToUtf8(src.extension()));
    // Copy, never move: the downloaded original stays where it is.
    fs::copy_file(src, dst, fs::copy_options::none, ec);
    if (ec) return Result<std::string>::error(ec.message());
    if (!source.empty() || !sourceUrl.empty()) {
        sources_[pathToUtf8(dst.filename())] = {source, sourceUrl};
        saveSources();
    }
    AVC_INFO("sounds", "imported {} into the library ({})", pathToUtf8(src.filename()), source.empty() ? "local" : source);
    return pathToUtf8(dst);
}

std::string SoundLibrary::myinstantsUrl(const std::string& search) {
    if (search.empty()) return "https://www.myinstants.com/";
    return "https://www.myinstants.com/en/search/?name=" + urlEncode(search);
}

void SoundLibrary::loadSources() {
    sources_.clear();
    auto text = readFileBytes(rootFolder() / "sources.json");
    if (!text) return;
    Json j = Json::parse(*text, nullptr, false);
    if (!j.is_object()) return;
    for (const auto& [file, v] : j.items())
        if (v.is_object()) sources_[file] = {v.value("source", ""), v.value("url", "")};
}

void SoundLibrary::saveSources() const {
    Json j = Json::object();
    for (const auto& [file, v] : sources_) j[file] = {{"source", v.first}, {"url", v.second}};
    std::error_code ec;
    fs::create_directories(rootFolder(), ec);
    writeFileAtomic(rootFolder() / "sources.json", j.dump(2));
}

}  // namespace avc::ui
