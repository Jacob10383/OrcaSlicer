#pragma once

#include <string>
#include <vector>

namespace Slic3r {

// box_toolchanger API v1, queried through Moonraker's printer/objects/query.
// Indices are physical T numbers, including gaps between disconnected boxes.
struct BoxFilamentSlot
{
    int         index = 0;
    bool        present = false;
    bool        external = false;
    std::string material;
    std::string color;
    std::string brand;
    std::string name;

    // API v1 always marks the external holder present, including when it has
    // no known filament profile. Physical presence comes from the slot sensor.
    bool has_filament() const { return present && (!external || !material.empty() || !name.empty()); }
};

// On failure, slots is empty and error explains why the caller should fall back.
bool parse_box_filament_status(const std::string& response, std::vector<BoxFilamentSlot>& slots, std::string& error);

bool fetch_box_filament_status(const std::string& base_url, const std::string& api_key,
                               std::vector<BoxFilamentSlot>& slots, std::string& error);

} // namespace Slic3r
