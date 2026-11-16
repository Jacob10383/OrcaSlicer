#pragma once

#include "slic3r/Utils/BoxPrintMapping.hpp"
#include <memory>
#include <vector>
#include <wx/panel.h>

class SwitchButton;
class wxSizer;
class wxString;
class wxWindow;

namespace Slic3r {
class Moonraker;
namespace GUI {

// The choices a printer offers when a print starts, shown under the Box slots:
// a row for each of its print options, or the heat soak alone on a printer that
// only takes a one-time soak. Hidden until the printer answers with something
// to offer.
class PrintOptionsPanel : public wxPanel
{
public:
    explicit PrintOptionsPanel(wxWindow* parent);
    ~PrintOptionsPanel() override;
    // Asks the printer in the background, then shows what it offers.
    void lookup(const Moonraker& host);
    // What differs from the printer's defaults. Empty while nothing is shown.
    PrintStartChoices choices() const;

private:
    class MinutesStepper;
    struct Row
    {
        PrintOption option;
        // Null for a time without a switch.
        SwitchButton* toggle = nullptr;
        // Null for a switch without a time.
        MinutesStepper* minutes = nullptr;
    };
    void show_options(const PrintStartOptions& offered);
    void add_option(const PrintOption& option, bool first_in_section);
    wxSizer* labelled_row(const wxString& label, const wxString& hint, wxWindow* control);
    void relayout();

    std::vector<Row> m_rows;
    // The one-time soak of a printer without print options, with its default.
    MinutesStepper* m_soak_once = nullptr;
    int m_soak_once_default = 0;
    // Background lookups check this on the UI thread before touching the panel.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace GUI
} // namespace Slic3r
