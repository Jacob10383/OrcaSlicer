#pragma once

#include "BoxFilamentSync.hpp"
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

class Moonraker;

struct BoxPrintTool
{
    int id = 0;
    std::string color;
    std::string material;
    std::string name;
};

using BoxPrintMapping = std::map<int, int>;

struct BoxPrintStatus
{
    bool supported = false;
    std::vector<BoxFilamentSlot> slots;
};

// An absent or unsupported Box is a successful probe with supported=false.
// Connection errors and invalid/unready supported firmware are failures.
bool fetch_box_print_status(const Moonraker& host, BoxPrintStatus& status, std::string& error);

// The same probe without blocking the caller. `done` runs on the HTTP thread.
using BoxPrintStatusFn = std::function<void(bool fetched, const BoxPrintStatus& status, const std::string& error)>;
void fetch_box_print_status_async(const Moonraker& host, BoxPrintStatusFn done);

// Heat soak chosen at print start. The printer offers it when START_PRINT's
// The longest heat soak the print dialogs offer. A printer whose own heat_soak is
// longer is left to its setting, as in Fluidd.
constexpr int MAX_HEAT_SOAK_MINUTES = 240;

// _START_PRINT_VARS has soak_once; `minutes` is its heat_soak setting.
struct HeatSoakStatus
{
    bool supported = false;
    int minutes = 0;
};

// Never fails: an unreachable printer or an older config reports supported=false.
// `done` runs on the HTTP thread.
void fetch_heat_soak_status_async(const Moonraker& host, std::function<void(const HeatSoakStatus&)> done);

// Fill each tool with the offered slot holding the closest matching filament
// (color within a shade, same or related material). Two tools matching only one
// slot share it. A tool with no match keeps the slot with its own number when
// offered, and is left out otherwise.
BoxPrintMapping suggest_box_print_mapping(const std::vector<BoxPrintTool>& tools,
                                          const std::vector<BoxFilamentSlot>& slots);

// Different material families (PLA and PA6-GF). A related material (PLA+ for
// PLA) or an unset one is not a conflict.
bool box_materials_conflict(const std::string& a, const std::string& b);

std::string serialize_box_print_mapping(const BoxPrintMapping& mapping);
bool parse_box_print_mapping(const std::string& serialized, BoxPrintMapping& mapping, std::string& error);

// Inspect the uploaded file, verify its tool IDs match the map, then start it.
// Box rereads the file at start and rejects a map that no longer matches.
// A selected map is never discarded. With no selection, ordinary start is only
// allowed for unsupported firmware or a file with no filament usage metadata.
// `cancelled` is polled between requests. `starting` runs just before the start
// request; it returns false to cancel, and the start cannot be cancelled after.
// `soak_minutes` sets START_PRINT's one-time heat soak in the start request
// itself: Klipper runs it only if the start succeeds, before the file's first line.
bool start_box_print(const Moonraker& host, const std::string& filename, const std::string& serialized_mapping,
                     std::string& error, const std::function<bool()>& cancelled = {},
                     const std::function<bool()>& starting = {}, std::optional<int> soak_minutes = std::nullopt);

} // namespace Slic3r
