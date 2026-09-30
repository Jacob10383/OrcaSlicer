#pragma once

#include "slic3r/Utils/BoxPrintMapping.hpp"
#include <memory>
#include <wx/panel.h>

class ComboBox;
class wxScrolledWindow;
class wxStaticText;

namespace Slic3r {
class DynamicPrintConfig;
namespace GUI {
class PartPlate;

wxDECLARE_EVENT(wxEVT_BOX_PRINT_MAPPING_CHANGED, wxCommandEvent);

// Keep the slice's logical tool IDs, including gaps, without changing presets.
std::vector<BoxPrintTool> collect_box_print_tools(PartPlate* plate, const DynamicPrintConfig& sliced_config);

class BoxPrintMappingPanel : public wxPanel
{
public:
    explicit BoxPrintMappingPanel(wxWindow* parent);
    ~BoxPrintMappingPanel() override;
    // Check Box support in the background, then show the slot choices. The
    // panel hides itself when the printer has no Box mapping support.
    // `plate_error` replaces the choices when mapping is supported.
    void lookup(const Moonraker& host, const std::vector<BoxPrintTool>& tools, const wxString& plate_error = {});
    void set_data(const std::vector<BoxPrintTool>& tools, const std::vector<BoxFilamentSlot>& slots,
                  const wxString& error = {});
    bool complete() const;
    // Upload and Print waits for the check, then needs a slot for every used
    // tool unless the printer has no Box mapping support.
    bool ready_to_print() const;
    // Empty unless the printer supports mapping and every tool has a slot.
    std::string serialized_mapping() const;

private:
    void show_message(const wxString& message);
    void notify_changed();
    void update_selection();
    std::vector<BoxPrintTool> m_tools;
    std::vector<BoxFilamentSlot> m_slots;
    std::vector<ComboBox*> m_choices;
    std::vector<wxStaticText*> m_warnings;
    wxScrolledWindow* m_scroll;
    wxStaticText* m_notice = nullptr;
    bool m_error = false;
    bool m_pending = false;
    bool m_supported = true;
    // Background lookups check this on the UI thread before touching the panel.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace GUI
} // namespace Slic3r
