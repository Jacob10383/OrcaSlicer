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

// A choice the printer offers when a print starts. The printer declares its
// options in printer.print_control.options and the dialog draws a control for
// each, so a new option needs no change here.
struct PrintOption
{
    enum class Type { Switch, Minutes };
    std::string id;
    std::string label;
    // The heading the option is grouped under.
    std::string section;
    std::string hint;
    // What the time of an optional option means, shown next to it.
    std::string value_label;
    Type type = Type::Switch;
    // A minutes option with a switch: off unless it is turned on.
    bool optional = false;
    int min = 0;
    int max = 1;
    int step = 1;
    // Minutes offered, or 1 and 0 for a switch that starts on and off.
    int default_value = 0;
};

struct PrintChoice
{
    bool on = false;
    int minutes = 0;
};

// What the dialog shows first: the printer's default, with an optional time off.
PrintChoice default_print_choice(const PrintOption& option);

// "PRINT_OPTIONS ..." with what differs from the defaults, or empty to leave
// everything to the printer. An untouched option is not sent, so the printer's
// own setting and the file's start parameters still apply to it. `choices`
// holds one entry per option.
std::string print_options_command(const std::vector<PrintOption>& options, const std::vector<PrintChoice>& choices);

// The longest one-time heat soak the print dialog offers. A printer whose own
// heat_soak is longer is left to its setting, as in Fluidd.
constexpr int MAX_HEAT_SOAK_MINUTES = 240;

struct PrintStartOptions
{
    std::vector<PrintOption> options;
    // The heat_soak setting of a printer whose START_PRINT takes soak_once but
    // which has no print options.
    std::optional<int> soak_once_default;
};

// Never fails: an unreachable printer, an older firmware or another printer
// offers nothing. `done` runs on the HTTP thread.
void fetch_print_start_options_async(const Moonraker& host, std::function<void(const PrintStartOptions&)> done);

// What the print dialog chose besides the Box map.
struct PrintStartChoices
{
    // A PRINT_OPTIONS line, sent ahead of the start command in the same script:
    // an error in it stops the script before the print starts.
    std::string print_options;
    // Minutes for START_PRINT's soak_once, set after the start command in the
    // same script: Klipper runs it only if the start succeeds, before the
    // file's first line.
    std::optional<int> soak_once;
    bool empty() const { return print_options.empty() && !soak_once; }
};

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
// `choices` go in the start request itself, so Orca starts the print instead of
// Moonraker when there are any.
bool start_box_print(const Moonraker& host, const std::string& filename, const std::string& serialized_mapping,
                     std::string& error, const std::function<bool()>& cancelled = {},
                     const std::function<bool()>& starting = {}, const PrintStartChoices& choices = {});

} // namespace Slic3r
