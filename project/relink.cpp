#include "project/relink.h"

#include "core/file_io.h"
#include "core/strings.h"

namespace avc {

bool mediaOnline(const MediaItem& m) {
    std::error_code ec;
    return !m.path.empty() && std::filesystem::is_regular_file(pathFromUtf8(m.path), ec);
}

std::vector<MissingMedia> findMissingMedia(const Project& project) {
    std::vector<MissingMedia> out;
    for (auto& m : project.media)
        if (!mediaOnline(*m)) out.push_back({m->id, pathToUtf8(pathFromUtf8(m->path).filename()), m->path, m->fileSize});
    return out;
}

std::map<MediaId, std::string> searchFolderForMedia(const std::vector<MissingMedia>& missing,
                                                    const std::filesystem::path& folder, int maxDepth,
                                                    const CancelToken& cancel) {
    std::map<MediaId, std::string> found;
    std::multimap<std::string, const MissingMedia*> byName;
    for (auto& m : missing) byName.emplace(toLower(m.name), &m);
    std::error_code ec;
    auto it = std::filesystem::recursive_directory_iterator(
        folder, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (cancel.cancelled()) break;
        if (it.depth() > maxDepth) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string name = toLower(pathToUtf8(it->path().filename()));
        auto range = byName.equal_range(name);
        for (auto r = range.first; r != range.second; ++r) {
            const MissingMedia* m = r->second;
            if (found.count(m->id)) continue;
            if (m->fileSize != 0 && it->file_size(e2) != m->fileSize) continue;
            found[m->id] = pathToUtf8(it->path());
        }
        if (found.size() == missing.size()) break;
    }
    return found;
}

std::map<MediaId, std::string> inferRelinks(const Project& project, MediaId located, const std::string& newPath) {
    std::map<MediaId, std::string> out;
    const MediaItem* anchor = project.findMedia(located);
    if (!anchor) return out;
    const auto oldDir = pathFromUtf8(anchor->path).parent_path();
    const auto newDir = pathFromUtf8(newPath).parent_path();
    for (auto& m : project.media) {
        if (m->id == located || mediaOnline(*m)) continue;
        std::error_code ec;
        auto rel = std::filesystem::relative(pathFromUtf8(m->path), oldDir, ec);
        if (ec || rel.empty() || pathToUtf8(rel).rfind("..", 0) == 0) continue;
        auto candidate = newDir / rel;
        if (std::filesystem::is_regular_file(candidate, ec)) out[m->id] = pathToUtf8(candidate);
    }
    return out;
}

Status relinkMedia(ProjectEditor& pe, MediaId id, const std::string& newPath) {
    const auto p = pathFromUtf8(newPath);
    const FileIdentity fid = fileIdentity(p);
    if (!fid.exists) return Status::error("File not found: " + newPath);
    MediaItem& m = pe.media(id);
    m.path = pathToUtf8(p);
    m.fileSize = fid.size;
    m.fileModifiedNs = fid.modifiedNs;
    return Status::ok();
}

}  // namespace avc
