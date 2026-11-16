#include "PrintOptionsPanel.hpp"
#include "BoxPrintMappingDialog.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/StaticBox.hpp"
#include "slic3r/GUI/Widgets/SwitchButton.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/valtext.h>
#include <wx/window.h>

namespace Slic3r {
namespace GUI {

// A joined "- value +" stepper for a time in minutes, as wide and tall as the
// Box slot pickers above it.
class PrintOptionsPanel::MinutesStepper : public StaticBox
{
public:
    MinutesStepper(wxWindow* parent, int min, int max, int step, int value)
        : StaticBox(parent), m_min(min), m_max(max), m_step(std::max(1, step))
    {
        SetCornerRadius(FromDIP(4));
        SetBorderColor(StateColor(std::make_pair(0xDBDBDBUL, (int) StateColor::Normal)));
        SetBackgroundColor(StateColor(std::make_pair(0xFFFFFFUL, (int) StateColor::Normal)));
        const auto divider = [this] {
            auto* line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
            line->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#DBDBDB")));
            return line;
        };
        const auto step_button = [this](const wxString& text, bool more) {
            auto* button = new ::Button(this, text);
            button->SetFont(::Label::Body_16);
            button->SetBorderWidth(0);
            button->SetCornerRadius(FromDIP(3));
            button->SetCanFocus(false);
            button->SetBackgroundColor(StateColor(std::make_pair(0xF8F8F8UL, (int) StateColor::Disabled),
                                                  std::make_pair(0xDFDFDFUL, (int) StateColor::Pressed),
                                                  std::make_pair(0xEEEEEEUL, (int) StateColor::Hovered),
                                                  std::make_pair(0xF8F8F8UL, (int) StateColor::Normal)));
            button->SetTextColor(StateColor(std::make_pair(0xACACACUL, (int) StateColor::Disabled),
                                            std::make_pair(0x262E30UL, (int) StateColor::Normal)));
            button->SetMinSize(FromDIP(wxSize(40, 30)));
            // Steps to the next multiple of the step size, so a typed or clamped
            // value goes back to round times.
            button->Bind(wxEVT_BUTTON, [this, more](wxCommandEvent&) {
                set_value(more ? m_value / m_step * m_step + m_step : (m_value + m_step - 1) / m_step * m_step - m_step);
            });
            return button;
        };
        m_less = step_button(wxString::FromUTF8("−"), false);
        m_more = step_button("+", true);

        m_input = new ::TextInput(this, "0", _L("min"), "", wxDefaultPosition, FromDIP(wxSize(72, 30)));
        m_input->SetBorderWidth(0);
        wxTextCtrl* text = m_input->GetTextCtrl();
        // The minutes sit right against their unit, centred in the stepper.
        text->SetWindowStyleFlag(text->GetWindowStyleFlag() | wxTE_RIGHT);
        text->SetValidator(wxTextValidator(wxFILTER_DIGITS));
        text->SetMaxLength(3);
        text->Bind(wxEVT_TEXT, [this, text](wxCommandEvent& e) {
            long typed = 0;
            if (text->GetValue().ToLong(&typed)) {
                m_value = static_cast<int>(std::clamp(typed, static_cast<long>(m_min), static_cast<long>(m_max)));
                enable_steps();
            }
            e.Skip();
        });
        // Leaving the field shows the value that will be sent: the last valid one, clamped.
        text->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
            set_value(m_value);
            e.Skip();
        });

        auto* steps = new wxBoxSizer(wxHORIZONTAL);
        steps->Add(m_less, 0, wxEXPAND);
        steps->Add(divider(), 0, wxEXPAND);
        steps->AddStretchSpacer();
        steps->Add(m_input, 0, wxALIGN_CENTER_VERTICAL);
        steps->AddStretchSpacer();
        steps->Add(divider(), 0, wxEXPAND);
        steps->Add(m_more, 0, wxEXPAND);
        auto* inset = new wxBoxSizer(wxVERTICAL);
        inset->Add(steps, 1, wxEXPAND | wxALL, 1); // inside the 1 px border
        SetSizer(inset);
        SetMinSize(FromDIP(wxSize(220, 32)));
        set_value(value);
    }

    int value() const { return m_value; }

private:
    void set_value(int value)
    {
        m_value = std::clamp(value, m_min, m_max);
        // ChangeValue: no wxEVT_TEXT, so the typed-value handler doesn't run again.
        m_input->GetTextCtrl()->ChangeValue(wxString::Format("%d", m_value));
        enable_steps();
    }

    void enable_steps()
    {
        m_less->Enable(m_value > m_min);
        m_more->Enable(m_value < m_max);
    }

    int          m_min;
    int          m_max;
    int          m_step;
    int          m_value = 0;
    ::TextInput* m_input = nullptr;
    ::Button*    m_less  = nullptr;
    ::Button*    m_more  = nullptr;
};

PrintOptionsPanel::PrintOptionsPanel(wxWindow* parent) : wxPanel(parent)
{
    SetBackgroundColour(parent->GetBackgroundColour());
    SetForegroundColour(parent->GetForegroundColour());
    SetSizer(new wxBoxSizer(wxVERTICAL));
    Hide();
}

PrintOptionsPanel::~PrintOptionsPanel() { *m_alive = false; }

void PrintOptionsPanel::lookup(const Moonraker& host)
{
    fetch_print_start_options_async(host, [this, alive = m_alive](const PrintStartOptions& offered) {
        wxGetApp().CallAfter([this, alive, offered] {
            if (*alive)
                show_options(offered);
        });
    });
}

wxSizer* PrintOptionsPanel::labelled_row(const wxString& label, const wxString& hint, wxWindow* control)
{
    auto* texts = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(this, wxID_ANY, label);
    title->SetFont(::Label::Body_14);
    title->SetForegroundColour(GetForegroundColour());
    texts->Add(title);
    if (!hint.empty()) {
        auto* detail = new wxStaticText(this, wxID_ANY, hint);
        detail->SetFont(::Label::Body_12);
        detail->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        texts->Add(detail, 0, wxTOP, FromDIP(2));
    }
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(texts, 1, wxALIGN_CENTER_VERTICAL);
    // The pickers above sit 8 DIP inside their cards; the labels line up with the headers.
    row->Add(control, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(9));
    return row;
}

void PrintOptionsPanel::add_option(const PrintOption& option, bool first_in_section)
{
    wxSizer*   rows  = GetSizer();
    const int  gap   = first_in_section ? 0 : FromDIP(8);
    const bool timed = option.type == PrintOption::Type::Minutes;
    Row        row{option};
    // The printer's own text: it is not in Orca's translations.
    const wxString label = from_u8(option.label);
    const wxString hint  = from_u8(option.hint);
    if (timed)
        row.minutes = new MinutesStepper(this, option.min, option.max, option.step, option.default_value);
    if (timed && !option.optional) {
        rows->Add(labelled_row(label, hint, row.minutes), 0, wxEXPAND | wxTOP, gap);
        m_rows.push_back(std::move(row));
        return;
    }
    row.toggle = new SwitchButton(this);
    row.toggle->SetBackgroundColour(GetBackgroundColour());
    row.toggle->SetValue(default_print_choice(option).on);
    rows->Add(labelled_row(label, hint, row.toggle), 0, wxEXPAND | wxTOP, gap);
    if (timed) {
        // The time is asked for once the switch is on.
        wxSizer* time_row = labelled_row(option.value_label.empty() ? _L("Time") : from_u8(option.value_label), {}, row.minutes);
        rows->Add(time_row, 0, wxEXPAND | wxTOP, FromDIP(8));
        rows->Show(time_row, row.toggle->GetValue());
        row.toggle->Bind(wxEVT_TOGGLEBUTTON, [this, rows, time_row](wxCommandEvent& e) {
            rows->Show(time_row, e.IsChecked());
            relayout();
            e.Skip();
        });
    }
    m_rows.push_back(std::move(row));
}

void PrintOptionsPanel::show_options(const PrintStartOptions& offered)
{
    wxSizer* rows = GetSizer();
    const auto header = [this, rows](const wxString& title) {
        rows->Add(create_print_section_header(this, title), 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
    };
    // Options are grouped under their headings, in the printer's order.
    std::vector<std::string> sections;
    for (const auto& option : offered.options)
        if (std::find(sections.begin(), sections.end(), option.section) == sections.end())
            sections.push_back(option.section);
    for (const auto& section : sections) {
        if (!section.empty())
            header(from_u8(section));
        bool first = !section.empty();
        for (const auto& option : offered.options) {
            if (option.section != section)
                continue;
            add_option(option, first);
            first = false;
        }
    }
    if (offered.options.empty() && offered.soak_once_default) {
        header(_L("Before printing"));
        m_soak_once_default = *offered.soak_once_default;
        m_soak_once         = new MinutesStepper(this, 0, MAX_HEAT_SOAK_MINUTES, 5, m_soak_once_default);
        rows->Add(labelled_row(_L("Heat soak"), {}, m_soak_once), 0, wxEXPAND);
    }
    if (m_rows.empty() && !m_soak_once)
        return;
    Show();
    relayout();
}

void PrintOptionsPanel::relayout()
{
    Layout();
    if (wxWindow* dialog = wxGetTopLevelParent(this)) {
        dialog->Layout();
        dialog->Fit();
    }
}

PrintStartChoices PrintOptionsPanel::choices() const
{
    PrintStartChoices result;
    std::vector<PrintOption> options;
    std::vector<PrintChoice> chosen;
    for (const Row& row : m_rows) {
        options.push_back(row.option);
        chosen.push_back({row.toggle ? row.toggle->GetValue() : true, row.minutes ? row.minutes->value() : 0});
    }
    result.print_options = print_options_command(options, chosen);
    // An unchanged soak is left to the printer's own setting.
    if (m_soak_once && m_soak_once->value() != m_soak_once_default)
        result.soak_once = m_soak_once->value();
    return result;
}

} // namespace GUI
} // namespace Slic3r
