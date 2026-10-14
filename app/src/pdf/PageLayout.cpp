#include "pdf/PageLayout.h"

#include <algorithm>
#include <cmath>

namespace {

bool overlaps(const PageRect& r, const PageRect& viewRect)
{
    return r.w > 0 && r.h > 0 && r.x < viewRect.x + viewRect.w && r.x + r.w > viewRect.x &&
           r.y < viewRect.y + viewRect.h && r.y + r.h > viewRect.y;
}

} // namespace

void PageLayout::compute(const std::vector<PageSizePts>& sizes, double zoom, int viewportWidth,
                         double /*dpiScale*/, LayoutMode mode, int shownPage, int viewportHeight)
{
    m_zoom = zoom;
    m_mode = mode;
    m_shown = -1;
    m_rects.clear();

    if (mode == LayoutMode::SinglePage) {
        m_rects.assign(sizes.size(), PageRect{0, 0, 0, 0});
        if (shownPage < 0 || shownPage >= static_cast<int>(sizes.size())) {
            m_totalWidth = std::max(viewportWidth, 2 * kGap);
            m_totalHeight = std::max(viewportHeight, 2 * kGap);
            return;
        }
        const PageSizePts& s = sizes[static_cast<size_t>(shownPage)];
        const int w = static_cast<int>(std::lround(s.w * zoom));
        const int h = static_cast<int>(std::lround(s.h * zoom));
        m_totalWidth = std::max(viewportWidth, w + 2 * kGap);
        m_totalHeight = std::max(viewportHeight, h + 2 * kGap);
        m_shown = shownPage;
        m_rects[static_cast<size_t>(shownPage)] = {(m_totalWidth - w) / 2,
                                                   (m_totalHeight - h) / 2, w, h};
        return;
    }

    m_rects.reserve(sizes.size());
    int widest = 0;
    int y = kGap;
    for (const PageSizePts& s : sizes) {
        const int w = static_cast<int>(std::lround(s.w * zoom));
        const int h = static_cast<int>(std::lround(s.h * zoom));
        m_rects.push_back({0, y, w, h});
        widest = std::max(widest, w);
        y += h + kGap;
    }

    m_totalHeight = y;
    m_totalWidth = std::max(viewportWidth, widest + 2 * kGap);
    for (PageRect& r : m_rects)
        r.x = (m_totalWidth - r.w) / 2;
}

PageRect PageLayout::pageRect(int page) const
{
    if (page < 0 || page >= pageCount())
        return {0, 0, 0, 0};
    return m_rects[static_cast<size_t>(page)];
}

int PageLayout::pageAt(int y) const
{
    if (m_mode == LayoutMode::SinglePage) {
        if (m_shown < 0)
            return -1;
        const PageRect& r = m_rects[static_cast<size_t>(m_shown)];
        return (y >= r.y && y < r.y + r.h) ? m_shown : -1;
    }

    // First page whose bottom edge is below y.
    const auto it = std::upper_bound(m_rects.begin(), m_rects.end(), y,
                                     [](int v, const PageRect& r) { return v < r.y + r.h; });
    if (it == m_rects.end() || y < it->y)
        return -1;
    return static_cast<int>(it - m_rects.begin());
}

std::vector<int> PageLayout::visiblePages(const PageRect& viewRect) const
{
    std::vector<int> out;
    if (viewRect.w <= 0 || viewRect.h <= 0)
        return out;

    if (m_mode == LayoutMode::SinglePage) {
        if (m_shown >= 0 && overlaps(m_rects[static_cast<size_t>(m_shown)], viewRect))
            out.push_back(m_shown);
        return out;
    }

    const int top = viewRect.y;
    const int bottom = viewRect.y + viewRect.h;
    const int left = viewRect.x;
    const int right = viewRect.x + viewRect.w;

    auto it = std::upper_bound(m_rects.begin(), m_rects.end(), top,
                               [](int v, const PageRect& r) { return v < r.y + r.h; });
    for (; it != m_rects.end() && it->y < bottom; ++it) {
        if (it->x < right && it->x + it->w > left && it->h > 0 && it->w > 0)
            out.push_back(static_cast<int>(it - m_rects.begin()));
    }
    return out;
}

PageRect PageLayout::pageToView(int page, const RectPts& rectPts) const
{
    if (page < 0 || page >= pageCount())
        return {0, 0, 0, 0};
    if (m_mode == LayoutMode::SinglePage && page != m_shown)
        return {0, 0, 0, 0};
    const PageRect& r = m_rects[static_cast<size_t>(page)];
    const auto edge = [this](int origin, double pts) {
        return origin + static_cast<int>(std::lround(pts * m_zoom));
    };
    const int left = edge(r.x, rectPts.x);
    const int top = edge(r.y, rectPts.y);
    const int right = edge(r.x, rectPts.x + rectPts.w);
    const int bottom = edge(r.y, rectPts.y + rectPts.h);
    return {left, top, right - left, bottom - top};
}

PagePoint PageLayout::viewToPage(ViewPoint point) const
{
    const int page = pageAt(point.y);
    if (page < 0)
        return {-1, {0, 0}};
    const PageRect& r = m_rects[static_cast<size_t>(page)];
    if (point.x < r.x || point.x >= r.x + r.w || m_zoom <= 0)
        return {-1, {0, 0}};
    return {page, {(point.x - r.x) / m_zoom, (point.y - r.y) / m_zoom}};
}
