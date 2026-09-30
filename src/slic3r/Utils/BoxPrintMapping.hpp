#pragma once

#include "BoxFilamentSync.hpp"
#include <functional>
#include <map>
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

// Fill each tool with the offered slot holding the closest matching filament
// (color within a shade, same or related material). Two tools matching only one
// slot share it. A tool with no match keeps the slot with its own number when
// offered, and is left out otherwise.
BoxPrintMapping suggest_box_print_mapping(const std::vector<BoxPrintTool>& tools,
                                          const std::vector<BoxFilamentSlot>& slots);

std::string serialize_box_print_mapping(const BoxPrintMapping& mapping);
bool parse_box_print_mapping(const std::string& serialized, BoxPrintMapping& mapping, std::string& error);

// Inspect the uploaded file, verify its tool IDs match the map, then start it.
// Box rereads the file at start and rejects a map that no longer matches.
// A selected map is never discarded. With no selection, ordinary start is only
// allowed for unsupported firmware or a file with no filament usage metadata.
// `cancelled` is polled between requests. `starting` runs just before the start
// request; it returns false to cancel, and the start cannot be cancelled after.
bool start_box_print(const Moonraker& host, const std::string& filename, const std::string& serialized_mapping,
                     std::string& error, const std::function<bool()>& cancelled = {},
                     const std::function<bool()>& starting = {});

} // namespace Slic3r
