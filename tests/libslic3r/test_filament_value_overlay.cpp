#include <catch2/catch_all.hpp>

#include "libslic3r/PresetBundle.hpp"

#include <limits>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

class FilamentOverlayFixture
{
public:
    static constexpr const char *printer_04 = "BBL H2D";
    static constexpr const char *printer_02 = "BBL H2D 0.2 nozzle";
    static constexpr const char *basic_04   = "Bambu PLA Basic @BBL H2D";
    static constexpr const char *basic_02   = "Bambu PLA Basic @BBL H2D 0.2 nozzle";
    static constexpr const char *standard_variant = "Direct Drive Standard";
    static constexpr const char *high_flow_variant = "Direct Drive High Flow";

    PresetBundle bundle;

    FilamentOverlayFixture(bool add_basic_sibling = true)
    {
        add_printer(printer_02, 0.2, false);
        add_printer(printer_04, 0.4, true);
        add_filament(basic_04, {printer_04}, 25., 0.8);
        if (add_basic_sibling)
            add_filament(basic_02, {printer_02}, 2., 0.22);

        bundle.filament_presets = {basic_04, basic_04};
        bundle.project_config.option<ConfigOptionFloats>("project_nozzle_diameter", true)->values = {0.4, 0.2};
        bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2};
    }

    void add_printer(const std::string &name, double diameter, bool select)
    {
        DynamicPrintConfig config(bundle.printers.default_preset().config);
        config.option<ConfigOptionString>("printer_model", true)->value = "H2D";
        config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {diameter, diameter};
        config.option<ConfigOptionFloats>("min_layer_height", true)->values = {diameter * 0.2, diameter * 0.2};
        config.option<ConfigOptionFloats>("max_layer_height", true)->values = {diameter * 0.7, diameter * 0.7};
        Preset &preset = bundle.printers.load_preset({}, name, std::move(config), select);
        preset.is_system = true;
    }

    void add_filament(const std::string &name, std::vector<std::string> compatible_printers,
                      double max_speed, double retraction_length)
    {
        DynamicPrintConfig config(bundle.filaments.default_preset().config);
        config.option<ConfigOptionStrings>("compatible_printers", true)->values = std::move(compatible_printers);
        config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {standard_variant};
        config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {max_speed};
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = {retraction_length};
        Preset &preset = bundle.filaments.load_preset({}, name, std::move(config), false);
        preset.is_system = true;
    }

    void set_filament_variants(const std::string &name, const std::vector<std::string> &variants,
                               const std::vector<double> &speeds,
                               const std::vector<double> &retraction_lengths = {})
    {
        Preset *preset = bundle.filaments.find_preset(name, false, true);
        REQUIRE(preset != nullptr);
        preset->config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = variants;
        preset->config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = speeds;
        if (!retraction_lengths.empty())
            preset->config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values =
                retraction_lengths;
    }
};

} // namespace

TEST_CASE("Filament nozzle overlay borrows sibling values without changing preset selection", "[FilamentValueOverlay]")
{
    FilamentOverlayFixture fixture;
    const std::vector<std::string> selected_before = fixture.bundle.filament_presets;
    const Preset *original = fixture.bundle.filaments.find_preset(FilamentOverlayFixture::basic_04, false);
    REQUIRE(original != nullptr);
    const double preset_speed_before = original->config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(0);
    const double preset_retract_before = original->config.option<ConfigOptionFloatsNullable>("filament_retraction_length")->get_at(0);

    const DynamicPrintConfig composed = fixture.bundle.full_config(false);

    CHECK((composed.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values ==
           std::vector<double>{25., 2.}));
    CHECK_THAT(composed.option<ConfigOptionFloatsNullable>("filament_retraction_length")->get_at(1),
               Catch::Matchers::WithinAbs(0.22, 1e-9));
    CHECK(fixture.bundle.filament_presets == selected_before);
    CHECK_THAT(original->config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(0),
               Catch::Matchers::WithinAbs(preset_speed_before, 1e-9));
    CHECK_THAT(original->config.option<ConfigOptionFloatsNullable>("filament_retraction_length")->get_at(0),
               Catch::Matchers::WithinAbs(preset_retract_before, 1e-9));

    const auto &notes = fixture.bundle.filament_value_overlay_notes();
    REQUIRE(notes.size() == 1);
    CHECK(notes.front().slot == 1);
    CHECK(notes.front().source_preset_name == FilamentOverlayFixture::basic_02);
    CHECK_THAT(notes.front().limited_speed, Catch::Matchers::WithinAbs(2., 1e-9));
}

TEST_CASE("A filament already native to its mapped nozzle remains untouched", "[FilamentValueOverlay]")
{
    FilamentOverlayFixture fixture;
    fixture.bundle.filament_presets = {FilamentOverlayFixture::basic_04, FilamentOverlayFixture::basic_02};

    const DynamicPrintConfig composed = fixture.bundle.full_config(false);

    CHECK((composed.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values ==
           std::vector<double>{25., 2.}));
    CHECK(fixture.bundle.filament_value_overlay_notes().empty());
}

TEST_CASE("Missing filament siblings scale flow down but never up", "[FilamentValueOverlay]")
{
    SECTION("smaller nozzle") {
        FilamentOverlayFixture fixture(false);
        const DynamicPrintConfig composed = fixture.bundle.full_config(false);

        CHECK_THAT(composed.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(1),
                   Catch::Matchers::WithinAbs(6.25, 1e-9));
        const auto &notes = fixture.bundle.filament_value_overlay_notes();
        REQUIRE(notes.size() == 1);
        CHECK(notes.front().source_preset_name.empty());
        CHECK_THAT(notes.front().limited_speed, Catch::Matchers::WithinAbs(6.25, 1e-9));
    }

    SECTION("larger nozzle") {
        FilamentOverlayFixture fixture(false);
        fixture.bundle.project_config.option<ConfigOptionFloats>("project_nozzle_diameter", true)->values = {0.4, 0.6};
        const DynamicPrintConfig composed = fixture.bundle.full_config(false);

        CHECK_THAT(composed.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(1),
                   Catch::Matchers::WithinAbs(25., 1e-9));
        CHECK(fixture.bundle.filament_value_overlay_notes().empty());
    }
}

TEST_CASE("Uniform nozzle filament overlay is byte identical", "[FilamentValueOverlay]")
{
    FilamentOverlayFixture fixture;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("project_nozzle_diameter", true)->values = {0.4, 0.4};
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.4};
    config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2};
    config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {25., 25.};
    const DynamicPrintConfig before(config);

    const auto notes = apply_filament_nozzle_value_overlay(config, fixture.bundle);

    CHECK(notes.empty());
    CHECK(config.keys() == before.keys());
    CHECK(config.diff(before).empty());
    CHECK(before.diff(config).empty());
}

TEST_CASE("Filament sibling name grammar handles implicit and explicit 0.4 suffixes", "[FilamentValueOverlay]")
{
    SECTION("implicit 0.4 suffix") {
        FilamentOverlayFixture fixture;
        const Preset *sibling = fixture.bundle.sibling_filament_preset_for_nozzle(0, 0.2);
        REQUIRE(sibling != nullptr);
        CHECK(sibling->name == FilamentOverlayFixture::basic_02);
    }

    SECTION("explicit 0.4 suffix") {
        FilamentOverlayFixture fixture;
        constexpr const char *asa_04 = "Bambu ASA @BBL H2D 0.4 nozzle";
        constexpr const char *asa_02 = "Bambu ASA @BBL H2D 0.2 nozzle";
        fixture.add_filament(asa_04, {FilamentOverlayFixture::printer_04}, 18., 0.8);
        fixture.add_filament(asa_02, {FilamentOverlayFixture::printer_02}, 2., 0.2);
        fixture.bundle.filament_presets = {asa_04};

        const Preset *sibling = fixture.bundle.sibling_filament_preset_for_nozzle(0, 0.2);
        REQUIRE(sibling != nullptr);
        CHECK(sibling->name == asa_02);
    }
}

TEST_CASE("Missing compatible printer names are skipped during filament sibling lookup", "[FilamentValueOverlay]")
{
    FilamentOverlayFixture fixture(false);
    constexpr const char *current = "Bambu PC @BBL H2D 0.4 nozzle";
    constexpr const char *sibling_name = "Bambu PC @BBL H2D 0.2 nozzle";
    fixture.add_filament(current, {"Removed H2D preset", FilamentOverlayFixture::printer_04}, 20., 0.8);
    fixture.add_filament(sibling_name, {"Removed H2D 0.2 preset", FilamentOverlayFixture::printer_02}, 2., 0.2);
    fixture.bundle.filament_presets = {current};

    const Preset *sibling = fixture.bundle.sibling_filament_preset_for_nozzle(0, 0.2);

    REQUIRE(sibling != nullptr);
    CHECK(sibling->name == sibling_name);
}

TEST_CASE("Filament nozzle overlay follows expanded variant columns", "[FilamentValueOverlay]")
{
    constexpr const char *secondary_04 = "Bambu PETG Basic @BBL H2D";

    SECTION("sibling values are matched by variant name") {
        FilamentOverlayFixture fixture;
        fixture.set_filament_variants(FilamentOverlayFixture::basic_04,
            {FilamentOverlayFixture::standard_variant, FilamentOverlayFixture::high_flow_variant},
            {25., 40.}, {0.8, 0.9});
        fixture.set_filament_variants(FilamentOverlayFixture::basic_02,
            {FilamentOverlayFixture::high_flow_variant, FilamentOverlayFixture::standard_variant},
            {2., 2.}, {0.3, 0.2});
        fixture.add_filament(secondary_04, {FilamentOverlayFixture::printer_04}, 18., 0.7);
        fixture.bundle.filament_presets = {FilamentOverlayFixture::basic_04, secondary_04};
        fixture.bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {2, 1};

        const DynamicPrintConfig composed = fixture.bundle.full_config(false);

        CHECK(composed.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{1, 1, 2});
        CHECK(composed.option<ConfigOptionStrings>("filament_extruder_variant")->values ==
              std::vector<std::string>{FilamentOverlayFixture::standard_variant,
                                       FilamentOverlayFixture::high_flow_variant,
                                       FilamentOverlayFixture::standard_variant});
        CHECK(composed.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values ==
              std::vector<double>{2., 2., 18.});
        CHECK(composed.option<ConfigOptionFloatsNullable>("filament_retraction_length")->values ==
              std::vector<double>{0.2, 0.3, 0.7});
        const auto &notes = fixture.bundle.filament_value_overlay_notes();
        REQUIRE(notes.size() == 1);
        CHECK_THAT(notes.front().limited_speed, Catch::Matchers::WithinAbs(2., 1e-9));
    }

    SECTION("heuristic scales every owned variant column") {
        FilamentOverlayFixture fixture(false);
        fixture.set_filament_variants(FilamentOverlayFixture::basic_04,
            {FilamentOverlayFixture::standard_variant, FilamentOverlayFixture::high_flow_variant},
            {25., 40.});
        fixture.add_filament(secondary_04, {FilamentOverlayFixture::printer_04}, 18., 0.7);
        fixture.bundle.filament_presets = {FilamentOverlayFixture::basic_04, secondary_04};
        fixture.bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values = {2, 1};

        const DynamicPrintConfig composed = fixture.bundle.full_config(false);

        CHECK(composed.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{1, 1, 2});
        CHECK(composed.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values ==
              std::vector<double>{6.25, 10., 18.});
        const auto &notes = fixture.bundle.filament_value_overlay_notes();
        REQUIRE(notes.size() == 1);
        CHECK_THAT(notes.front().limited_speed, Catch::Matchers::WithinAbs(6.25, 1e-9));
    }
}
