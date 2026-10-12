#pragma once

#include "pdf/PageLayout.h"
#include "pdf/PdfDocument.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Per-page cache of word boxes, filled from render results (RenderResult::textBoxes).
// It never loads text itself. Page indices are 0-based, points are page points.
// UI thread only.
class TextLayer
{
public:
    // Stores the boxes of `page`, replacing any it had. An empty vector marks a
    // loaded page without text.
    void setBoxes(int page, std::vector<TextBox> boxes);
    bool hasPage(int page) const;
    // nullptr when the page is not loaded.
    const std::vector<TextBox>* boxes(int page) const;
    void clear();

    // Index of the box whose bbox contains the point, or -1 for no box or a page
    // not loaded.
    int boxAt(int page, PointPts pointPts) const;
    // Index of the box closest to the point (0 distance inside a box), or -1 for a
    // page not loaded or without boxes.
    int nearestBox(int page, PointPts pointPts) const;

    // Text of boxes fromIdx..toIdx, both included, joined by separator(). A
    // backward range is swapped and indices are clamped to the page's boxes ("" when
    // the range misses them). "" for a page not loaded.
    std::string textBetween(int page, int fromIdx, int toIdx) const;

    // What goes between box a and the next box b of the same page: "\n" when b's
    // baseline is lower than a's by more than half a's height, or when b lies
    // entirely above a (the jump from the bottom of one column to the top of the
    // next). Otherwise " " when a has a space after it, else "".
    static std::string_view separator(const TextBox& a, const TextBox& b);

private:
    std::unordered_map<int, std::vector<TextBox>> m_pages;
};
