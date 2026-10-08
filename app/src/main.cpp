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

        auto* frame = new MainFrame(m_pdfPath);
        frame->Show();
        return true;
    }

    // Temporary: accept one optional PDF path (M2.2). M6 replaces this.
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
