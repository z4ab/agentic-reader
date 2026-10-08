# Slide deck support (deferred design)

Deferred until after M8. This is a design note, not a task. Agents must not pick it up. When work starts, split it into task files in this folder.

Origin: the user wants the PDF view to handle both long "course notes" PDFs (one long document) and lecture slide PDFs. A design agent proposed the design below on 2026-10-08, written against the M2–M6 specs before M2.2 started. M2.2, M2.4, M3.x, M5.3, M6.1 and M6.5 are being built without it, so the "Changes to existing tasks" section describes changes to finished code, not edits to open task files.

## Recommendation

Two layout modes, Continuous and Single Page, chosen automatically from page shape, with a per-file override. F11 fullscreen is the presentation mode. Thumbnails, presenter features and build-page handling are out.

## Decisions

- D1 Two modes only. Continuous is the M2.2 vertical scroll. Single Page shows one page, centred and fit to the pane. Both live in `PageLayout`. In Single Page only the shown page has a rect, so `visiblePages`, `pageAt` and the M3.1 coordinate mapping work unchanged.
- D2 Auto-detect from shape, not page count. Check the first 20 pages after rotation. If more than half have width/height ≥ 1.2, open in Single Page, else Continuous. This catches 4:3 (1.33) and 16:9 (1.78). Course notes and portrait handouts stay Continuous.
- D3 Per-file override. A View menu radio, a toolbar toggle and Ctrl+Shift+L switch modes. Switching keeps the current page. `project.json` stores `layout` only when the choice differs from auto-detect. Switching back removes it, so there is no reset command.
- D4 Zoom. Default is fit width for Continuous and fit page for Single Page. Fit modes stay in effect, so the page re-fits on window resize, chat splitter drag and fullscreen. Ctrl+0 returns to the current mode's default.
- D5 Single Page navigation.
  - Keys: Right, PgDn, Space and N go forward. Left, PgUp, Shift+Space and P go back. Home/End and Ctrl+G work as in Continuous.
  - Up/Down scroll 40px when zoomed past fit and flip at the page edge, otherwise flip.
  - Wheel: one notch flips one page, with a 150 ms lockout after each flip so trackpad momentum does not skip slides. When zoomed in, the wheel pans first and flips at the edge.
  - Toolbar gets Prev and Next buttons in both modes.
  - No click zones, because a left drag selects text.
- D6 Fullscreen (F11) is the presentation mode. It hides the project pane, chat pane and toolbar. Esc or F11 restores them at the same widths. Works in both modes. Single Page fullscreen has a black background.
- D7 Chat pane stays as is. Single Page letterboxes the slide in the remaining width. Ctrl+Shift+A ("Ask about selection") leaves fullscreen first.
- D8 Current page and reader context. In Single Page the current page is the page on screen. The `<reader_context>` format does not change.
- D9 Selection and find (M3). Selection stays within the shown page, survives page flips, and is drawn only when its page is shown. Find highlights matches on the shown page. Next/prev go through one `PdfView::revealRect(page, rectPts)`, which in Single Page switches to the match's page.

## Left out

- X1 Thumbnail or filmstrip strip. Needs a second render queue and a fourth pane competing for width. On decks with builds it is mostly repeats. Ctrl+G, Home/End and Continuous mode cover jumping around.
- X2 Collapsing build pages. Needs M3.1 text and a heuristic.
- X3 Presenter features: two-page spreads, presenter view, timers, transitions.

## Open questions for the user

- Q1. Do your lecture decks have many build pages? If so, add a "Skip builds" task that steps over pages whose text is contained in the next page's text.
- Q2. In fullscreen, should the chat pane hide (proposed) or stay visible with only the project pane and toolbar hidden?
- Q3. Is leaving the thumbnail strip out acceptable? If not, add a collapsible strip in Single Page mode only.
- Q4. In Single Page mode, should the wheel flip slides (proposed), or only keys and buttons?

## Changes to existing tasks

### PageLayout and PdfView (M2.2)

- `PageLayout` inputs: page sizes in points, zoom, viewport size, DPI scale, `LayoutMode`, and the shown page (Single Page only). Outputs: each page's rect in virtual pixels and the virtual size.
- `enum class LayoutMode { Continuous, SinglePage }`. Continuous: pages stacked vertically, centred horizontally, 12px gap. SinglePage: only the shown page has a rect, centred on both axes when smaller than the viewport. Virtual size is the larger of the viewport and the page plus a 12px margin.
- `pageAt(y)` and `visiblePages(viewRect)` return only the shown page in SinglePage.
- `fitWidthZoom(page)` and `fitPageZoom(page)` return the zoom that fits that page to the viewport.
- `PdfView::setLayoutMode(mode)`.
- Done when: `PageLayout` tests cover both modes (gaps, centring, `pageAt`, `visiblePages`), SinglePage centring on both axes, and both fit helpers for a portrait page and a 16:9 page.

### Zoom and navigation (M2.4)

- Fit modes re-fit when the viewport resizes (window, splitter, fullscreen). Ctrl+wheel and Ctrl+`+`/`-` step by 10% and switch to custom. Ctrl+0 resets to the layout mode's default.
- Current page: in Continuous, the page with the most visible area. In SinglePage, the shown page.
- One navigation API for keys, toolbar, go-to box, find and position restore: `goToPage(int page)` puts the top of the page at the top of the viewport. `revealRect(int page, wxRect2DDouble rectPts)` scrolls a rect in page points into view with a 24px margin.
- Getter `layoutMode()`.
- Done when adds: in fit width, dragging the chat splitter re-fits the page.

### Text layer, selection, find (M3.1–M3.3)

- Coordinate mapping works in SinglePage mode, where only the shown page maps. `viewToPage` returns no page for points outside it. Done when: the round trip also holds in SinglePage mode.
- Selection: in SinglePage a drag selects within the shown page only. The selection survives page flips and is drawn only when its page is shown. Done when (manual): select text on slide 3, flip to slide 4 and back, the highlight is still there and Ctrl+C copies it.
- Find: Enter or next goes to the next match through `PdfView::revealRect`, which in SinglePage shows the match's page. Shift+Enter or prev goes back. Wrap at the ends. Done when: in SinglePage, next and prev flip to the slide holding the match.

### Ask about selection (M5.3)

- If the frame is fullscreen, leave fullscreen first so the chat pane is visible.

### Project file and reading position (M6.1, M6.5)

- `project.json` per-file entry gains `"layout":"single"|"continuous"`, present only when the user overrode auto-detect. M6.1 already preserves unknown fields.
- On `EVT_PDF_LAYOUT_CHANGED`, store `layout` if the chosen mode differs from `detectLayout`, remove the key if it matches.
- On open, pass the stored `layout` to `PdfView` before the first layout so the file never shows in the wrong mode. Then restore `zoom` and `last_page`.
- Done when (manual): switch a portrait PDF to Single Page and restart. It reopens in Single Page on the same page. Switch it back to Continuous and `layout` disappears from `project.json`.

## New task: single-page layout for slide decks

- files: `app/src/pdf/LayoutDetect.{h,cpp}`, `tests/test_layout_detect.cpp`, `app/src/pdf/PdfView.{h,cpp}`, `app/src/MainFrame.{h,cpp}`, `tests/data/slides-16x9.pdf`
- goal: Lecture slide PDFs open one slide at a time, fit to the pane, with slide-style navigation and fullscreen.
- spec:
  1. `detectLayout(pageSizes) -> LayoutMode` is a pure function over page sizes as returned by `PdfDocument::pageSizePts`. It checks the first 20 pages. If more than half have width/height ≥ 1.2, it returns SinglePage, otherwise (or for no pages) Continuous.
  2. On open, `PdfView` uses the per-file layout passed in by the caller if there is one, else `detectLayout`. Zoom starts at the mode's default.
  3. Mode switch: View menu radio items "Continuous" and "Single Page", a toolbar toggle, and Ctrl+Shift+L. Switching keeps the current page and applies the new mode's default zoom. Emit `EVT_PDF_LAYOUT_CHANGED` only for user-initiated switches.
  4. SinglePage keys: Right, PgDn, Space, N next. Left, PgUp, Shift+Space, P previous. Home/End first/last. Up/Down scroll 40px within a page that overflows the viewport and flip at its edge, flip directly when the page fits. Ctrl+G as in Continuous.
  5. SinglePage wheel: when the page fits, accumulate `GetWheelRotation()` and flip one page per `GetWheelDelta()`, ignoring wheel events for 150 ms after a flip. When the page overflows, the wheel pans and one more full notch at the edge flips. Going forward shows the new page from its top, going back from its bottom.
  6. Toolbar gains Previous and Next page buttons, active in both modes.
  7. SinglePage branch of `goToPage` and `revealRect`: show the target page and keep the zoom.
  8. F11 toggles fullscreen: `ShowFullScreen(true, wxFULLSCREEN_ALL)`, unsplit the project and chat panes, hide the toolbar. Esc or F11 restores the panes at their previous sash positions and the toolbar. Works in both modes. SinglePage fullscreen background is black.
- done when:
  - [ ] `detectLayout` tests: Continuous for portrait Letter or A4, SinglePage for 16:9 and 4:3, Continuous for a portrait document with one landscape page, Continuous for an empty list.
  - [ ] Manual: `sample.pdf` opens in Continuous and the 16:9 deck in SinglePage. The toggle switches both ways and keeps the page.
  - [ ] SinglePage: every key in item 4 works. One mouse-wheel notch and one trackpad swipe each move exactly one slide.
  - [ ] Dragging the chat splitter re-fits the slide. F11 in and out restores the same pane widths.
- verify: `ctest -R layout_detect`, then a manual run with both PDFs. Record the deck's source in `notes`.
