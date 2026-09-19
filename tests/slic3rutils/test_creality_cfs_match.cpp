#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/CrealityPrintAgent.hpp"

using namespace Slic3r;

namespace {

// A standalone filament collection, built the same way PresetBundle builds its own, so the
// CFS matcher can be exercised without loading the shipped profiles or touching a printer.
struct FilamentTestCollection : public PresetCollection
{
    FilamentTestCollection()
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    {}
};

struct FilamentSpec
{
    const char *name;
    const char *filament_id;
    const char *filament_type;
    bool        is_library = false;   // belongs to the Orca Filament Library, not the printer vendor
    bool        is_system  = true;
};

// Load the specs into the collection, then stamp the fields the matcher reads. Done in a second
// pass because load_preset() keeps m_presets sorted, so a reference taken during the first pass
// can be left dangling by a later load.
void populate(PresetCollection &filaments, const std::vector<FilamentSpec> &specs,
              const VendorProfile &vendor, const VendorProfile &library)
{
    for (const FilamentSpec &spec : specs) {
        DynamicPrintConfig config(filaments.default_preset().config);
        config.option<ConfigOptionStrings>("filament_type", true)->values = {spec.filament_type};
        filaments.load_preset(std::string(), spec.name, config, /*select=*/false);
    }
    for (auto it = filaments.begin(); it != filaments.end(); ++it) {
        const auto spec = std::find_if(specs.begin(), specs.end(),
                                       [&](const FilamentSpec &s) { return it->name == s.name; });
        if (spec == specs.end())
            continue;
        it->filament_id  = spec->filament_id;
        it->vendor       = spec->is_library ? &library : &vendor;
        it->is_system    = spec->is_system;
        it->is_default   = false;
        it->is_visible   = true;
        it->is_compatible = true;
    }
}

// What a stock K2 with a 0.4 nozzle sees after the filament_id rework: the vendor's plain generics
// carry no "Creality" in their name (they are "Generic <material> @<scope>"), while the subtype
// variants that do keep a vendor scope are separate products with their own filament_ids.
const std::vector<FilamentSpec> k2_stock_nozzle{
    {"AliZ PLA @System",                       "ALIZ-PLA",     "PLA",  /*is_library=*/true},
    {"Generic PETG @K2-all",                   "PETG-GENERIC", "PETG"},
    {"Generic PLA @K2-all",                    "PLA-GENERIC",  "PLA"},
    {"Generic PLA High Speed @Creality K2-all","PLA-HS",       "PLA"},
    {"Generic PLA Matte @Creality K2-all",     "PLA-MATTE",    "PLA"},
    {"Generic PLA Silk @Creality K2-all",      "PLA-SILK",     "PLA"},
    {"Hyper PLA @K2-all",                      "PLA-HYPER",    "PLA"},
};

std::string match(const std::vector<FilamentSpec> &specs, const std::string &spool_vendor,
                  const std::string &spool_name, const std::string &base_type)
{
    FilamentTestCollection filaments;
    VendorProfile          vendor("Creality");
    VendorProfile          library(PresetBundle::ORCA_FILAMENT_LIBRARY);
    vendor.name  = "Creality";
    library.name = PresetBundle::ORCA_FILAMENT_LIBRARY;
    populate(filaments, specs, vendor, library);
    return CrealityPrintAgent::match_filament_preset(filaments, spool_vendor, spool_name, base_type);
}

} // namespace

TEST_CASE("An explicit CFS profile name preserves the exact preset", "[CFS][Creality]")
{
    const std::vector<FilamentSpec> presets{
        {"My ASA @K2", "ASA-ID", "ASA", false, false},
        {"Generic PLA @K2", "PLA-ID", "PLA"},
    };
    CHECK(match(presets, "", "my asa @k2", "PLA") == "My ASA @K2");
}

TEST_CASE("CFS score ties preserve user tuning and have a stable winner", "[CFS][Creality]")
{
    const std::vector<FilamentSpec> presets{
        {"Hyper PLA @K2", "SHARED-ID", "PLA"},
        {"Hyper PLA @K2 Alice", "SHARED-ID", "PLA", false, false},
        {"Hyper PLA @K2 Zoe", "SHARED-ID", "PLA", false, false},
    };
    CHECK(match(presets, "Creality", "Hyper PLA", "PLA") == "Hyper PLA @K2 Alice");
}

TEST_CASE("A bare material name uses the legacy generic fallback", "[CFS][Creality]")
{
    const std::vector<FilamentSpec> presets{{"Generic PLA @K2", "PLA-ID", "PLA"}};
    CHECK(match(presets, "", "PLA", "PLA") == "PLA-ID");
}

TEST_CASE("Stock CFS sync retains empty positions and appends the external spool", "[CFS][Creality]")
{
    const std::string response = R"({"boxsInfo":{"materialBoxs":[
        {"id":0,"state":1,"type":1,"materials":[{"id":0,"state":1,"type":"ASA","name":"My ASA","color":"#012AB34"}]},
        {"id":1,"state":1,"type":0,"materials":[{"id":2,"state":1,"type":"PLA","name":"My PLA","color":"#123456"}]}
    ]}})";
    std::vector<CrealityPrintAgent::CFSSlot> slots;
    int box_count = 0;
    bool external_present = false;
    std::string error;
    REQUIRE(CrealityPrintAgent::parse_cfs_response(response, slots, box_count, external_present, error));
    REQUIRE(slots.size() == 2);
    CHECK(box_count == 1);
    CHECK(external_present);
    CHECK(slots[0].box_id == 0);
    CHECK(slots[0].slot_id == 2);
    CHECK(slots[1].box_id == 1);
    CHECK(slots[1].slot_id == 0);
    CHECK(slots[1].color_hex == "#12AB34");
}

// Orca: a CFS spool that names no recognised product must map to the vendor's plain generic. The
// subtype variants ("High Speed", "Matte", "Silk") score just as well on vendor alone, and since
// each is its own product with its own filament_id, letting one of them win sends the printer the
// id of a filament the user does not have loaded.
TEST_CASE("An unbranded CFS spool maps to the vendor's plain generic, not a subtype", "[CFS][Creality]")
{
    CHECK(match(k2_stock_nozzle, "Creality", "", "PLA") == "Generic PLA @K2-all");
}

// Orca: the vendor bonus reads the preset's owning VendorProfile. "Generic PETG @K2-all" does not
// repeat "Creality" anywhere in its name, so a name based vendor test scored nothing for it and the
// spool fell through to the collection wide first-of-type - an unrelated third party PETG.
TEST_CASE("A CFS spool matches its vendor's presets even when the name omits the vendor", "[CFS][Creality]")
{
    CHECK(match(k2_stock_nozzle, "Creality", "", "PETG") == "Generic PETG @K2-all");
}

TEST_CASE("A branded CFS spool still beats the generic", "[CFS][Creality]")
{
    CHECK(match(k2_stock_nozzle, "Creality", "Hyper PLA", "PLA") == "Hyper PLA @K2-all");
}

// Orca: the specificity penalty must not stop a spool that genuinely asks for a subtype from
// getting it - only unclaimed qualifiers are penalised.
TEST_CASE("A CFS spool that names a subtype gets that subtype", "[CFS][Creality]")
{
    CHECK(match(k2_stock_nozzle, "Creality", "Generic PLA Silk", "PLA") == "Generic PLA Silk @Creality K2-all");
    CHECK(match(k2_stock_nozzle, "Creality", "Generic PLA Matte", "PLA") == "Generic PLA Matte @Creality K2-all");
}

// Orca: a third party spool matches no preset by brand or vendor, so it falls back to the first
// visible preset of the same type rather than returning nothing. Which one that is depends on the
// collection's ordering, so only the type is pinned here - what matters is that a PLA spool never
// comes back empty and never comes back as another material.
TEST_CASE("A CFS spool from an unknown vendor falls back to a preset of the same type", "[CFS][Creality]")
{
    const std::string matched = match(k2_stock_nozzle, "SomeOtherBrand", "", "PLA");
    REQUIRE_FALSE(matched.empty());
    const auto spec = std::find_if(k2_stock_nozzle.begin(), k2_stock_nozzle.end(),
                                   [&](const FilamentSpec &s) { return matched == s.filament_id; });
    REQUIRE(spec != k2_stock_nozzle.end());
    CHECK(std::string(spec->filament_type) == "PLA");
}


// Orca: Creality's bundle also ships third party filaments ("eSUN PLA+ @K2 Plus-all"). Those carry
// Creality's VendorProfile but name their real brand, so the vendor bonus has to accept a name
// match as well as a profile match - otherwise an eSUN spool stops matching its own preset.
TEST_CASE("A third party spool matches its preset inside the printer vendor's bundle", "[CFS][Creality]")
{
    const std::vector<FilamentSpec> with_third_party{
        {"Generic PLA @K2-all",     "PLA-GENERIC", "PLA"},
        {"eSUN PLA+ @K2 Plus-all",  "ESUN-PLA",    "PLA"},   // shipped by Creality, branded eSUN
    };
    CHECK(match(with_third_party, "eSUN", "", "PLA") == "eSUN PLA+ @K2 Plus-all");
    // ... and a Creality spool still prefers Creality's own generic over the eSUN preset, which
    // carries Creality's profile too but adds a brand word the spool never claimed.
    CHECK(match(with_third_party, "Creality", "", "PLA") == "Generic PLA @K2-all");
}
