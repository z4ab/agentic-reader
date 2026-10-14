# M10 slide decks: try-out guide

M10.1-M10.5 are merged on `main`. Q1-Q4 in `DESIGN.md` shipped with the proposed defaults. Result (2026-10-13): the user tried them and kept all four defaults (no skip-builds, chat hides in fullscreen, no thumbnails, wheel flips slides).

## Start

```
cd ~/Documents/git/agentic-reader
./build/app/agentic-reader tests/data/slides-16x9.pdf
```

Then open one of your own lecture decks (File > Open) and one long course-notes PDF. Decks open in Single Page and portrait PDFs in Continuous on their own.

## Keys

| Action | Keys |
|---|---|
| Next slide | Right, PgDn, Space, N, wheel down, toolbar Next |
| Previous slide | Left, PgUp, Shift+Space, P, wheel up, toolbar Previous |
| First / last | Home / End |
| Switch layout | Ctrl+Shift+L, View menu, toolbar "Single Page" toggle |
| Fullscreen | F11 (Esc or F11 to leave) |
| Default zoom for the layout | Ctrl+0 |

## Questions to answer by trying the defaults

- Q1. Build slides. Default: every page is a slide, so a bullet that appears in 5 steps takes 5 presses. If your decks have many of these and stepping through them is annoying, ask for a "Skip builds" mode.
- Q2. Chat in fullscreen. Default: F11 hides the chat pane. Present or read a deck in fullscreen. If you keep pressing F11 to ask a question, ask for fullscreen that keeps the chat pane.
- Q3. Thumbnails. Default: none. Jump around a 40+ slide deck with Ctrl+G, Home/End and Continuous mode. If you miss seeing all slides at a glance, ask for a thumbnail strip in Single Page.
- Q4. Wheel flips slides. Default: one wheel notch or trackpad swipe moves one slide. If it moves too easily (trackpad), or you would rather scroll than flip, ask for keys and buttons only.

## Checks the automated tests could not do

- C1. One mouse-wheel notch moves exactly one slide. One trackpad swipe moves exactly one slide, not two or three.
- C2. Zoom a slide in (Ctrl+wheel) until it is taller than the pane. Down and the wheel scroll to the slide's bottom edge first, and only the next press flips. Up does the same toward the top.
- C3. In Single Page at Fit Page, drag the chat splitter. The slide re-fits.
- C4. Click in the chat input and press Ctrl+Shift+L. The layout switches and the typed text does not change.
- C5. F11 fills the whole screen on your desktop. F11 or Esc brings the panes back at the same widths.
- C6. In fullscreen, open the find bar (Ctrl+F), click the slide, press Esc. Fullscreen exits and the find bar stays open.
- C7. Switch a portrait PDF to Single Page in a project, quit, reopen. It comes back in Single Page on the same page. Switch it back and the choice is forgotten.

## Known limits

- On Linux the Chat menu shows no key hint next to Next Chat and Previous Chat (Ctrl+Tab). wxGTK cannot display Tab shortcuts. The keys work.
- A slide that is only wider than the pane (not taller) flips on Down and the wheel. Pan it sideways with the horizontal wheel or the scrollbar.
