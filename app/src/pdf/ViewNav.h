#pragma once

#include "pdf/PageLayout.h"

// Window-free zoom and navigation math for PdfView. Zoom is a scale factor
// (1.0 = 100%, 1 pt = 1 DIP), as in PageLayout::compute.

constexpr double kMinZoom = 0.25;
constexpr double kMaxZoom = 8.0;
// One Ctrl+wheel notch or one Ctrl+`+`/`-` press.
constexpr double kZoomStep = 0.10;

double clampZoom(double zoom);

// Adds steps * kZoomStep without snapping, then clamps.
double stepZoom(double zoom, int steps);

// Zoom that fits the page width, or the whole page, into the viewport (DIPs)
// with PageLayout::kGap on both sides. Clamped.
double fitWidthZoom(const PageSizePts& page, int viewportWidth);
double fitPageZoom(const PageSizePts& page, int viewportWidth, int viewportHeight);

// 0-based page with the largest overlap area with viewRect. Ties go to the lower
// page. -1 when no page overlaps.
int mostVisiblePage(const PageLayout& layout, const PageRect& viewRect);

// Scroll origin under `after` that puts the document point at client position
// `anchor` (under `before` with scroll origin `origin`) back at `anchor`.
// Offsets inside a page scale with the page. Offsets in the gaps and margins
// around it do not. The result is not clamped to the scroll range.
ViewPoint zoomScrollOrigin(const PageLayout& before, const PageLayout& after, ViewPoint origin,
                           ViewPoint anchor);

// Single Page wheel: decides, per wheel event, whether to flip a page, pan the
// overflowing page, or do nothing. Holds the accumulated rotation and the time
// of the last flip. Times are monotonic milliseconds.
struct WheelFlip
{
    struct Step
    {
        int flip = 0;     // -1 previous page, +1 next page, 0 no flip
        bool pan = false; // true: the wheel pans the overflowing page instead
    };

    // rotation is one event's GetWheelRotation() (-120 is one notch down), wheelDelta
    // is GetWheelDelta(). Wheel down (rotation < 0) is forward.
    // An event within 150 ms of the last flip is ignored and does not accumulate.
    Step onWheel(int rotation, int wheelDelta, long long nowMs, bool pageFitsVertically,
                 bool atTopEdge, bool atBottomEdge, int page, int pageCount);

private:
    int m_accumulated = 0;
    bool m_hasFlipped = false;
    long long m_lastFlipMs = 0;
};
