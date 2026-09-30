#include "BoxPrintMappingDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <wx/dcmemory.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(wxEVT_BOX_PRINT_MAPPING_CHANGED, wxCommandEvent);

namespace {
std::string config_string_at(const DynamicPrintConfig& config, const char* key, size_t index)
{
    const auto* values = config.option<ConfigOptionStrings>(key);
    return values && index < values->values.size() ? values->values[index] : std::string();
}

wxString slot_label(const BoxFilamentSlot& slot)
{
    wxString label = slot.external ? _L("External spool") :
        wxString::Format(_L("Box %d, slot %d"), slot.index / 4 + 1, slot.index % 4 + 1);
    const auto& description = slot.name.empty() ? slot.material : slot.name;
    if (!description.empty())
        label += wxString::FromUTF8(" · ") + wxString::FromUTF8(description);
    return label;
}
} // namespace

BoxPrintMappingPanel::BoxPrintMappingPanel(wxWindow* parent)
    : wxPanel(parent), m_scroll(new wxScrolledWindow(this, wxID_ANY))
{
    SetFont(::Label::Body_13);
    SetBackgroundColour(parent->GetBackgroundColour());
    SetForegroundColour(parent->GetForegroundColour());
    m_scroll->SetFont(GetFont());
    m_scroll->SetBackgroundColour(GetBackgroundColour());
    m_scroll->SetForegroundColour(GetForegroundColour());
    m_scroll->SetScrollRate(0, FromDIP(12));
    m_scroll->SetSizer(new wxBoxSizer(wxVERTICAL));
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(m_scroll, 1, wxEXPAND);
    SetSizer(layout);
}

BoxPrintMappingPanel::~BoxPrintMappingPanel()
{
    *m_alive = false;
}

void BoxPrintMappingPanel::lookup(const Moonraker& host, const std::vector<BoxPrintTool>& tools, const wxString& plate_error)
{
    m_pending = true;
    m_supported = true;
    m_error = false;
    m_tools = tools;
    show_message(_L("Checking filament slots..."));
    fetch_box_print_status_async(host, [this, alive = m_alive, tools, plate_error](
                                           bool fetched, const BoxPrintStatus& status, const std::string& error) {
        wxGetApp().CallAfter([this, alive, tools, plate_error, fetched, status, error] {
            if (!*alive)
                return;
            m_pending = false;
            if (fetched && !status.supported) {
                m_supported = false;
                Hide();
            } else if (!fetched) {
                set_data(tools, {}, _L("Could not check Box print mapping support.") + "\n" + wxString::FromUTF8(error));
            } else {
                set_data(tools, status.slots, plate_error);
            }
            notify_changed();
        });
    });
}

void BoxPrintMappingPanel::show_message(const wxString& message)
{
    m_choices.clear();
    m_warnings.clear();
    m_notice = nullptr;
    auto* rows = m_scroll->GetSizer();
    rows->Clear(true);
    auto* text = new wxStaticText(m_scroll, wxID_ANY, message);
    text->Wrap(FromDIP(360));
    rows->Add(text, 0, wxEXPAND | wxALL, FromDIP(4));
    m_scroll->SetMinSize(wxSize(FromDIP(380), rows->CalcMin().y));
    Layout();
    m_scroll->Layout();
    m_scroll->FitInside();
}

void BoxPrintMappingPanel::notify_changed()
{
    wxCommandEvent event(wxEVT_BOX_PRINT_MAPPING_CHANGED, GetId());
    event.SetEventObject(this);
    ProcessWindowEvent(event);
}

void BoxPrintMappingPanel::set_data(const std::vector<BoxPrintTool>& tools,
                                   const std::vector<BoxFilamentSlot>& slots, const wxString& error)
{
    m_tools = tools;
    m_slots.clear();
    m_error = !error.empty() || tools.empty();
    if (m_error) {
        show_message(error.empty() ?
            _L("No sliced filament data is available. Slice this plate again before printing.") : error);
        return;
    }
    m_choices.clear();
    m_warnings.clear();
    m_notice = nullptr;
    auto* rows = m_scroll->GetSizer();
    rows->Clear(true);
    for (const auto& slot : slots)
        if (slot.present || slot.external)
            m_slots.push_back(slot);

    const auto swatch = [this](const std::string& value) {
        wxBitmap bitmap(FromDIP(16), FromDIP(16));
        wxMemoryDC dc(bitmap);
        const wxColour color(wxString::FromUTF8(value));
        dc.SetBackground(wxBrush(m_scroll->GetBackgroundColour()));
        dc.Clear();
        dc.SetPen(wxPen(wxColour(140, 140, 140)));
        dc.SetBrush(wxBrush(color.IsOk() ? color : wxColour(128, 128, 128)));
        dc.DrawRoundedRectangle(0, 0, bitmap.GetWidth(), bitmap.GetHeight(), FromDIP(3));
        dc.SelectObject(wxNullBitmap);
        return bitmap;
    };

    const auto suggested = suggest_box_print_mapping(m_tools, m_slots);
    for (const auto& tool : m_tools) {
        auto* row = new wxBoxSizer(wxVERTICAL);
        auto* heading = new wxBoxSizer(wxHORIZONTAL);
        const wxColour color(wxString::FromUTF8(tool.color));
        const auto* badge = get_extruder_color_icon(color.IsOk() ? tool.color : "#808080",
                                                   std::to_string(tool.id + 1), FromDIP(18), FromDIP(18));
        heading->Add(new wxStaticBitmap(m_scroll, wxID_ANY, *badge),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        wxString name = wxString::FromUTF8(tool.name.empty() ? tool.material : tool.name);
        auto* profile = new wxStaticText(m_scroll, wxID_ANY, name,
                                         wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        profile->SetToolTip(name);
        profile->SetMinSize(FromDIP(wxSize(100, -1)));
        heading->Add(profile, 1, wxALIGN_CENTER_VERTICAL);
        row->Add(heading, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        auto* load_from = new wxStaticText(m_scroll, wxID_ANY, _L("Load from"));
        load_from->SetFont(::Label::Body_12);
        row->Add(load_from, 0, wxBOTTOM, FromDIP(2));

        auto* choice = new ComboBox(m_scroll, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                    FromDIP(wxSize(360, 32)), 0, nullptr, wxCB_READONLY);
        choice->SetFont(::Label::Body_13);
        choice->SetCornerRadius(4);
        choice->SetKeepDropArrow(true);
        choice->SetIconOnRight(true);
        choice->Append(_L("Select a slot"));
        choice->SetSelection(0);
        const auto slot = suggested.find(tool.id);
        for (size_t i = 0; i < m_slots.size(); ++i) {
            choice->Append(slot_label(m_slots[i]), swatch(m_slots[i].color));
            if (slot != suggested.end() && m_slots[i].index == slot->second)
                choice->SetSelection(static_cast<int>(i) + 1);
        }
        choice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            update_selection();
            notify_changed();
        });
        row->Add(choice, 0, wxEXPAND);
        auto* warning_panel = new wxPanel(m_scroll);
        auto* warning_row = new wxBoxSizer(wxHORIZONTAL);
        warning_row->Add(new wxStaticBitmap(warning_panel, wxID_ANY, create_scaled_bitmap("obj_warning", warning_panel, 14)),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        auto* warning = new wxStaticText(warning_panel, wxID_ANY, wxEmptyString);
        warning->SetFont(::Label::Body_12);
        warning->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#B56A00")));
        warning_row->Add(warning, 1, wxALIGN_CENTER_VERTICAL);
        warning_panel->SetSizer(warning_row);
        row->Add(warning_panel, 0, wxEXPAND | wxTOP, FromDIP(4));
        rows->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(16));
        m_choices.push_back(choice);
        m_warnings.push_back(warning);
    }
    auto* notice_panel = new wxPanel(m_scroll);
    auto* notice_row = new wxBoxSizer(wxHORIZONTAL);
    const auto notice_color = StateColor::darkModeColorFor(wxColour("#2196F3"));
    auto* notice_icon = new wxStaticText(notice_panel, wxID_ANY, wxString::FromUTF8("ⓘ"));
    notice_icon->SetForegroundColour(notice_color);
    notice_row->Add(notice_icon, 0, wxRIGHT, FromDIP(4));
    m_notice = new wxStaticText(notice_panel, wxID_ANY,
        _L("External spool changes require you to load filament at the printer."));
    m_notice->SetFont(::Label::Body_12);
    m_notice->SetForegroundColour(notice_color);
    m_notice->Wrap(FromDIP(340));
    notice_row->Add(m_notice, 1);
    notice_panel->SetSizer(notice_row);
    rows->Add(notice_panel, 0, wxEXPAND);
    update_selection();
}

bool BoxPrintMappingPanel::complete() const
{
    return !m_pending && !m_error && !m_choices.empty() && std::all_of(m_choices.begin(), m_choices.end(),
        [](const ComboBox* choice) { return choice->GetSelection() > 0; });
}

bool BoxPrintMappingPanel::ready_to_print() const
{
    return !m_pending && (!m_supported || complete());
}

std::string BoxPrintMappingPanel::serialized_mapping() const
{
    if (!m_supported || !complete())
        return {};
    BoxPrintMapping mapping;
    for (size_t i = 0; i < m_tools.size(); ++i)
        mapping.emplace(m_tools[i].id, m_slots[m_choices[i]->GetSelection() - 1].index);
    return serialize_box_print_mapping(mapping);
}

void BoxPrintMappingPanel::update_selection()
{
    bool external = false;
    for (size_t i = 0; i < m_choices.size(); ++i) {
        wxString warning;
        const int selected = m_choices[i]->GetSelection();
        if (selected > 0) {
            const auto& slot = m_slots[selected - 1];
            external |= slot.external;
            if (!slot.material.empty() && !m_tools[i].material.empty() &&
                wxString::FromUTF8(slot.material).CmpNoCase(wxString::FromUTF8(m_tools[i].material)) != 0)
                warning = wxString::Format(_L("File expects %s; selected slot is %s."),
                    wxString::FromUTF8(m_tools[i].material), wxString::FromUTF8(slot.material));
        }
        m_warnings[i]->SetLabel(warning);
        m_warnings[i]->Wrap(FromDIP(340));
        m_warnings[i]->GetParent()->Show(!warning.empty());
        m_warnings[i]->GetParent()->Layout();
    }
    m_notice->GetParent()->Show(external);
    m_notice->GetParent()->Layout();
    m_scroll->SetMinSize(wxSize(FromDIP(380), std::min(FromDIP(340), m_scroll->GetSizer()->CalcMin().y)));
    Layout();
    m_scroll->Layout();
    m_scroll->FitInside();
}

std::vector<BoxPrintTool> collect_box_print_tools(PartPlate* plate, const DynamicPrintConfig& sliced_config)
{
    std::vector<BoxPrintTool> tools;
    if (!plate || !plate->is_slice_result_valid() || !plate->get_slice_result())
        return tools;

    const auto* diameters = sliced_config.option<ConfigOptionFloats>("filament_diameter");
    for (const auto& usage : plate->get_slice_result()->print_statistics.total_volumes_per_extruder) {
        // Box reads the footer's `filament used [mm]`, printed with two decimals.
        // Use the same rounding so both sides agree on which tools are used.
        const double diameter = diameters && usage.first < diameters->values.size() ? diameters->values[usage.first] : 1.75;
        const double length = usage.second / (M_PI * 0.25 * diameter * diameter);
        if (std::round(length * 100.) <= 0.)
            continue;
        BoxPrintTool tool;
        tool.id = static_cast<int>(usage.first);
        tool.name = config_string_at(sliced_config, "filament_settings_id", usage.first);
        tool.color = config_string_at(sliced_config, "filament_colour", usage.first);
        tool.material = config_string_at(sliced_config, "filament_type", usage.first);
        tools.push_back(std::move(tool));
    }
    return tools;
}

} } // namespace Slic3r::GUI
