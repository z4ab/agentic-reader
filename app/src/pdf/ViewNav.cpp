#include "pdf/ViewNav.h"

#include <algorithm>
#include <cmath>

namespace {

// Maps v from the span [a0, a0 + len0) to [a1, a1 + len1). Inside the span the
// offset scales. Outside it the distance to the nearest edge stays the same.
int mapAxis(int v, int a0, int len0, int a1, int len1)
{
    if (v < a0)
        return a1 - (a0 - v);
    if (v >= a0 + len0 || len0 <= 0)
        return a1 + len1 + (v - a0 - len0);
    return a1 + static_cast<int>(std::lround((v - a0) * static_cast<double>(len1) / len0));
}

// Page containing y, else the first page below y, else the last page.
int pageNear(const PageLayout& layout, int y)
{
    int lo = 0;
    int hi = layout.pageCount();
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        const PageRect r = layout.pageRect(mid);
        if (y < r.y + r.h)
            hi = mid;
        else
            lo = mid + 1;
    }
    return std::min(lo, layout.pageCount() - 1);
}

} // namespace

double clampZoom(double zoom)
{
    return std::clamp(zoom, kMinZoom, kMaxZoom);
}

double stepZoom(double zoom, int steps)
{
    return clampZoom(zoom + steps * kZoomStep);
}

double fitWidthZoom(const PageSizePts& page, int viewportWidth)
{
    return clampZoom((viewportWidth - 2.0 * PageLayout::kGap) / page.w);
}

double fitPageZoom(const PageSizePts& page, int viewportWidth, int viewportHeight)
{
    return clampZoom(std::min((viewportWidth - 2.0 * PageLayout::kGap) / page.w,
                              (viewportHeight - 2.0 * PageLayout::kGap) / page.h));
}

int mostVisiblePage(const PageLayout& layout, const PageRect& viewRect)
{
    int best = -1;
    long long bestArea = 0;
    for (int page : layout.visiblePages(viewRect)) {
        const PageRect r = layout.pageRect(page);
        const long long w = std::min(r.x + r.w, viewRect.x + viewRect.w) - std::max(r.x, viewRect.x);
        const long long h = std::min(r.y + r.h, viewRect.y + viewRect.h) - std::max(r.y, viewRect.y);
        if (w > 0 && h > 0 && w * h > bestArea) {
            best = page;
            bestArea = w * h;
        }
    }
    return best;
}

ViewPoint zoomScrollOrigin(const PageLayout& before, const PageLayout& after, ViewPoint origin,
                           ViewPoint anchor)
{
    if (before.pageCount() == 0 || before.pageCount() != after.pageCount())
        return origin;

    const int x = origin.x + anchor.x;
    const int y = origin.y + anchor.y;
    const int page = pageNear(before, y);
    const PageRect r0 = before.pageRect(page);
    const PageRect r1 = after.pageRect(page);
    return {mapAxis(x, r0.x, r0.w, r1.x, r1.w) - anchor.x,
            mapAxis(y, r0.y, r0.h, r1.y, r1.h) - anchor.y};
}
