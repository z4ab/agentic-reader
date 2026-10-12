#include <wx/app.h>
#include <wx/cmdline.h>

#include "MainFrame.h"

class ReaderApp : public wxApp
{
public:
    bool OnInit() override
    {
        if (!wxApp::OnInit())
            return false;

        auto* frame = new MainFrame();
        frame->Show();
        // Opened once the window is on screen, so an error box has a visible parent.
        // The project panel's file watcher needs the running event loop.
        if (!m_pdfPath.empty())
            frame->CallAfter([frame, path = m_pdfPath] { frame->openFile(path); });
        else
            frame->CallAfter([frame] { frame->restoreSession(); });
        return true;
    }

    // One optional PDF path to open at startup.
    void OnInitCmdLine(wxCmdLineParser& parser) override
    {
        wxApp::OnInitCmdLine(parser);
        parser.AddParam("pdf", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL);
    }

    bool OnCmdLineParsed(wxCmdLineParser& parser) override
    {
        if (parser.GetParamCount() > 0)
            m_pdfPath = parser.GetParam(0);
        return wxApp::OnCmdLineParsed(parser);
    }

private:
    wxString m_pdfPath;
};

wxIMPLEMENT_APP(ReaderApp);
