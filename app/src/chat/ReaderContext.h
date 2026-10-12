#pragma once

#include "agent/Protocol.h"
#include "pdf/Selection.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

class PdfDocument;
class TextLayer;

// What the user is reading, turned into a prompt's `context`, the context
// chip's text and the "Ask about selection" pre-fill. No wx types, UTF-8
// strings, never throws.
namespace readercontext
{

// What the PDF view shows at send time, read from PdfView on the UI thread.
struct Snapshot
{
    std::filesystem::path file;             // PdfView::filePath(), empty = no PDF open
    int currentPage = -1;                   // PdfView::currentPage(), 0-based, -1 = none
    int pageCount = 0;                      // PdfView::pageCount()
    std::optional<SelectionInfo> selection; // PdfView::selection()
};

// The prompt's context for `view`, std::nullopt when no PDF is open. The file
// is absolute with forward slashes, the page 1-based, and the selection whole
// (the sidecar cuts it), left out when `includeSelection` is false or there is
// no selected text. Unknown page or page count fields are left out.
std::optional<protocol::Context> build(const Snapshot& view, bool includeSelection);

// The chip text: "attention.pdf · p. 12 · 214 chars selected", without the
// parts that are unknown or excluded. "" when no PDF is open. Counts code
// points.
std::string chipLabel(const Snapshot& view, bool includeSelection);

// The chat input text that asks about `selection`: its first 300 code points
// without trailing newlines, each line quoted with "> ", then a blank line.
// "" for "".
std::string askPrefill(std::string_view selection);

// Sets the word boxes of every page from the selection's first to its last
// page that `layer` has not loaded, from `doc`. Pages already loaded are kept.
// Returns how many pages it loaded. Selection::text skips pages not loaded,
// which a long drag-scroll can leave behind. UI thread, slow on dense pages.
int loadSelectionPages(const Selection& selection, const PdfDocument& doc, TextLayer& layer);

// The chip's x: leaves the selection out of the next prompt only.
class ChipState
{
public:
    void excludeSelection() { m_excluded = true; }
    // Takes the x back, for a new selection or "Ask about selection".
    void includeSelection() { m_excluded = false; }
    bool selectionExcluded() const { return m_excluded; }

    std::string label(const Snapshot& view) const;
    // The context of the prompt being sent now. Ends the exclusion.
    std::optional<protocol::Context> takeForPrompt(const Snapshot& view);

private:
    bool m_excluded = false;
};

} // namespace readercontext
