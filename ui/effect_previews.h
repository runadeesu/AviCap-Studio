#pragma once
// Thumbnails for the effect and transition browser: every effect is applied
// (with its default or a showcase setting) to a small built-in sample picture,
// so the tiles show what the effect does instead of a generic icon.

#include <map>
#include <string>

#include "cache/cache.h"
#include "core/jobs.h"

namespace avc::ui {

// Renders on a private CPU device (safe on any thread). Keys are effect ids
// and "transition:<id>"; an effect whose output looks the same as the input
// at its default settings (e.g. an empty pen mask) maps to nullptr so the UI
// can fall back to an icon tile.
std::map<std::string, cache::ImagePtr> renderEffectPreviews(int width, int height, const CancelToken& cancel = {});

// The sample pictures (RGBA8): 0 = sunset landscape, 1 = night city.
cache::ImagePtr effectPreviewSample(int which, int width, int height);

}  // namespace avc::ui
