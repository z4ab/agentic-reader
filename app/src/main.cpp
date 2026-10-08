#include <wx/app.h>
#include <wx/log.h>

#include <poppler-version.h>

#include "MainFrame.h"

class ReaderApp : public wxApp
{
public:
    bool OnInit() override
    {
        if (!wxApp::OnInit())
            return false;

        // Referencing poppler-cpp here makes the app binary link it (M1.2, risk R2).
        wxLogDebug("poppler-cpp %s", poppler::version_string());

        auto* frame = new MainFrame();
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP(ReaderApp);
