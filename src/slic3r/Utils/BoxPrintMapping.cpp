#include "BoxPrintMapping.hpp"
#include "Http.hpp"
#include "Moonraker.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace {
using nlohmann::json;

const char* const STATUS_QUERY = "printer/objects/query?box";

struct Response
{
    bool success = false;
    unsigned status = 0;
    json body;
    std::string error;
};

// Moonraker supplies the URL, authentication and TLS settings. This layer adds
// the JSON body, limits, and envelope validation.
Http make_request(const Moonraker& host, const std::string& path, const json* body, bool probe)
{
    auto http = host.request(path, body != nullptr);
    if (body)
        http.header("Content-Type", "application/json").set_post_body(body->dump());
    http.timeout_connect(probe ? 2 : 5).timeout_max(probe ? 5 : 30).size_limit(1024 * 1024);
    return http;
}

void on_response(Http& http, std::function<void(const Response&)> finished)
{
    http.on_complete([finished](std::string text, unsigned status) {
            Response response;
            response.status = status;
            response.body = json::parse(text, nullptr, false);
            if (response.body.is_discarded() || !response.body.is_object())
                response.error = "Invalid Moonraker JSON response.";
            else if (response.body.contains("error"))
                response.error = "Moonraker: " + response.body["error"].dump();
            else if (!response.body.contains("result"))
                response.error = "Moonraker response is missing its result.";
            else
                response.success = true;
            finished(response);
        })
        .on_error([finished](std::string text, std::string detail, unsigned status) {
            Response response;
            response.status = status;
            response.error = "Moonraker request failed (HTTP " + std::to_string(status) + "): " + detail;
            const auto result = json::parse(text, nullptr, false);
            if (result.is_object() && result.contains("error"))
                response.error += " " + result["error"].dump();
            finished(response);
        });
}

Response send(const Moonraker& host, const std::string& path, const json* body, bool probe = false)
{
    Response response;
    auto http = make_request(host, path, body, probe);
    on_response(http, [&response](const Response& result) { response = result; });
    http.perform_sync();
    return response;
}

bool request(const Moonraker& host, const std::string& path, const json* body, json& result, std::string& error)
{
    const Response response = send(host, path, body);
    result = response.body;
    error = response.error;
    return response.success;
}

bool script(const Moonraker& host, const std::string& script, std::string& error)
{
    const json body = {{"script", script}};
    json response;
    return request(host, "printer/gcode/script", &body, response, error);
}

bool ordinary_start(const Moonraker& host, const std::string& filename, std::string& error)
{
    const json body = {{"filename", filename}};
    json response;
    return request(host, "printer/print/start", &body, response, error);
}

bool valid_index(const json& value, int maximum)
{
    return value.is_number_integer() && value >= 0 && value <= maximum;
}

bool parse_status(const json& response, BoxPrintStatus& status, std::string& error)
{
    status = {};
    try {
        const auto& objects = response.at("result").at("status");
        if (!objects.is_object())
            throw std::runtime_error("Invalid Moonraker object status.");
        if (!objects.contains("box"))
            return true;
        const auto& box = objects.at("box");
        if (!box.is_object())
            throw std::runtime_error("Invalid Box status.");
        if (!box.contains("print_mapping_version"))
            return true;
        const auto& version = box.at("print_mapping_version");
        if (!version.is_number_integer())
            throw std::runtime_error("Invalid Box print mapping version.");
        if (version != 1)
            return true;
        status.supported = true;
        if (box.at("data_ready") != true || box.at("driver_ready") != true)
            throw std::runtime_error("Box filament slots are not ready.");
        const auto& slots = box.at("slots");
        if (!slots.is_array() || slots.empty())
            throw std::runtime_error("Invalid Box filament slots.");
        std::set<int> indices;
        // Printing needs raw slot identities, not the complete CFS topology
        // required by the filament-sync parser and its AMS conversion.
        for (const auto& item : slots) {
            if (!valid_index(item.at("index"), std::numeric_limits<int>::max()) ||
                !item.at("present").is_boolean() || !item.at("external").is_boolean())
                throw std::runtime_error("Invalid Box slot fields.");
            BoxFilamentSlot slot;
            slot.index = item.at("index").get<int>();
            if (!indices.insert(slot.index).second)
                throw std::runtime_error("Duplicate Box slot index.");
            slot.present = item.at("present").get<bool>();
            slot.external = item.at("external").get<bool>();
            slot.color = item.value("color", std::string());
            slot.material = item.value("material", std::string());
            slot.name = item.value("name", std::string());
            slot.brand = item.value("brand", std::string());
            status.slots.push_back(std::move(slot));
        }
        std::sort(status.slots.begin(), status.slots.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
        return true;
    } catch (const std::exception& ex) {
        status.slots.clear();
        error = std::string("Cannot read Box print status: ") + ex.what();
        return false;
    }
}

bool probe_result(const Response& response, BoxPrintStatus& status, std::string& error)
{
    status = {};
    error.clear();
    if (!response.success) {
        // Old Moonraker-compatible hosts may not implement object queries.
        if (response.status == 404 || response.status == 501)
            return true;
        error = response.error;
        return false;
    }
    return parse_status(response.body, status, error);
}

// Weighted OKLab distance. Lightness counts half, so a darker or lighter shade
// of the same color still matches.
constexpr double COLOR_MATCH_LIMIT = 0.12;
// Hue must also agree unless a color is nearly gray: common reds span about
// 10 degrees, and red to orange is more.
constexpr double HUE_MATCH_LIMIT = 10.;
constexpr double NEUTRAL_CHROMA = 0.05;

std::string trim(const std::string& value)
{
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), space);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), space).base();
    return first < last ? std::string(first, last) : std::string();
}
// A related material (PLA+ for PLA) or an unset one matches, behind an exact one.
constexpr double MATERIAL_FAMILY_PENALTY = 0.05;

bool oklab(const std::string& color, std::array<double, 3>& lab)
{
    std::string hex = trim(color);
    if (!hex.empty() && hex.front() == '#')
        hex.erase(0, 1);
    if ((hex.size() != 6 && hex.size() != 8) ||
        !std::all_of(hex.begin(), hex.end(), [](unsigned char c) { return std::isxdigit(c); }))
        return false;
    double rgb[3];
    for (int i = 0; i < 3; ++i) {
        const double value = std::stoi(hex.substr(i * 2, 2), nullptr, 16) / 255.;
        rgb[i] = value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    }
    const double l = std::cbrt(0.4122214708 * rgb[0] + 0.5363325363 * rgb[1] + 0.0514459929 * rgb[2]);
    const double m = std::cbrt(0.2119034982 * rgb[0] + 0.6806995451 * rgb[1] + 0.1073969566 * rgb[2]);
    const double s = std::cbrt(0.0883024619 * rgb[0] + 0.2817188376 * rgb[1] + 0.6299787005 * rgb[2]);
    lab = {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
           1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
           0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
    return true;
}

// Negative when either color is unknown.
double color_distance(const std::string& a, const std::string& b)
{
    std::array<double, 3> first, second;
    if (!oklab(a, first) || !oklab(b, second))
        return -1.;
    if (std::hypot(first[1], first[2]) >= NEUTRAL_CHROMA && std::hypot(second[1], second[2]) >= NEUTRAL_CHROMA) {
        const double hue = std::abs(std::atan2(first[2], first[1]) - std::atan2(second[2], second[1])) * 180. / M_PI;
        if (std::min(hue, 360. - hue) > HUE_MATCH_LIMIT)
            return -1.;
    }
    return std::sqrt(std::pow((first[0] - second[0]) / 2., 2) + std::pow(first[1] - second[1], 2) +
                     std::pow(first[2] - second[2], 2));
}

// Negative when the materials differ; otherwise the cost of pairing them.
double material_cost(const std::string& a, const std::string& b)
{
    const auto upper = [](const std::string& material) {
        std::string result = trim(material);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return result;
    };
    const auto exact = [](std::string material) {
        material.erase(std::remove_if(material.begin(), material.end(), [](unsigned char c) {
            return std::isspace(c) || c == '-' || c == '_';
        }), material.end());
        return material;
    };
    const std::string first = upper(a), second = upper(b);
    if (first.empty() || second.empty())
        return MATERIAL_FAMILY_PENALTY;
    if (exact(first) == exact(second))
        return 0.;
    // The family is the leading name: PLA for PLA+, PLA-CF and PLA Matte.
    const auto family = [](const std::string& material) {
        return material.substr(0, std::find_if(material.begin(), material.end(),
            [](unsigned char c) { return !std::isalpha(c); }) - material.begin());
    };
    const std::string first_family = family(first);
    return !first_family.empty() && first_family == family(second) ? MATERIAL_FAMILY_PENALTY : -1.;
}

bool quote_gcode(const std::string& value, std::string& quoted, std::string& error)
{
    quoted = "\"";
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') {
            error = "Box print parameters contain a line break or null character.";
            return false;
        }
        if (c == '\\' || c == '"')
            quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    return true;
}
} // namespace

bool fetch_box_print_status(const Moonraker& host, BoxPrintStatus& status, std::string& error)
{
    return probe_result(send(host, STATUS_QUERY, nullptr, true), status, error);
}

void fetch_box_print_status_async(const Moonraker& host, BoxPrintStatusFn done)
{
    auto http = make_request(host, STATUS_QUERY, nullptr, true);
    on_response(http, [done](const Response& response) {
        BoxPrintStatus status;
        std::string error;
        const bool fetched = probe_result(response, status, error);
        done(fetched, status, error);
    });
    http.perform();
}

BoxPrintMapping suggest_box_print_mapping(const std::vector<BoxPrintTool>& tools,
                                          const std::vector<BoxFilamentSlot>& slots)
{
    struct Candidate
    {
        int tool;
        int slot;
        // Ties go to the tool's own slot number, then Box slots before External.
        std::tuple<double, bool, bool, int> rank;
    };
    std::vector<Candidate> candidates;
    for (const auto& tool : tools)
        for (const auto& slot : slots) {
            const double distance = color_distance(tool.color, slot.color);
            const double material = material_cost(tool.material, slot.material);
            if (distance < 0. || distance > COLOR_MATCH_LIMIT || material < 0.)
                continue;
            candidates.push_back({tool.id, slot.index,
                                  {distance + material, slot.index != tool.id, slot.external, slot.index}});
        }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.rank < b.rank; });
    BoxPrintMapping mapping;
    std::set<int> used;
    for (const bool shared : {false, true})
        for (const auto& candidate : candidates) {
            if (mapping.count(candidate.tool) || (!shared && used.count(candidate.slot)))
                continue;
            mapping.emplace(candidate.tool, candidate.slot);
            used.insert(candidate.slot);
        }
    for (const auto& tool : tools)
        if (!mapping.count(tool.id) &&
            std::any_of(slots.begin(), slots.end(), [&](const auto& slot) { return slot.index == tool.id; }))
            mapping.emplace(tool.id, tool.id);
    return mapping;
}

std::string serialize_box_print_mapping(const BoxPrintMapping& mapping)
{
    json result = json::object();
    for (const auto& [tool, slot] : mapping)
        result[std::to_string(tool)] = slot;
    return result.dump();
}

bool parse_box_print_mapping(const std::string& serialized, BoxPrintMapping& mapping, std::string& error)
{
    mapping.clear();
    error.clear();
    const auto value = json::parse(serialized, nullptr, false);
    if (value.is_object() && !value.empty()) {
        BoxPrintMapping parsed;
        for (auto it = value.begin(); it != value.end(); ++it) {
            try {
                const int tool = std::stoi(it.key());
                if (tool < 0 || tool > 255 || std::to_string(tool) != it.key() ||
                    !valid_index(it.value(), std::numeric_limits<int>::max()))
                    break;
                parsed.emplace(tool, it.value().get<int>());
            } catch (const std::exception&) {
                break;
            }
        }
        if (parsed.size() == value.size()) {
            mapping = std::move(parsed);
            return true;
        }
    }
    error = "Invalid Box print mapping. Select a slot for every used tool.";
    return false;
}

bool start_box_print(const Moonraker& host, const std::string& filename, const std::string& serialized_mapping,
                     std::string& error, const std::function<bool()>& cancelled,
                     const std::function<bool()>& starting)
{
    error.clear();
    auto is_cancelled = [&] {
        if (cancelled && cancelled()) {
            error = "Box print cancelled.";
            return true;
        }
        return false;
    };
    auto start_cancelled = [&] {
        if (is_cancelled())
            return true;
        if (starting && !starting()) {
            error = "Box print cancelled.";
            return true;
        }
        return false;
    };
    if (is_cancelled())
        return false;
    const bool selected = !serialized_mapping.empty();
    BoxPrintMapping mapping;
    if (selected && !parse_box_print_mapping(serialized_mapping, mapping, error))
        return false;
    std::string quoted_filename;
    if (filename.empty() || !quote_gcode(filename, quoted_filename, error)) {
        if (error.empty())
            error = "Moonraker did not return an uploaded filename.";
        return false;
    }
    BoxPrintStatus status;
    if (!fetch_box_print_status(host, status, error) || is_cancelled())
        return false;
    if (!status.supported) {
        if (selected) {
            error = "This printer no longer supports the selected Box print mapping.";
            return false;
        }
        return !start_cancelled() && ordinary_start(host, filename, error);
    }
    if (!script(host, "BOX_PRINT_INFO FILENAME=" + quoted_filename, error) || is_cancelled())
        return false;
    json response;
    if (!request(host, "printer/objects/query?box=print_info", nullptr, response, error) || is_cancelled())
        return false;
    try {
        const auto& info = response.at("result").at("status").at("box").at("print_info");
        if (info.at("filename") != filename)
            throw std::runtime_error("The inspected filename changed. Reopen the print dialog.");
        const auto& tools = info.at("tools");
        if (!tools.is_array())
            throw std::runtime_error("Invalid Box file tools.");
        if (tools.empty() && !selected)
            return !start_cancelled() && ordinary_start(host, filename, error);
        std::set<int> used;
        for (const auto& tool : tools) {
            if (!valid_index(tool.at("tool"), 255) || !used.insert(tool.at("tool").get<int>()).second)
                throw std::runtime_error("Invalid or duplicate Box file tool ID.");
        }
        if (!selected || used.size() != mapping.size() ||
            !std::all_of(mapping.begin(), mapping.end(), [&](const auto& entry) { return used.count(entry.first); }))
            throw std::runtime_error("Uploaded file tools differ from the selected Box mapping. Reopen the print dialog.");
        std::string map;
        for (const auto& [tool, slot] : mapping) {
            if (!map.empty())
                map += ',';
            map += std::to_string(tool) + ':' + std::to_string(slot);
        }
        if (start_cancelled())
            return false;
        return script(host, "BOX_PRINT_START FILENAME=" + quoted_filename + " MAP=\"" + map + "\"", error);
    } catch (const std::exception& ex) {
        error = std::string("Cannot start Box print: ") + ex.what();
        return false;
    }
}
} // namespace Slic3r
