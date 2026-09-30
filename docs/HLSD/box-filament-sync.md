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

## Print slot mapping

Moonraker printers advertising `box.print_mapping_version == 1` offer slot
mapping in the legacy `PrintHostSendDialog` when uploading the current sliced
plate as G-code. The printer-agent print path does not handle Box print mapping.
Upload-only ignores the selections and remains available when mapping is
incomplete.

The dialog opens immediately. The panel checks `printer.objects.query?box` in
the background and shows "Checking filament slots..." until it answers. Upload and
Print stays disabled until then. A printer without Box mapping support hides the
panel and prints normally. A failed check shows the error; Upload remains available.
Requests go through the `Moonraker` host, so URL, API key and TLS settings are shared
with upload.

The panel captures a snapshot of slot metadata when the check completes. It hides
empty physical slots, keeps the sliced plate's original tool IDs, and shows
physical Box/slot names, including the external holder. Orca checks that every used tool has a selection. It does
not validate current slot presence again before starting a print. Firmware alone
validates current slot presence.
Each tool starts on the slot holding matching filament: a similar shade and
hue, and the same or a related material (PLA+ for PLA). Tools that match only
one slot share it. A tool with no match starts on the slot with its own number
when that slot is offered.
Selecting a slot does not sync filament presets or change sliced settings.
Material differences are warnings.

The selected map travels with the legacy Moonraker upload job. After uploading
without auto-start, Orca requests `BOX_PRINT_INFO` for the server's returned
filename from the upload response's `item.path` and checks that the file's used
tools match the selected map. It then calls `BOX_PRINT_START` with the filename and map; Box rereads the file and
rejects a map that no longer matches its tools.
Box validates the current slots and file, installs the mapping, and starts the
print. A rejected mapping never falls back to an ordinary print. G-code is not
rewritten.

The transport uses upstream Moonraker's object-query and G-code-script endpoints.
It does not use the stock Creality mapping protocol or the AMS tray adapter,
which does not retain Box's external-holder distinction.

Tests live in `tests/slic3rutils/test_box_filament_sync.cpp` and
`tests/slic3rutils/test_creality_cfs_match.cpp`. Run the `BoxFilamentSync`, `CFS`,
and `Creality` Catch2 tags in `slic3rutils_tests`.
Print protocol tests are in `tests/slic3rutils/test_box_print_mapping.cpp`, tagged
`BoxPrintMapping`. They use a local HTTP server to check inspection,
tool validation, cancellation, and start failures.
