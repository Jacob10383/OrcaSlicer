#include "CrealityPrintAgent.hpp"
#include "CrealityPrint.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

using json = nlohmann::json;

namespace Slic3r {

namespace {

constexpr const char* CrealityPrintAgent_VERSION = "0.1.0";

std::string to_lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has_visible_base_preset(const PresetCollection& filaments, const std::string& filament_id)
{
    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible
            && filaments.get_preset_base(p) == &p
            && p.filament_id == filament_id)
            return true;
    }
    return false;
}

// Lower-case words of a preset name with the "@scope" suffix dropped:
// "Generic PLA Matte @Creality K2-all" -> {"generic", "pla", "matte"}.
std::vector<std::string> name_words(const std::string& name)
{
    std::vector<std::string> words;
    std::string              word;
    for (char c : name.substr(0, name.find('@'))) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    if (!word.empty())
        words.push_back(word);
    return words;
}

} // namespace

// Score visible compatible filament presets against the CFS spool metadata and
// return the exact name of the best-matching preset. An exact case-insensitive
// CFS name is authoritative and matched before type scoring so a bridge can
// preserve an explicitly configured profile. Otherwise scoring is:
//   +20  preset name contains brand_name as a substring
//        (e.g. "Hyper PLA" in "Hyper PLA @Creality K2 0.4 nozzle")
//   +10  the preset belongs to the spool's vendor - by its owning VendorProfile OR by
//        its name. The profile test is what finds the vendor's own generics, which are
//        named "Generic <material> @<scope>" and do not repeat the vendor; the name test
//        still finds a third party filament shipped inside that vendor's bundle, which
//        carries the bundle owner's profile but names its real brand.
//    -5  per word of the preset name the spool never mentioned ("generic" excepted -
//        it marks the unbranded base product rather than a qualifier), so the least
//        specific preset that still explains the spool wins. Without it a spool
//        reporting only "PLA" scores "Generic PLA High Speed" and "Generic PLA Matte"
//        exactly as high as "Generic PLA"; those are three products with three
//        filament_ids, so whichever sorted first won and the printer got the wrong
//        one. Applied after the score gate, so it only reorders genuine matches.
//   Tiebreak: prefer user presets, then sort by name for a deterministic winner.
//   Return the exact name so a user preset keeps its tuning even when it shares
//   a filament_id with the system preset.
// Requires the preset's declared filament_type to equal the spool's base type
// (PLA/PETG/ABS/...) for heuristic matches so we never auto-pick a PETG preset
// for a PLA spool. Falls back to filaments.filament_id_by_type(base_type) when
// nothing scores; the downstream resolver retains its legacy ID lookup for that
// generic fallback and for agents that still supply filament IDs.
std::string CrealityPrintAgent::match_filament_preset(const PresetCollection& filaments,
                                                      const std::string&      vendor,
                                                      const std::string&      brand_name,
                                                      const std::string&      base_type)
{
    const std::string vendor_lower = to_lower(vendor);
    const std::string brand_lower  = to_lower(brand_name);
    const std::string type_lower   = to_lower(base_type);

    // Everything the spool told us about itself, as words. A word in a preset's name
    // that is not in here is a qualifier the spool never claimed.
    std::set<std::string> spool_words{"generic"};
    for (const std::string& src : {brand_lower, vendor_lower, type_lower})
        for (auto& w : name_words(src))
            spool_words.insert(std::move(w));

    struct Match {
        const Preset* preset;
        int           score;
        bool          is_user;
    };
    std::vector<Match> matches;

    int considered = 0;
    for (const auto& p : filaments.get_presets()) {
        if (!p.is_visible || !p.is_compatible) continue;
        // Note: we deliberately do NOT filter on get_preset_base(p) == &p.
        // K2 owners frequently keep tweaked copies of system presets
        // (e.g. "Creality Hyper PLA @K2 (Harky)" with their per-spool PA),
        // which are derived presets — filtering to bases-only would skip
        // exactly the presets users care about most.
        ++considered;

        const std::string name_lower = to_lower(p.name);
        if (!brand_lower.empty() && brand_lower != type_lower && name_lower == brand_lower) {
            BOOST_LOG_TRIVIAL(info)
                << "CrealityPrintAgent: exact CFS preset name \"" << brand_name
                << "\" -> \"" << p.name << "\"";
            return p.name;
        }

        std::string preset_type;
        if (const auto* ft = p.config.option<ConfigOptionStrings>("filament_type"))
            if (!ft->values.empty()) preset_type = ft->values.front();
        if (to_lower(preset_type) != type_lower) continue;

        int score = 0;
        if (!brand_lower.empty() && brand_lower != type_lower && name_lower.find(brand_lower) != std::string::npos)
            score += 20;
        // Profile OR name - neither alone covers both the vendor's own generics and the
        // third party filaments shipped inside its bundle. See the header comment.
        if (!vendor_lower.empty()
            && ((p.vendor != nullptr && to_lower(p.vendor->name) == vendor_lower)
                || name_lower.find(vendor_lower) != std::string::npos))
            score += 10;

        if (score == 0) continue;

        for (const auto& w : name_words(p.name))
            if (spool_words.count(w) == 0)
                score -= 5;

        matches.push_back({&p, score, !p.is_system && !p.is_default});
    }

    if (matches.empty()) {
        const std::string fallback = filaments.filament_id_by_type(base_type);
        const bool        fallback_ok = has_visible_base_preset(filaments, fallback);
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: no preset scored for spool {" << vendor << " "
            << brand_name << " (" << base_type << ")} after considering " << considered
            << " presets; falling back to generic preset id \"" << fallback << "\""
            << (fallback_ok ? "" : " (NOT visible — returning empty)");
        return fallback_ok ? fallback : std::string();
    }

    std::sort(matches.begin(), matches.end(),
              [](const Match& a, const Match& b) {
                  if (a.score   != b.score)   return a.score > b.score;
                  if (a.is_user != b.is_user) return a.is_user; // prefer user over system
                  return a.preset->name < b.preset->name;        // keep the winner deterministic
              });

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: matched spool {" << vendor << " " << brand_name
        << " (" << base_type << ")} -> preset \"" << matches.front().preset->name
        << "\" (score=" << matches.front().score
        << ", " << matches.size() << " candidate(s) of " << considered << " considered)";

    return matches.front().preset->name;
}

CrealityPrintAgent::CrealityPrintAgent(std::string log_dir)
    : MoonrakerPrinterAgent(std::move(log_dir))
{
}

AgentInfo CrealityPrintAgent::get_agent_info_static()
{
    return AgentInfo{
        "crealityprint",
        "CrealityPrint",
        CrealityPrintAgent_VERSION,
        "Creality K-series printer agent (CFS-aware filament sync)"
    };
}

std::string CrealityPrintAgent::normalize_filament_type(const std::string& filament_type)
{
    static const std::vector<std::string> bases = {
        "PETG", "PET", "PLA", "ABS", "ASA", "TPU", "PC", "PA", "PVA", "HIPS"
    };
    for (const auto& base : bases) {
        if (filament_type.rfind(base, 0) == 0) return base;
    }
    return filament_type;
}

// Parse the boxsInfo JSON returned by CrealityPrint::query_boxes_info().
// Schema (verified 2026-05-06 against K2 Combo F021 firmware v1.1.260206):
//   { "boxsInfo": { "materialBoxs": [
//     { "id": int, "state": int, "type": int,    // type 0 = CFS, 1 = single-spool external
//       "external": bool,                           // optional bridge compatibility marker
//       "materials": [
//         { "id": int, "state": int,             // state 1 = loaded
//           "vendor": str, "type": str, "name": str,
//           "color": "#0RRGGBB" }, ...
//       ]}, ...
//   ]}}
bool CrealityPrintAgent::parse_cfs_response(const std::string&    response,
                                            std::vector<CFSSlot>& slots,
                                            int&                  box_count,
                                            bool&                 external_present,
                                            std::string&          error)
{
    using nlohmann::json;

    slots.clear();
    box_count = 0;
    external_present = false;

    if (response.empty()) {
        error = "empty response";
        return false;
    }

    json resp;
    try {
        resp = json::parse(response);
    } catch (const std::exception& e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }

    if (!resp.contains("boxsInfo") || !resp["boxsInfo"].contains("materialBoxs")) {
        error = "invalid schema (missing boxsInfo.materialBoxs)";
        return false;
    }

    // Renumber physical CFS boxes sequentially; raw ids reserve 0 for external.
    int cfs_count = 0;
    CFSSlot external_slot;
    bool external_loaded = false;
    for (const auto& box : resp["boxsInfo"]["materialBoxs"]) {
        const int box_st   = box.value("state", 0);
        const int box_type = box.value("type",  0);
        const bool is_external = box_type == 1 || box.value("external", false);
        if (box_st != 1) continue;
        if (box_type != 0 && !is_external) continue;

        external_present |= is_external;
        const int cfs_index = is_external ? -1 : cfs_count++;

        if (!box.contains("materials") || !box["materials"].is_array())
            continue;

        for (const auto& mat : box["materials"]) {
            // CFS slot state encoding observed across K2 family firmwares:
            //   * K2 (base) / K2 Pro    : 0 = empty, 1 = loaded.
            //   * K2 Plus (1.1.5.5/CFS 1.4.2 onwards): 0 = empty,
            //                                          1 = loaded AND currently
            //                                              selected as the active
            //                                              spool for printing,
            //                                          2 = loaded but not selected.
            // We treat anything non-zero as loaded. Belt-and-braces: also skip
            // entries that look blank (no vendor and no type) regardless of state.
            const int s_state = mat.value("state", 0);
            const std::string s_vendor = mat.value("vendor", std::string());
            const std::string s_type   = mat.value("type",   std::string());
            if (s_state == 0)                       continue; // explicitly empty
            if (s_vendor.empty() && s_type.empty()) continue; // blank entry — likely empty under a different state encoding

            CFSSlot s;
            s.box_id        = cfs_index;
            s.slot_id       = mat.value("id",     0);
            s.vendor        = s_vendor;
            s.brand_name    = mat.value("name",   "");
            s.filament_type = s_type;
            s.color_hex     = mat.value("color",  "#FFFFFF");

            // Creality reports colour as "#0RRGGBB" (8 chars with a leading zero
            // after '#'). Normalise to standard "#RRGGBB".
            if (s.color_hex.size() == 8 && s.color_hex[0] == '#')
                s.color_hex = "#" + s.color_hex.substr(2);

            if (is_external) {
                external_slot = std::move(s);
                external_loaded = true;
                break;
            }
            slots.push_back(std::move(s));
        }
    }

    box_count = cfs_count;
    if (external_loaded) {
        external_slot.box_id = cfs_count;
        external_slot.slot_id = 0;
        slots.push_back(std::move(external_slot));
    }
    return true;
}

bool CrealityPrintAgent::fetch_filament_info(std::string)
{
    if (fetch_box_filament_info())
        return true;

    if (device_info.dev_ip.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent::fetch_filament_info: no device IP, falling back to base agent";
        return fetch_standard_filament_info();
    }

    // Build a CrealityPrint helper so we can use its model detection + WS helpers
    // (added in upstream PR #13291).
    DynamicPrintConfig cfg;
    cfg.set_key_value("print_host",                  new ConfigOptionString("http://" + device_info.dev_ip));
    cfg.set_key_value("print_host_webui",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_cafile",            new ConfigOptionString(""));
    cfg.set_key_value("printhost_port",              new ConfigOptionString(""));
    cfg.set_key_value("printhost_apikey",            new ConfigOptionString(device_info.api_key));
    cfg.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));

    CrealityPrint host(&cfg);

    // Defer to base if this isn't a K-series board with CFS firmware support.
    if (!host.supports_multi_color_print()) {
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: " << host.model_name()
            << " is not CFS-capable, deferring to base Moonraker agent";
        return fetch_standard_filament_info();
    }

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: querying CFS slots on " << host.model_name();

    const std::string response = host.query_boxes_info();

    std::vector<CFSSlot> slots;
    int                  box_count = 0;
    bool                 external_present = false;
    std::string          parse_err;
    if (!parse_cfs_response(response, slots, box_count, external_present, parse_err)) {
        BOOST_LOG_TRIVIAL(warning)
            << "CrealityPrintAgent: CFS query failed (" << parse_err << "), "
            << "falling back to base agent";
        return fetch_standard_filament_info();
    }

    if (box_count == 0 && !external_present) {
        // No active CFS boxes attached — printer is in direct-spool mode. Let the
        // base agent take over so the user still gets whatever filament info
        // Moonraker exposes.
        BOOST_LOG_TRIVIAL(info)
            << "CrealityPrintAgent: no active CFS boxes, deferring to base agent";
        return fetch_standard_filament_info();
    }

    BOOST_LOG_TRIVIAL(info)
        << "CrealityPrintAgent: " << box_count << " CFS box(es), "
        << (external_present ? "external holder, " : "")
        << slots.size() << " loaded slot(s)";

    // Index loaded slots by (box, slot), with external appended after the CFS grid.
    std::map<std::pair<int, int>, const CFSSlot*> by_position;
    for (const auto& s : slots)
        by_position[{s.box_id, s.slot_id}] = &s;

    auto* bundle = GUI::wxGetApp().preset_bundle;

    const int total_slots = box_count * 4 + (external_present ? 1 : 0);
    const int ams_count = box_count + (external_present ? 1 : 0);
    std::vector<AmsTrayData> trays;
    trays.reserve(total_slots);

    for (int slot_index = 0; slot_index < total_slots; ++slot_index) {
        const int box = slot_index / 4;
        const int idx = slot_index % 4;
        AmsTrayData tray;
        tray.slot_index = slot_index;

        auto it = by_position.find({box, idx});
        if (it == by_position.end()) {
            tray.has_filament = false;
            trays.push_back(std::move(tray));
            continue;
        }

        const CFSSlot& s = *it->second;
        tray.has_filament = true;
        tray.tray_type    = normalize_filament_type(s.filament_type);
        tray.tray_color   = s.color_hex;

        if (bundle) {
            tray.tray_info_idx = match_filament_preset(
                bundle->filaments, s.vendor, s.brand_name, tray.tray_type);
        }

        trays.push_back(std::move(tray));
    }

    build_ams_payload(ams_count, total_slots - 1, trays);
    return true;
}

} // namespace Slic3r
