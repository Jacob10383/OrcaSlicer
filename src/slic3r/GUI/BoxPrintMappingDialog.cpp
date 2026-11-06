#include "BoxPrintMappingDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/StateColor.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
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

// The closed picker shows one line, so it names the slot and its material; the
// menu item's tooltip carries the filament name.
wxString slot_label(const BoxFilamentSlot& slot)
{
    wxString label = slot.external ? _L("External spool") :
        wxString::Format(_L("Box %d, slot %d"), slot.index / 4 + 1, slot.index % 4 + 1);
    const auto& description = slot.material.empty() ? slot.name : slot.material;
    if (!description.empty())
        label += wxString::FromUTF8(" · ") + wxString::FromUTF8(description);
    return label;
}

// Orca names presets "<name> @<printer>", and copies get " - Copy" after that.
wxString short_preset_name(const std::string& name)
{
    wxString result = wxString::FromUTF8(name.substr(0, name.find('@')));
    result.Trim(true).Trim(false);
    return result;
}

wxColour muted_text_colour() { return StateColor::darkModeColorFor(wxColour("#6B6B6B")); }
// Light palette values; StateColor maps them for dark mode when it paints.
constexpr unsigned long WARNING_RGB    = 0xB56A00;
constexpr unsigned long WARNING_DARK   = 0xF0A040; // no dark mapping exists for the warning colour
constexpr unsigned long ROW_BORDER_RGB = 0xDBDBDB;
constexpr unsigned long LABEL_RGB      = 0x262E30;
} // namespace

wxSizer* create_print_section_header(wxWindow* parent, const wxString& title, const wxString& column)
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    for (const wxString* text : {&title, &column}) {
        if (text->empty())
            continue;
        auto* label = new wxStaticText(parent, wxID_ANY, text->Upper());
        label->SetFont(::Label::Body_12);
        label->SetForegroundColour(muted_text_colour());
        row->Add(label, text == &title ? 1 : 0, wxALIGN_CENTER_VERTICAL);
    }
    return row;
}

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
    m_rows.clear();
    m_notice = nullptr;
    auto* rows = m_scroll->GetSizer();
    rows->Clear(true);
    auto* text = new wxStaticText(m_scroll, wxID_ANY, message);
    text->Wrap(FromDIP(400));
    rows->Add(text, 0, wxEXPAND | wxALL, FromDIP(4));
    m_scroll->SetMinSize(wxSize(FromDIP(420), rows->CalcMin().y));
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
    m_rows.clear();
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

    rows->Add(create_print_section_header(m_scroll, _L("Filament"), _L("Load from")), 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    const auto suggested = suggest_box_print_mapping(m_tools, m_slots);
    for (const auto& tool : m_tools) {
        // Same layout as Fluidd's print dialog: the file's filament, an arrow, then the slot.
        auto* card = new StaticBox(m_scroll);
        card->SetCornerRadius(FromDIP(6));
        // Built after the dialog's dark-mode pass, so take the palette text colour here.
        card->SetForegroundColour(m_scroll->GetForegroundColour());
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        const wxColour color(wxString::FromUTF8(tool.color));
        const auto* badge = get_extruder_color_icon(color.IsOk() ? tool.color : "#808080",
                                                   std::to_string(tool.id + 1), FromDIP(20), FromDIP(20));
        row->Add(new wxStaticBitmap(card, wxID_ANY, *badge), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

        const wxString material = wxString::FromUTF8(tool.material);
        const wxString preset   = short_preset_name(tool.name);
        const wxString full     = wxString::FromUTF8(tool.name.empty() ? tool.material : tool.name);
        auto* names = new wxBoxSizer(wxVERTICAL);
        auto* primary = new wxStaticText(card, wxID_ANY, material.empty() ? preset : material,
                                         wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        primary->SetFont(::Label::Body_14);
        primary->SetMinSize(wxSize(FromDIP(40), -1));
        primary->SetToolTip(full);
        names->Add(primary, 0, wxEXPAND);
        if (!material.empty() && !preset.empty()) {
            auto* secondary = new wxStaticText(card, wxID_ANY, preset, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
            secondary->SetFont(::Label::Body_12);
            secondary->SetForegroundColour(muted_text_colour());
            secondary->SetMinSize(wxSize(FromDIP(40), -1));
            secondary->SetToolTip(full);
            names->Add(secondary, 0, wxEXPAND | wxTOP, FromDIP(2));
        }
        row->Add(names, 1, wxALIGN_CENTER_VERTICAL);

        auto* arrow = new wxStaticText(card, wxID_ANY, wxString::FromUTF8("→"));
        arrow->SetFont(::Label::Body_14);
        arrow->SetForegroundColour(muted_text_colour());
        row->Add(arrow, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));

        auto* choice = new ComboBox(card, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                    FromDIP(wxSize(220, 32)), 0, nullptr, wxCB_READONLY);
        choice->SetFont(::Label::Body_13);
        choice->SetCornerRadius(4);
        choice->SetKeepDropArrow(true);
        choice->SetIconOnRight(true);
        choice->Append(_L("Select a slot"));
        choice->SetSelection(0);
        const auto slot = suggested.find(tool.id);
        for (size_t i = 0; i < m_slots.size(); ++i) {
            const int item = choice->Append(slot_label(m_slots[i]), swatch(m_slots[i].color));
            if (!m_slots[i].name.empty())
                choice->SetItemTooltip(item, wxString::FromUTF8(m_slots[i].name));
            if (slot != suggested.end() && m_slots[i].index == slot->second)
                choice->SetSelection(static_cast<int>(i) + 1);
        }
        choice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            update_selection();
            notify_changed();
        });
        row->Add(choice, 0, wxALIGN_CENTER_VERTICAL);

        auto* padding = new wxBoxSizer(wxVERTICAL);
        padding->Add(row, 1, wxEXPAND | wxALL, FromDIP(8));
        card->SetSizer(padding);
        rows->Add(card, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        m_choices.push_back(choice);
        m_rows.push_back(card);
    }
    auto* notice_panel = new wxPanel(m_scroll);
    notice_panel->SetBackgroundColour(m_scroll->GetBackgroundColour());
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
    // The row says something is wrong; the picker, naming the slot's material in
    // the warning colour, says what. A related material (PLA+ for PLA) is no conflict.
    const wxColour normal_bg = m_scroll->GetBackgroundColour();
    const bool dark = wxGetApp().dark_mode();
    const wxColour warning_bg = dark ? wxColour("#3A2E1E") : wxColour("#FFF4E3");
    const unsigned long warning = dark ? WARNING_DARK : WARNING_RGB;
    bool external = false;
    for (size_t i = 0; i < m_choices.size(); ++i) {
        bool conflict = false;
        const int selected = m_choices[i]->GetSelection();
        if (selected > 0) {
            const auto& slot = m_slots[selected - 1];
            external |= slot.external;
            conflict = box_materials_conflict(m_tools[i].material, slot.material);
            // The closed picker has room for one label, so the warning sign joins it.
            const wxString label = m_choices[i]->GetString(selected);
            m_choices[i]->SetLabel(conflict ? label + wxString::FromUTF8(" ⚠") : label);
        }
        const wxColour bg = conflict ? warning_bg : normal_bg;
        m_rows[i]->SetBorderColor(StateColor(std::make_pair(conflict ? warning : ROW_BORDER_RGB, (int) StateColor::Normal)));
        // A plain StaticBox has no background state for SetBackgroundColorNormal to change.
        m_rows[i]->SetBackgroundColor(StateColor(bg));
        for (wxWindow* child : m_rows[i]->GetChildren())
            if (child != m_choices[i])
                child->SetBackgroundColour(bg);
        m_choices[i]->SetLabelColor(StateColor(std::make_pair(0x6B6B6BUL, (int) StateColor::Disabled),
                                               std::make_pair(conflict ? warning : LABEL_RGB, (int) StateColor::Normal)));
        m_rows[i]->Refresh();
    }
    m_notice->GetParent()->Show(external);
    m_notice->GetParent()->Layout();
    m_scroll->SetMinSize(wxSize(FromDIP(420), std::min(FromDIP(360), m_scroll->GetSizer()->CalcMin().y)));
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
