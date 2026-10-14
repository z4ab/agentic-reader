#pragma once

#include "pdf/PageLayout.h"

#include <vector>

// Picks the layout for a document from its page shape. sizes are as
// PdfDocument::pageSizePts returns them, after /Rotate. Only the first 20 pages
// are checked. SinglePage when more than half of them are landscape
// (width / height >= 1.2), else Continuous. No pages gives Continuous.
LayoutMode detectLayout(const std::vector<PageSizePts>& sizes);
