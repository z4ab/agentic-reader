#pragma once

#include "pdf/PageLayout.h"
#include "pdf/PdfDocument.h"

#include <string>
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

    // Text of boxes fromIdx..toIdx, both included. A backward range is swapped and
    // indices are clamped to the page's boxes ("" when the range misses them). Boxes are joined with "\n" when the
    // next baseline is lower by more than half the line height, else with " " when
    // the box has a space after it. "" for a page not loaded.
    std::string textBetween(int page, int fromIdx, int toIdx) const;

private:
    std::unordered_map<int, std::vector<TextBox>> m_pages;
};
