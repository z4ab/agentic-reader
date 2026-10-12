// M5.3: spec tests for the reader context of a prompt, the context chip's label
// and its x, and the "Ask about selection" pre-fill. The GUI (the chip widget,
// the PDF context menu entry, Ctrl+Shift+A, focus) is checked by
// tests/acceptance/m5.3.sh, since a GUI test needs a display that ctest lacks.
//
// The spec names PdfView's getters and protocol::Context but no function that
// joins them. Assumed interface, app/src/chat/ReaderContext.h (no wx types,
// UTF-8 strings, never throws):
//
//   #include "agent/Protocol.h"
//   #include "pdf/Selection.h"
//
//   #include <filesystem>
//   #include <optional>
//   #include <string>
//   #include <string_view>
//
//   class PdfDocument;
//   class TextLayer;
//
//   namespace readercontext
//   {
//   // What the PDF view shows at send time, read from PdfView on the UI thread.
//   struct Snapshot
//   {
//       std::filesystem::path file;              // PdfView::filePath(), empty = no PDF open
//       int currentPage = -1;                    // PdfView::currentPage(), 0-based, -1 = none
//       int pageCount = 0;                       // PdfView::pageCount()
//       std::optional<SelectionInfo> selection;  // PdfView::selection()
//   };
//
//   // prompt.context for `view` (spec 1, 4). std::nullopt when no PDF is open
//   // (`file` empty), whatever the other fields hold. Otherwise:
//   //   file        absolute (a relative path is made absolute against the
//   //               current directory) with forward slashes, UTF-8
//   //               (path::generic_u8string), per the carried M4.4 note.
//   //   page        currentPage + 1 (1-based, sidecar/PROTOCOL.md), nullopt
//   //               when currentPage < 0.
//   //   pageCount   pageCount, nullopt when it is 0 or less.
//   //   selection   the whole selected text, never cut (the sidecar cuts at
//   //               8,000 chars), when `includeSelection` is true and the
//   //               selection exists with non-empty text. Otherwise nullopt,
//   //               never "".
//   std::optional<protocol::Context> build(const Snapshot& view, bool includeSelection);
//
//   // The chip text (spec 2), parts joined by " · " (U+00B7 with a space on
//   // each side):
//   //   "attention.pdf · p. 12 · 214 chars selected"
//   //   "attention.pdf · p. 12 · 1 char selected"      (singular)
//   //   "attention.pdf · p. 12"                        (no selection, empty
//   //                                                   selection text, or
//   //                                                   includeSelection false)
//   //   "attention.pdf"                                (currentPage < 0)
//   //   ""                                             (no PDF open: no chip)
//   // The name is file.filename() in UTF-8. The page is 1-based. The count is
//   // the number of Unicode code points of the selected text (not bytes, not
//   // grapheme clusters), newlines included, in plain digits without
//   // thousands separators.
//   std::string chipLabel(const Snapshot& view, bool includeSelection);
//
//   // The text "Ask about selection" puts in the chat input (spec 3): the
//   // first 300 code points of `selection` (a cut never splits a UTF-8
//   // sequence and adds no marker), trailing "\n"s of that cut dropped, each
//   // line prefixed with "> ", lines joined by "\n", then "\n\n" so the input
//   // ends with a blank line and the caret goes after it. "" for "".
//   std::string askPrefill(std::string_view selection);
//
//   // Carried M3.2 note. Selection::text skips pages the TextLayer has not
//   // loaded (a long drag-scroll). Before the selection goes to the chat, the
//   // caller fills in the missing pages: for every page from
//   // selection.start().page to selection.end().page that `layer` lacks, sets
//   // that page's boxes from doc.textBoxes(page). Pages already loaded are not
//   // replaced. Returns how many pages it loaded. PdfView calls it on the UI
//   // thread with its own document (the render thread opens its own copy), so
//   // the context always carries the whole selection rather than a flag.
//   int loadSelectionPages(const Selection& selection, const PdfDocument& doc, TextLayer& layer);
//
//   // The chip's x: leaves the selection out of the next prompt only.
//   class ChipState
//   {
//   public:
//       void excludeSelection();          // the x on the chip
//       bool selectionExcluded() const;
//       // chipLabel(view, !selectionExcluded()).
//       std::string label(const Snapshot& view) const;
//       // build(view, !selectionExcluded()) for the prompt being sent now. Ends
//       // the exclusion, also when it returns nullopt or no selection existed.
//       std::optional<protocol::Context> takeForPrompt(const Snapshot& view);
//   };
//   }
//
// Not checked here: whether a new selection made after the x ends the
// exclusion early (implementer's choice), and the order of fields in the JSON.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "agent/Protocol.h"
#include "chat/ReaderContext.h"
#include "pdf/PdfDocument.h"
#include "pdf/Selection.h"
#include "pdf/TextLayer.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef TEST_DATA_DIR
#error "TEST_DATA_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace fs = std::filesystem;
using readercontext::askPrefill;
using readercontext::build;
using readercontext::ChipState;
using readercontext::chipLabel;
using readercontext::loadSelectionPages;
using readercontext::Snapshot;

namespace {

const std::string kDot = "\xC2\xB7"; // U+00B7 MIDDLE DOT

#ifdef _WIN32
const fs::path kAbsFile = fs::path(L"C:\\Users\\u\\papers\\attention.pdf");
const std::string kAbsFileText = "C:/Users/u/papers/attention.pdf";
#else
const fs::path kAbsFile = fs::path("/home/u/papers/attention.pdf");
const std::string kAbsFileText = "/home/u/papers/attention.pdf";
#endif

SelectionInfo sel(std::string text, int first = 11, int last = 11)
{
    return SelectionInfo{std::move(text), first, last};
}

// attention.pdf open at page 12 of 15 (0-based 11).
Snapshot view(std::optional<SelectionInfo> selection = std::nullopt)
{
    Snapshot v;
    v.file = kAbsFile;
    v.currentPage = 11;
    v.pageCount = 15;
    v.selection = std::move(selection);
    return v;
}

std::string repeat(std::string_view piece, int n)
{
    std::string s;
    for (int i = 0; i < n; ++i)
        s += piece;
    return s;
}

// Whether `s` is valid UTF-8.
bool validUtf8(const std::string& s)
{
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size())
            return false;
        for (int k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        i += len;
    }
    return true;
}

std::string u8(const std::u8string& s)
{
    return std::string(s.begin(), s.end());
}

std::unique_ptr<PdfDocument> openSample()
{
    std::string error;
    auto doc = PdfDocument::open(fs::path(TEST_DATA_DIR) / "sample.pdf", {}, error);
    REQUIRE_MESSAGE(doc != nullptr, "open failed: " << error);
    return doc;
}

const std::string kSelection = "Scaled dot-product attention";

} // namespace

// ---------------------------------------------------------------------------
// Interface checks

static_assert(std::is_same_v<decltype(Snapshot::file), fs::path>);
static_assert(std::is_same_v<decltype(Snapshot::currentPage), int>);
static_assert(std::is_same_v<decltype(Snapshot::pageCount), int>);
static_assert(std::is_same_v<decltype(Snapshot::selection), std::optional<SelectionInfo>>);
static_assert(std::is_same_v<decltype(build(std::declval<const Snapshot&>(), true)),
                             std::optional<protocol::Context>>);
static_assert(std::is_same_v<decltype(chipLabel(std::declval<const Snapshot&>(), true)), std::string>);
static_assert(std::is_same_v<decltype(askPrefill(std::string_view())), std::string>);
static_assert(std::is_same_v<decltype(loadSelectionPages(std::declval<const Selection&>(),
                                                         std::declval<const PdfDocument&>(),
                                                         std::declval<TextLayer&>())),
                             int>);

TEST_CASE("reader_context a default Snapshot is no PDF open")
{
    const Snapshot v;
    CHECK(v.file.empty());
    CHECK(v.currentPage == -1);
    CHECK(v.pageCount == 0);
    CHECK_FALSE(v.selection.has_value());
}

// ---------------------------------------------------------------------------
// Spec 1: prompt.context from the file, page, page count and selection

TEST_CASE("reader_context build fills every field")
{
    const auto ctx = build(view(sel(kSelection)), true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->file == std::optional<std::string>(kAbsFileText));
    CHECK(ctx->page == std::optional<int>(12));
    CHECK(ctx->pageCount == std::optional<int>(15));
    CHECK(ctx->selection == std::optional<std::string>(kSelection));
}

TEST_CASE("reader_context page is 1-based")
{
    Snapshot v = view();
    v.currentPage = 0;
    v.pageCount = 1;
    const auto ctx = build(v, true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->page == std::optional<int>(1));
    CHECK(ctx->pageCount == std::optional<int>(1));
}

TEST_CASE("reader_context without a selection has no selection field")
{
    const auto ctx = build(view(), true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->file == std::optional<std::string>(kAbsFileText));
    CHECK(ctx->page == std::optional<int>(12));
    CHECK(ctx->pageCount == std::optional<int>(15));
    CHECK_FALSE(ctx->selection.has_value());
}

TEST_CASE("reader_context an empty selection text counts as no selection")
{
    const auto ctx = build(view(sel("")), true);
    REQUIRE(ctx.has_value());
    CHECK_FALSE(ctx->selection.has_value());
    CHECK(ctx->file.has_value());
}

TEST_CASE("reader_context includeSelection false leaves out only the selection")
{
    const auto ctx = build(view(sel(kSelection)), false);
    REQUIRE(ctx.has_value());
    CHECK(ctx->file == std::optional<std::string>(kAbsFileText));
    CHECK(ctx->page == std::optional<int>(12));
    CHECK(ctx->pageCount == std::optional<int>(15));
    CHECK_FALSE(ctx->selection.has_value());
}

TEST_CASE("reader_context the selection travels whole, never cut")
{
    // Longer than the 300-char pre-fill and the sidecar's 8,000-char limit.
    const std::string longText = repeat("All work and no play. ", 999) + "\nLast line, caf\xC3\xA9.";
    const auto ctx = build(view(sel(longText, 3, 9)), true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->selection == std::optional<std::string>(longText));
}

TEST_CASE("reader_context a multi-line selection keeps its newlines")
{
    const std::string text = "first line\nsecond line\nthird";
    const auto ctx = build(view(sel(text)), true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->selection == std::optional<std::string>(text));
}

TEST_CASE("reader_context a page or page count not known is left out")
{
    Snapshot v = view(sel(kSelection));
    v.currentPage = -1;
    v.pageCount = 0;
    const auto ctx = build(v, true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->file == std::optional<std::string>(kAbsFileText));
    CHECK_FALSE(ctx->page.has_value());
    CHECK_FALSE(ctx->pageCount.has_value());
    CHECK(ctx->selection == std::optional<std::string>(kSelection));
}

// Carried M4.4 note: no relative path and no backslash reaches the prompt.
TEST_CASE("reader_context a relative file becomes absolute with forward slashes")
{
    Snapshot v = view();
    v.file = fs::path("papers") / "attention.pdf";
    const auto ctx = build(v, true);
    REQUIRE(ctx.has_value());
    REQUIRE(ctx->file.has_value());
    const std::string file = *ctx->file;
    const std::string expected = u8((fs::current_path() / "papers" / "attention.pdf").generic_u8string());
    CHECK(file == expected);
    CHECK(fs::path(file).is_absolute());
    CHECK(file.find('\\') == std::string::npos);
    CHECK(file.ends_with("/papers/attention.pdf"));
}

TEST_CASE("reader_context an absolute file is sent with forward slashes only")
{
    const auto ctx = build(view(), true);
    REQUIRE(ctx.has_value());
    REQUIRE(ctx->file.has_value());
    CHECK(*ctx->file == kAbsFileText);
    CHECK(ctx->file->find('\\') == std::string::npos);
}

TEST_CASE("reader_context a non-ASCII file name is sent as UTF-8")
{
    Snapshot v = view();
    v.file = kAbsFile.parent_path() / fs::path(u8"Caf\u00e9 na\u00efve.pdf");
    const auto ctx = build(v, true);
    REQUIRE(ctx.has_value());
    REQUIRE(ctx->file.has_value());
    CHECK(ctx->file->ends_with("/Caf\xC3\xA9 na\xC3\xAFve.pdf"));
    CHECK(validUtf8(*ctx->file));
}

// ---------------------------------------------------------------------------
// Spec 4: no PDF open, no context

TEST_CASE("reader_context no PDF open sends no context")
{
    CHECK_FALSE(build(Snapshot{}, true).has_value());
    CHECK_FALSE(build(Snapshot{}, false).has_value());

    // Even if the other fields were left over.
    Snapshot v = view(sel(kSelection));
    v.file.clear();
    CHECK_FALSE(build(v, true).has_value());
}

TEST_CASE("reader_context a prompt without context has no context key in its JSON")
{
    const protocol::Inbound msg = protocol::Prompt{"c1", "hi", build(Snapshot{}, true), std::nullopt};
    const nlohmann::json j = protocol::toJson(msg);
    CHECK_FALSE(j.contains("context"));
}

TEST_CASE("reader_context a prompt with context serializes the protocol fields")
{
    const protocol::Inbound msg =
        protocol::Prompt{"c1", "hi", build(view(sel(kSelection)), true), std::nullopt};
    const nlohmann::json j = protocol::toJson(msg);
    REQUIRE(j.contains("context"));
    const nlohmann::json& c = j["context"];
    CHECK(c["file"] == kAbsFileText);
    CHECK(c["page"] == 12);
    CHECK(c["page_count"] == 15);
    CHECK(c["selection"] == kSelection);
}

TEST_CASE("reader_context a context without a selection omits the selection key")
{
    const protocol::Inbound msg = protocol::Prompt{"c1", "hi", build(view(sel(kSelection)), false), std::nullopt};
    const nlohmann::json j = protocol::toJson(msg);
    REQUIRE(j.contains("context"));
    CHECK_FALSE(j["context"].contains("selection"));
    CHECK(j["context"]["page"] == 12);
}

// ---------------------------------------------------------------------------
// Spec 2: the chip label

TEST_CASE("reader_context chip label with a selection")
{
    const std::string text = repeat("x", 214);
    CHECK(chipLabel(view(sel(text)), true) == "attention.pdf " + kDot + " p. 12 " + kDot + " 214 chars selected");
}

TEST_CASE("reader_context chip label for one char is singular")
{
    CHECK(chipLabel(view(sel("a")), true) == "attention.pdf " + kDot + " p. 12 " + kDot + " 1 char selected");
}

TEST_CASE("reader_context chip label for two chars is plural")
{
    CHECK(chipLabel(view(sel("ab")), true) == "attention.pdf " + kDot + " p. 12 " + kDot + " 2 chars selected");
}

TEST_CASE("reader_context chip label without a selection")
{
    const std::string expected = "attention.pdf " + kDot + " p. 12";
    CHECK(chipLabel(view(), true) == expected);
    CHECK(chipLabel(view(sel("")), true) == expected);
    CHECK(chipLabel(view(sel(kSelection)), false) == expected);
}

TEST_CASE("reader_context chip label counts code points, newlines included")
{
    // "Größe €5" is 8 code points in 11 bytes; with "\n" and "naïve" 14.
    const std::string text = "Gr\xC3\xB6\xC3\x9F" "e \xE2\x82\xAC" "5\nna\xC3\xAFve";
    CHECK(chipLabel(view(sel(text)), true) == "attention.pdf " + kDot + " p. 12 " + kDot + " 14 chars selected");
    // A 4-byte code point counts once.
    CHECK(chipLabel(view(sel("\xF0\x9F\x98\x80")), true)
          == "attention.pdf " + kDot + " p. 12 " + kDot + " 1 char selected");
}

TEST_CASE("reader_context chip label numbers have no thousands separator")
{
    CHECK(chipLabel(view(sel(repeat("y", 12345))), true)
          == "attention.pdf " + kDot + " p. 12 " + kDot + " 12345 chars selected");
}

TEST_CASE("reader_context chip label shows the 1-based current page and only the file name")
{
    Snapshot v = view();
    v.currentPage = 0;
    CHECK(chipLabel(v, true) == "attention.pdf " + kDot + " p. 1");
    v.file = fs::path("papers") / "deep" / "notes.pdf";
    v.currentPage = 2;
    CHECK(chipLabel(v, true) == "notes.pdf " + kDot + " p. 3");
}

TEST_CASE("reader_context chip label with a non-ASCII file name")
{
    Snapshot v = view();
    v.file = kAbsFile.parent_path() / fs::path(u8"Caf\u00e9.pdf");
    CHECK(chipLabel(v, true) == "Caf\xC3\xA9.pdf " + kDot + " p. 12");
}

TEST_CASE("reader_context chip label without a known page")
{
    Snapshot v = view(sel("abc"));
    v.currentPage = -1;
    CHECK(chipLabel(v, true) == "attention.pdf " + kDot + " 3 chars selected");
    v.selection.reset();
    CHECK(chipLabel(v, true) == "attention.pdf");
}

TEST_CASE("reader_context chip label is empty with no PDF open")
{
    CHECK(chipLabel(Snapshot{}, true) == "");
    Snapshot v = view(sel(kSelection));
    v.file.clear();
    CHECK(chipLabel(v, true) == "");
}

// ---------------------------------------------------------------------------
// Spec 2: the x excludes the selection from the next prompt only

TEST_CASE("reader_context the chip starts with the selection included")
{
    ChipState chip;
    CHECK_FALSE(chip.selectionExcluded());
    CHECK(chip.label(view(sel("abc"))) == "attention.pdf " + kDot + " p. 12 " + kDot + " 3 chars selected");
    const auto ctx = chip.takeForPrompt(view(sel("abc")));
    REQUIRE(ctx.has_value());
    CHECK(ctx->selection == std::optional<std::string>("abc"));
}

TEST_CASE("reader_context x leaves the selection out of the next prompt only")
{
    ChipState chip;
    const Snapshot v = view(sel(kSelection));
    chip.excludeSelection();
    CHECK(chip.selectionExcluded());
    CHECK(chip.label(v) == "attention.pdf " + kDot + " p. 12");

    const auto next = chip.takeForPrompt(v);
    REQUIRE(next.has_value());
    CHECK_FALSE(next->selection.has_value());
    CHECK(next->file == std::optional<std::string>(kAbsFileText));
    CHECK(next->page == std::optional<int>(12));
    CHECK(next->pageCount == std::optional<int>(15));

    CHECK_FALSE(chip.selectionExcluded());
    CHECK(chip.label(v) == "attention.pdf " + kDot + " p. 12 " + kDot + " 28 chars selected");
    const auto after = chip.takeForPrompt(v);
    REQUIRE(after.has_value());
    CHECK(after->selection == std::optional<std::string>(kSelection));
}

TEST_CASE("reader_context x twice before a prompt still covers one prompt")
{
    ChipState chip;
    const Snapshot v = view(sel(kSelection));
    chip.excludeSelection();
    chip.excludeSelection();
    CHECK_FALSE(chip.takeForPrompt(v)->selection.has_value());
    CHECK(chip.takeForPrompt(v)->selection == std::optional<std::string>(kSelection));
}

TEST_CASE("reader_context the exclusion ends with the next prompt even without a PDF")
{
    ChipState chip;
    chip.excludeSelection();
    CHECK_FALSE(chip.takeForPrompt(Snapshot{}).has_value());
    CHECK_FALSE(chip.selectionExcluded());
    CHECK(chip.takeForPrompt(view(sel(kSelection)))->selection == std::optional<std::string>(kSelection));
}

TEST_CASE("reader_context the exclusion ends with the next prompt even without a selection")
{
    ChipState chip;
    chip.excludeSelection();
    const auto ctx = chip.takeForPrompt(view());
    REQUIRE(ctx.has_value());
    CHECK_FALSE(ctx->selection.has_value());
    CHECK_FALSE(chip.selectionExcluded());
}

TEST_CASE("reader_context the chip label is empty with no PDF open")
{
    ChipState chip;
    CHECK(chip.label(Snapshot{}) == "");
    chip.excludeSelection();
    CHECK(chip.label(Snapshot{}) == "");
}

// ---------------------------------------------------------------------------
// Spec 3: the "Ask about selection" pre-fill

TEST_CASE("reader_context pre-fill quotes the selection and ends with a blank line")
{
    CHECK(askPrefill(kSelection) == "> Scaled dot-product attention\n\n");
}

TEST_CASE("reader_context pre-fill quotes every line")
{
    CHECK(askPrefill("first line\nsecond line\nthird") == "> first line\n> second line\n> third\n\n");
}

TEST_CASE("reader_context pre-fill of an empty selection is empty")
{
    CHECK(askPrefill("") == "");
}

TEST_CASE("reader_context pre-fill keeps 300 chars exactly")
{
    const std::string text300 = repeat("a", 300);
    CHECK(askPrefill(text300) == "> " + text300 + "\n\n");
    CHECK(askPrefill(text300 + "b") == "> " + text300 + "\n\n");
    CHECK(askPrefill(text300 + repeat("b", 5000)) == "> " + text300 + "\n\n");
}

TEST_CASE("reader_context pre-fill counts newlines as chars and cuts before quoting")
{
    // 100 lines "ab\n" are 300 chars: the cut ends on a "\n", which is dropped.
    const std::string text = repeat("ab\n", 100) + "cd";
    CHECK(askPrefill(text) == repeat("> ab\n", 99) + "> ab\n\n");

    // 299 chars and a newline: no empty quoted line at the end.
    CHECK(askPrefill(repeat("a", 299) + "\nmore") == "> " + repeat("a", 299) + "\n\n");

    // The cut can end mid-line: lines of 99, 99 and 98 chars and 3 newlines
    // are 299 chars, then 1 char of the fourth line.
    const std::string line = repeat("z", 99);
    const std::string shorter = repeat("z", 98);
    CHECK(askPrefill(line + "\n" + line + "\n" + shorter + "\n" + "QRS")
          == "> " + line + "\n> " + line + "\n> " + shorter + "\n> Q\n\n");
}

TEST_CASE("reader_context pre-fill counts code points and never splits one")
{
    // 400 "é" (2 bytes each): 300 of them, 600 bytes.
    const std::string e = "\xC3\xA9";
    CHECK(askPrefill(repeat(e, 400)) == "> " + repeat(e, 300) + "\n\n");

    // The 300th char is a 3-byte "€": kept whole. The 301st is not kept.
    const std::string euro = "\xE2\x82\xAC";
    CHECK(askPrefill(repeat("a", 299) + euro + euro + "tail") == "> " + repeat("a", 299) + euro + "\n\n");

    // A 4-byte emoji straddling the byte position 300 is kept whole.
    const std::string emoji = "\xF0\x9F\x98\x80";
    const std::string out = askPrefill(repeat("a", 298) + emoji + "bc" + repeat("d", 50));
    CHECK(out == "> " + repeat("a", 298) + emoji + "b\n\n");
    CHECK(validUtf8(out));

    // 150 emoji (600 bytes) are under 300 chars: all kept.
    const std::string emojis = repeat(emoji, 150);
    CHECK(askPrefill(emojis) == "> " + emojis + "\n\n");
}

TEST_CASE("reader_context pre-fill of a short multi-line non-ASCII selection")
{
    CHECK(askPrefill("Caf\xC3\xA9 na\xC3\xAFve\nGr\xC3\xB6\xC3\x9F" "e \xE2\x82\xAC" "5")
          == "> Caf\xC3\xA9 na\xC3\xAFve\n> Gr\xC3\xB6\xC3\x9F" "e \xE2\x82\xAC" "5\n\n");
}

// ---------------------------------------------------------------------------
// Carried M3.2 note: a selection over pages whose text is not loaded yet

TEST_CASE("reader_context missing pages of a selection are loaded before it is sent")
{
    auto doc = openSample();
    TextLayer layer;
    layer.setBoxes(0, doc->textBoxes(0));
    layer.setBoxes(2, doc->textBoxes(2));

    // From "word:" on page 0 to "Three" on page 2. Page 1 (the middle of a
    // drag-scroll) never rendered, so its text is missing.
    const Selection s(TextPos{0, 27}, TextPos{2, 1});
    const std::string before = s.text(layer);
    CHECK(before.find("Attention") == std::string::npos);

    CHECK(loadSelectionPages(s, *doc, layer) == 1);
    CHECK(layer.hasPage(1));
    const std::string after = s.text(layer);
    CHECK(after
          == "word: zebra.\n1\n"
             "Page Two\n"
             "Attention is all you need. The second page holds different text so tests can\n"
             "tell pages apart.\n"
             "2\n"
             "Page Three");

    // Nothing left to load.
    CHECK(loadSelectionPages(s, *doc, layer) == 0);
    CHECK(s.text(layer) == after);
}

TEST_CASE("reader_context loading pages leaves pages outside the selection alone")
{
    auto doc = openSample();
    TextLayer layer;
    const Selection s(TextPos{1, 0}, TextPos{1, 1});
    CHECK(loadSelectionPages(s, *doc, layer) == 1);
    CHECK(layer.hasPage(1));
    CHECK_FALSE(layer.hasPage(0));
    CHECK_FALSE(layer.hasPage(2));
    CHECK(s.text(layer) == "Page Two");
}

TEST_CASE("reader_context loading pages keeps the boxes of loaded pages")
{
    auto doc = openSample();
    TextLayer layer;
    // A stand-in for page 0's boxes: loadSelectionPages must not replace it.
    TextBox box;
    box.text = "kept";
    box.bbox = RectPts{10, 10, 20, 10};
    layer.setBoxes(0, {box});
    const Selection s(TextPos{0, 0}, TextPos{1, 1});
    CHECK(loadSelectionPages(s, *doc, layer) == 1);
    REQUIRE(layer.boxes(0) != nullptr);
    REQUIRE(layer.boxes(0)->size() == 1);
    CHECK(layer.boxes(0)->front().text == "kept");
    CHECK(s.text(layer) == "kept\nPage Two");
}
