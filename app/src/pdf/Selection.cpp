#include "pdf/Selection.h"

#include "pdf/TextLayer.h"

#include <utility>
#include <vector>

Selection::Selection(TextPos a, TextPos b) : m_start(a), m_end(b)
{
    if (m_end < m_start)
        std::swap(m_start, m_end);
}

std::string Selection::text(const TextLayer& layer) const
{
    std::string out;
    bool any = false;
    for (int page = m_start.page; page <= m_end.page; ++page) {
        const std::vector<TextBox>* boxes = layer.boxes(page);
        if (!boxes || boxes->empty())
            continue;
        const int last = static_cast<int>(boxes->size()) - 1;
        const int from = page == m_start.page ? m_start.boxIndex : 0;
        const int to = page == m_end.page ? m_end.boxIndex : last;
        if (to < 0 || from > last || from > to)
            continue;
        if (any)
            out += '\n';
        out += layer.textBetween(page, from, to);
        any = true;
    }
    return out;
}

SelectionInfo Selection::info(const TextLayer& layer) const
{
    return {text(layer), m_start.page, m_end.page};
}

Selection Selection::word(TextPos pos)
{
    return {pos, pos};
}

Selection Selection::line(const TextLayer& layer, TextPos pos)
{
    const std::vector<TextBox>* boxes = layer.boxes(pos.page);
    if (!boxes || pos.boxIndex < 0 || pos.boxIndex >= static_cast<int>(boxes->size()))
        return word(pos);

    const auto breaks = [&](int i) {
        // True when a line break separates box i from box i + 1.
        return TextLayer::separator((*boxes)[static_cast<std::size_t>(i)],
                                    (*boxes)[static_cast<std::size_t>(i) + 1]) == "\n";
    };
    int first = pos.boxIndex;
    while (first > 0 && !breaks(first - 1))
        --first;
    int last = pos.boxIndex;
    const int lastIndex = static_cast<int>(boxes->size()) - 1;
    while (last < lastIndex && !breaks(last))
        ++last;
    return {TextPos{pos.page, first}, TextPos{pos.page, last}};
}
