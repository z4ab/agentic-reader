#pragma once

#include <compare>
#include <string>

class TextLayer;

// A word box in the document: 0-based page and the box's index in that page's
// TextLayer vector. Ordered by page, then box index (document order).
struct TextPos
{
    int page = 0;
    int boxIndex = 0;

    auto operator<=>(const TextPos&) const = default;
};

// What PdfView::selection() reports. Pages are 0-based.
struct SelectionInfo
{
    std::string text;
    int firstPage = 0;
    int lastPage = 0;
};

// A range of word boxes, both ends included. Never empty: "no selection" is an
// empty std::optional<Selection> in the owner.
class Selection
{
public:
    // Either order. Normalizes so start() <= end().
    Selection(TextPos a, TextPos b);

    TextPos start() const { return m_start; }
    TextPos end() const { return m_end; }
    bool contains(TextPos pos) const { return m_start <= pos && pos <= m_end; }

    // The selected words joined by TextLayer::separator within a page, and the
    // pages joined by "\n". Pages the layer has not loaded, or without words,
    // contribute nothing.
    std::string text(const TextLayer& layer) const;
    SelectionInfo info(const TextLayer& layer) const;

    // The single box at pos (double-click).
    static Selection word(TextPos pos);
    // The boxes around pos on its page with no line break between them
    // (triple-click). word(pos) for a page not loaded or an index out of range.
    static Selection line(const TextLayer& layer, TextPos pos);

private:
    TextPos m_start;
    TextPos m_end;
};
