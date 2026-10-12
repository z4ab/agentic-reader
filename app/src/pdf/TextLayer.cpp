#include "pdf/TextLayer.h"

#include <algorithm>
#include <utility>

void TextLayer::setBoxes(int page, std::vector<TextBox> boxes)
{
    m_pages[page] = std::move(boxes);
}

bool TextLayer::hasPage(int page) const
{
    return m_pages.find(page) != m_pages.end();
}

const std::vector<TextBox>* TextLayer::boxes(int page) const
{
    const auto it = m_pages.find(page);
    return it == m_pages.end() ? nullptr : &it->second;
}

void TextLayer::clear()
{
    m_pages.clear();
}

int TextLayer::boxAt(int page, PointPts pointPts) const
{
    const std::vector<TextBox>* list = boxes(page);
    if (!list)
        return -1;
    for (std::size_t i = 0; i < list->size(); ++i) {
        const RectPts& r = (*list)[i].bbox;
        if (pointPts.x >= r.x && pointPts.x < r.x + r.w && pointPts.y >= r.y &&
            pointPts.y < r.y + r.h)
            return static_cast<int>(i);
    }
    return -1;
}

int TextLayer::nearestBox(int page, PointPts pointPts) const
{
    const std::vector<TextBox>* list = boxes(page);
    if (!list)
        return -1;
    int best = -1;
    double bestDistance = 0;
    for (std::size_t i = 0; i < list->size(); ++i) {
        const RectPts& r = (*list)[i].bbox;
        const double dx = std::max({r.x - pointPts.x, 0.0, pointPts.x - (r.x + r.w)});
        const double dy = std::max({r.y - pointPts.y, 0.0, pointPts.y - (r.y + r.h)});
        const double distance = dx * dx + dy * dy;
        if (best < 0 || distance < bestDistance) {
            best = static_cast<int>(i);
            bestDistance = distance;
        }
    }
    return best;
}

std::string TextLayer::textBetween(int page, int fromIdx, int toIdx) const
{
    const std::vector<TextBox>* list = boxes(page);
    if (!list || list->empty())
        return {};
    if (fromIdx > toIdx)
        std::swap(fromIdx, toIdx);
    const int last = static_cast<int>(list->size()) - 1;
    if (toIdx < 0 || fromIdx > last)
        return {};
    fromIdx = std::clamp(fromIdx, 0, last);
    toIdx = std::clamp(toIdx, 0, last);

    std::string out;
    for (int i = fromIdx; i <= toIdx; ++i) {
        const TextBox& box = (*list)[static_cast<std::size_t>(i)];
        out += box.text;
        if (i < toIdx)
            out += separator(box, (*list)[static_cast<std::size_t>(i) + 1]);
    }
    return out;
}

std::string_view TextLayer::separator(const TextBox& a, const TextBox& b)
{
    const double baseline = a.bbox.y + a.bbox.h;
    const double nextBaseline = b.bbox.y + b.bbox.h;
    if (nextBaseline - baseline > a.bbox.h / 2 || nextBaseline <= a.bbox.y)
        return "\n";
    return a.hasSpaceAfter ? " " : "";
}
