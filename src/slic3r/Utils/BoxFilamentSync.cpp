#include "BoxFilamentSync.hpp"
#include "Http.hpp"

#include <algorithm>
#include <set>
#include <nlohmann/json.hpp>

namespace Slic3r {

bool fetch_box_filament_status(const std::string& base_url, const std::string& api_key,
                               std::vector<BoxFilamentSlot>& slots, std::string& error)
{
    slots.clear();
    error.clear();
    if (base_url.empty()) {
        error = "no Moonraker URL";
        return false;
    }
    std::string url = base_url;
    if (url.back() != '/')
        url += '/';
    url += "printer/objects/query?box";

    bool success = false;
    auto http = Http::get(url);
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.timeout_connect(2)
        .timeout_max(5)
        .size_limit(1024 * 1024)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200)
                success = parse_box_filament_status(body, slots, error);
            else
                error = "HTTP " + std::to_string(status);
        })
        .on_error([&](std::string, std::string, unsigned status) {
            error = "request failed, HTTP " + std::to_string(status);
        })
        .perform_sync();
    return success;
}

bool parse_box_filament_status(const std::string& response, std::vector<BoxFilamentSlot>& slots, std::string& error)
{
    slots.clear();
    error.clear();
    const auto json = nlohmann::json::parse(response, nullptr, false);
    if (json.is_discarded()) {
        error = "invalid Moonraker JSON";
        return false;
    }

    try {
        const auto& box = json.at("result").at("status").at("box");
        // Versions describe a schema, not a minimum feature level. Unknown
        // versions must fall back until their slot semantics are supported.
        if (!box.at("api_version").is_number_integer() || box.at("api_version") != 1) {
            error = "unsupported box API version";
            return false;
        }
        if (!box.at("data_ready").is_boolean() || box.at("data_ready") != true ||
            !box.at("driver_ready").is_boolean() || box.at("driver_ready") != true) {
            error = "box data is not ready";
            return false;
        }
        const auto& data = box.at("slots");
        // API v1 supports four four-slot boxes and one external holder.
        if (!data.is_array() || data.empty() || data.size() > 17) {
            error = "invalid box slot list";
            return false;
        }

        std::vector<BoxFilamentSlot> parsed;
        std::set<int> indices;
        int max_physical = -1;
        int external_index = -1;
        for (const auto& entry : data) {
            if (!entry.at("index").is_number_integer() || entry.at("index") < 0 || entry.at("index") > 16 ||
                !entry.at("present").is_boolean() || !entry.at("external").is_boolean()) {
                error = "invalid box slot fields";
                return false;
            }
            BoxFilamentSlot slot;
            slot.index    = entry.at("index").get<int>();
            slot.present  = entry.at("present").get<bool>();
            slot.external = entry.at("external").get<bool>();
            slot.material = entry.at("material").get<std::string>();
            slot.color    = entry.at("color").get<std::string>();
            slot.brand    = entry.at("brand").get<std::string>();
            slot.name     = entry.at("name").get<std::string>();
            if (!indices.insert(slot.index).second || (!slot.external && slot.index >= 16) ||
                (slot.external && external_index >= 0)) {
                error = "duplicate or out-of-range box slot";
                return false;
            }
            if (slot.external)
                external_index = slot.index;
            else
                max_physical = std::max(max_physical, slot.index);
            parsed.push_back(std::move(slot));
        }

        // The external holder follows the last physical box, even with gaps.
        if (external_index != max_physical + 1 || external_index % 4 != 0) {
            error = "invalid external holder index";
            return false;
        }
        for (const auto& slot : parsed) {
            if (!slot.external) {
                for (int i = slot.index / 4 * 4; i < slot.index / 4 * 4 + 4; ++i) {
                    if (!indices.count(i)) {
                        error = "incomplete physical box";
                        return false;
                    }
                }
            }
        }
        std::sort(parsed.begin(), parsed.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
        slots = std::move(parsed);
        return true;
    } catch (const nlohmann::json::exception&) {
        error = "missing or malformed box status";
        return false;
    }
}

} // namespace Slic3r
