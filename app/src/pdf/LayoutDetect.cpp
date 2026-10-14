#include "pdf/LayoutDetect.h"

#include <algorithm>
#include <cstddef>

namespace {

constexpr std::size_t kCheckedPages = 20;
constexpr double kLandscapeRatio = 1.2;

} // namespace

LayoutMode detectLayout(const std::vector<PageSizePts>& sizes)
{
    const std::size_t checked = std::min(sizes.size(), kCheckedPages);
    std::size_t landscape = 0;
    for (std::size_t i = 0; i < checked; ++i) {
        const PageSizePts& s = sizes[i];
        if (s.h > 0 && s.w / s.h >= kLandscapeRatio)
            ++landscape;
    }
    return landscape * 2 > checked ? LayoutMode::SinglePage : LayoutMode::Continuous;
}
