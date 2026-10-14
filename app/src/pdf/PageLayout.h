#pragma once

#include <vector>

// Page size in PDF points.
struct PageSizePts
{
    double w;
    double h;
};

// Rect in logical (DIP) virtual pixels. Half-open: [x, x + w) x [y, y + h).
struct PageRect
{
    int x;
    int y;
    int w;
    int h;
};

// Point in logical (DIP) virtual pixels, the same space as PageRect.
struct ViewPoint
{
    int x;
    int y;
};

// Point in page points: origin at the top-left of the page as displayed (after
// /Rotate), y grows downward. The convention of poppler's text_list().
struct PointPts
{
    double x;
    double y;
};

// Rect in page points, same convention as PointPts.
struct RectPts
{
    double x;
    double y;
    double w;
    double h;
};

// A point on a page. page is -1 when the point is on no page, and pt is then unspecified.
struct PagePoint
{
    int page;
    PointPts pt;
};

// Continuous: pages stacked vertically. SinglePage: only the shown page has a rect.
enum class LayoutMode { Continuous, SinglePage };

// Pure layout math for a page layout. Page indices are 0-based.
class PageLayout
{
public:
    // Gap above the first page, between pages and below the last page.
    static constexpr int kGap = 12;

    // dpiScale affects only render resolution, never the layout rects.
    // shownPage and viewportHeight apply only to SinglePage. viewportHeight 0
    // means the page plus margins.
    void compute(const std::vector<PageSizePts>& sizes, double zoom, int viewportWidth,
                 double dpiScale, LayoutMode mode = LayoutMode::Continuous, int shownPage = 0,
                 int viewportHeight = 0);

    int pageCount() const { return static_cast<int>(m_rects.size()); }
    PageRect pageRect(int page) const;
    int totalWidth() const { return m_totalWidth; }
    int totalHeight() const { return m_totalHeight; }

    // Page containing y, or -1 for a gap, margin or outside the layout.
    int pageAt(int y) const;

    // Ascending indices of pages overlapping viewRect with positive area.
    std::vector<int> visiblePages(const PageRect& viewRect) const;

    // Page points to virtual DIP pixels: pageRect(page) origin + pts * zoom, each
    // edge rounded to the nearest pixel. The scroll offset is not applied, and the
    // HiDPI scale does not enter (view space is DIP). {0, 0, 0, 0} for an invalid page.
    PageRect pageToView(int page, const RectPts& rectPts) const;

    // Inverse of pageToView for a point. Page -1 when the point is in no page rect.
    PagePoint viewToPage(ViewPoint point) const;

private:
    std::vector<PageRect> m_rects;
    LayoutMode m_mode = LayoutMode::Continuous;
    int m_shown = -1; // SinglePage only: the shown page, or -1 when none.
    double m_zoom = 1.0;
    int m_totalWidth = 0;
    int m_totalHeight = kGap;
};
