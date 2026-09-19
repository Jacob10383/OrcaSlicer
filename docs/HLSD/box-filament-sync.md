# Box filament sync

Both the Moonraker and CrealityPrint printer agents query
`GET /printer/objects/query?box` at the configured Moonraker base URL, with the
configured API key. A supported response uses `result.status.box.api_version`
equal to integer `1`, with `data_ready` and `driver_ready` both true.

This is the committed `box_toolchanger` API. Each `slots` entry contains its
physical `index`, `present`, `external`, `material`, `color`, `brand`, and `name`.
`present` describes filament in a slot. `loaded` describes filament selected
into the toolhead and does not control which spools are imported.

Slot indices stay unchanged, including gaps between disconnected CFS units.
Four physical slots belong to each CFS. The external holder follows the highest
physical slot, or has index zero with no CFS. API v1 allows up to sixteen
physical slots and one external holder. An external holder with no material or
profile name is shown as an empty slot. Empty physical slots retain their
positions and do not shift later tool numbers.

Exact profile names take priority, including user profiles and case-insensitive
name matches. Other matches use the shared CFS vendor and material scoring.
Equal scores prefer user presets, then sort by name. A matched preset keeps its
exact name through filament sync; generic fallback continues to use filament
IDs. Mapping and support-material metadata resolve that name back to its
filament family ID when needed.

The direct request has a two-second connection timeout and a five-second total
timeout. Unknown API versions, missing objects, incomplete status, invalid JSON,
invalid topology, and HTTP failures reject the entire response. Partial results
are never published. Each sync retries capability detection, so firmware
upgrades and temporary failures need no saved capability flag.

On a CrealityPrint connection, failure continues through stock model detection
and the port-9999 CFS query, then standard Moonraker lane data and Happy Hare.
On a Moonraker connection, failure continues through lane data and Happy Hare.
A successful direct query returns before stock model detection or port 9999.

Tests live in `tests/slic3rutils/test_box_filament_sync.cpp` and
`tests/slic3rutils/test_creality_cfs_match.cpp`. Run the `BoxFilamentSync`, `CFS`,
and `Creality` Catch2 tags in `slic3rutils_tests`.
