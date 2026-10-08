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

// Pure layout math for a continuous vertical page stack. Page indices are 0-based.
class PageLayout
{
public:
    // Gap above the first page, between pages and below the last page.
    static constexpr int kGap = 12;

    // dpiScale affects only render resolution, never the layout rects.
    void compute(const std::vector<PageSizePts>& sizes, double zoom, int viewportWidth,
                 double dpiScale);

    int pageCount() const { return static_cast<int>(m_rects.size()); }
    PageRect pageRect(int page) const;
    int totalWidth() const { return m_totalWidth; }
    int totalHeight() const { return m_totalHeight; }

    // Page containing y, or -1 for a gap, margin or outside the layout.
    int pageAt(int y) const;

    // Ascending indices of pages overlapping viewRect with positive area.
    std::vector<int> visiblePages(const PageRect& viewRect) const;

private:
    std::vector<PageRect> m_rects;
    int m_totalWidth = 0;
    int m_totalHeight = kGap;
};
