#include "chat/ReaderContext.h"

#include "pdf/PdfDocument.h"
#include "pdf/TextLayer.h"

#include <system_error>

namespace fs = std::filesystem;

namespace readercontext
{

namespace
{

constexpr std::size_t kPrefillChars = 300;
const char* const kSeparator = " \xC2\xB7 "; // " · "

std::string fromU8(const std::u8string& s)
{
    return std::string(s.begin(), s.end());
}

bool isContinuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

std::size_t codePoints(std::string_view s)
{
    std::size_t n = 0;
    for (const char c : s)
        if (!isContinuation(c))
            ++n;
    return n;
}

const std::string* selectedText(const Snapshot& view, bool includeSelection)
{
    if (!includeSelection || !view.selection || view.selection->text.empty())
        return nullptr;
    return &view.selection->text;
}

} // namespace

std::optional<protocol::Context> build(const Snapshot& view, bool includeSelection)
{
    if (view.file.empty())
        return std::nullopt;
    std::error_code ec;
    fs::path file = fs::absolute(view.file, ec);
    if (ec)
        file = view.file;

    protocol::Context context;
    context.file = fromU8(file.generic_u8string());
    if (view.currentPage >= 0)
        context.page = view.currentPage + 1;
    if (view.pageCount > 0)
        context.pageCount = view.pageCount;
    if (const std::string* text = selectedText(view, includeSelection))
        context.selection = *text;
    return context;
}

std::string chipLabel(const Snapshot& view, bool includeSelection)
{
    if (view.file.empty())
        return {};
    std::string label = fromU8(view.file.filename().u8string());
    if (view.currentPage >= 0)
        label += kSeparator + std::string("p. ") + std::to_string(view.currentPage + 1);
    if (const std::string* text = selectedText(view, includeSelection)) {
        const std::size_t n = codePoints(*text);
        label += kSeparator + std::to_string(n) + (n == 1 ? " char selected" : " chars selected");
    }
    return label;
}

std::string askPrefill(std::string_view selection)
{
    // The first kPrefillChars code points, whole.
    std::size_t end = 0;
    std::size_t chars = 0;
    while (end < selection.size()) {
        if (!isContinuation(selection[end]) && chars++ == kPrefillChars)
            break;
        ++end;
    }
    std::string_view cut = selection.substr(0, end);
    while (!cut.empty() && cut.back() == '\n')
        cut.remove_suffix(1);
    if (cut.empty())
        return {};

    std::string out;
    out.reserve(cut.size() + cut.size() / 16 + 8);
    std::size_t start = 0;
    while (true) {
        const std::size_t newline = cut.find('\n', start);
        out += "> ";
        out += cut.substr(start, newline == std::string_view::npos ? cut.npos : newline - start);
        out += '\n';
        if (newline == std::string_view::npos)
            break;
        start = newline + 1;
    }
    out += '\n';
    return out;
}

int loadSelectionPages(const Selection& selection, const PdfDocument& doc, TextLayer& layer)
{
    int loaded = 0;
    for (int page = selection.start().page; page <= selection.end().page; ++page) {
        if (layer.hasPage(page))
            continue;
        layer.setBoxes(page, doc.textBoxes(page));
        ++loaded;
    }
    return loaded;
}

std::string ChipState::label(const Snapshot& view) const
{
    return chipLabel(view, !m_excluded);
}

std::optional<protocol::Context> ChipState::takeForPrompt(const Snapshot& view)
{
    const bool include = !m_excluded;
    m_excluded = false;
    return build(view, include);
}

} // namespace readercontext
