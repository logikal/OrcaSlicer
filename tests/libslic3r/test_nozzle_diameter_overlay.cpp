#include <catch2/catch_all.hpp>

#include "libslic3r/FeatureProcessResolver.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

DynamicPrintConfig overlay_config(std::vector<double> project_diameters)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.4};
    config.option<ConfigOptionFloats>("min_layer_height", true)->values = {0.08, 0.08};
    config.option<ConfigOptionFloats>("max_layer_height", true)->values = {0.3, 0.3};
    config.option<ConfigOptionFloats>("project_nozzle_diameter", true)->values = std::move(project_diameters);
    return config;
}

class OverlayResolverFixture
{
public:
    static constexpr const char *printer_04  = "OverlayPrinter 0.4 nozzle";
    static constexpr const char *printer_02  = "OverlayPrinter 0.2 nozzle";
    static constexpr const char *standard_04 = "0.20mm Standard @OverlayPrinter 0.4 nozzle";
    static constexpr const char *standard_02 = "0.10mm Standard @OverlayPrinter 0.2 nozzle";

    PresetBundle       bundle;
    DynamicPrintConfig printer_config = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig object_config  = DynamicPrintConfig::full_print_config();

    OverlayResolverFixture()
    {
        add_printer(printer_02, 0.2, false);
        add_printer(printer_04, 0.4, true);
        add_process(standard_04, 0.2, printer_04);
        add_process(standard_02, 0.1, printer_02);

        printer_config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.4};
        printer_config.option<ConfigOptionFloats>("project_nozzle_diameter", true)->values = {0.4, 0.2};
        printer_config.option<ConfigOptionFloats>("min_layer_height", true)->values = {0.07, 0.07};
        printer_config.option<ConfigOptionFloats>("max_layer_height", true)->values = {0.3, 0.3};
        printer_config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2};
        printer_config.option<ConfigOptionString>("printer_model", true)->value = "OverlayPrinter";
        apply_project_nozzle_diameters(printer_config);

        object_config.option<ConfigOptionFloat>("layer_height", true)->value = 0.2;
        object_config.option<ConfigOptionInt>("extruder", true)->value = 1;
    }

    FeatureProcessRequest request() const
    {
        FeatureProcessRequest result;
        result.role                    = FeatureRole::Wall;
        result.policy                  = FeatureProcessPolicy::AutoNozzleVariant;
        result.feature_filament        = 2;
        result.effective_object_config = &object_config;
        result.object_process_preset   = standard_04;
        result.bundle                  = &bundle;
        result.printer_config          = &printer_config;
        return result;
    }

private:
    void add_printer(const std::string &name, double nozzle, bool select)
    {
        DynamicPrintConfig config(bundle.printers.default_preset().config);
        config.option<ConfigOptionString>("printer_model", true)->value = "OverlayPrinter";
        config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {nozzle};
        config.option<ConfigOptionFloats>("min_layer_height", true)->values = {0.07};
        config.option<ConfigOptionFloats>("max_layer_height", true)->values = {nozzle == 0.2 ? 0.15 : 0.3};
        Preset &preset = bundle.printers.load_preset({}, name, std::move(config), select);
        preset.is_system = true;
    }

    void add_process(const std::string &name, double height, const std::string &compatible_printer)
    {
        DynamicPrintConfig config(bundle.prints.default_preset().config);
        config.option<ConfigOptionFloat>("layer_height", true)->value = height;
        config.option<ConfigOptionStrings>("compatible_printers", true)->values = {compatible_printer};
        Preset &preset = bundle.prints.load_preset({}, name, std::move(config), false);
        preset.is_system = true;
    }
};

} // namespace

TEST_CASE("An empty project nozzle overlay is an exact no-op", "[NozzleOverlay]")
{
    DynamicPrintConfig config = overlay_config({});
    const DynamicPrintConfig before(config);

    apply_project_nozzle_diameters(config);

    CHECK(config.keys() == before.keys());
    CHECK(config.diff(before).empty());
    CHECK(before.diff(config).empty());
}

TEST_CASE("Project nozzle overlays replace only supplied diameters and rederive changed limits", "[NozzleOverlay]")
{
    SECTION("partial overlay") {
        DynamicPrintConfig config = overlay_config({0.2});
        apply_project_nozzle_diameters(config);

        CHECK((config.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.2, 0.4}));
        CHECK((config.option<ConfigOptionFloats>("min_layer_height")->values == std::vector<double>{0., 0.08}));
        CHECK((config.option<ConfigOptionFloats>("max_layer_height")->values == std::vector<double>{0., 0.3}));
    }

    SECTION("full overlay resets only the changed extruder") {
        DynamicPrintConfig config = overlay_config({0.4, 0.2});
        CHECK((effective_nozzle_diameters(config) == std::vector<double>{0.4, 0.2}));

        apply_project_nozzle_diameters(config);

        CHECK((config.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.4, 0.2}));
        CHECK((config.option<ConfigOptionFloats>("min_layer_height")->values == std::vector<double>{0.08, 0.}));
        CHECK((config.option<ConfigOptionFloats>("max_layer_height")->values == std::vector<double>{0.3, 0.}));
        CHECK((effective_nozzle_diameters(config) == std::vector<double>{0.4, 0.2}));

        const DynamicPrintConfig once(config);
        apply_project_nozzle_diameters(config);
        CHECK(config.diff(once).empty());
        CHECK(once.diff(config).empty());
    }
}

TEST_CASE("Preset composition applies the project nozzle overlay after the printer preset", "[NozzleOverlay]")
{
    PresetBundle bundle;
    Preset printer = bundle.printers.default_preset();
    Preset process = bundle.prints.default_preset();
    std::vector<Preset> filaments{bundle.filaments.default_preset()};
    printer.config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.4};
    printer.config.option<ConfigOptionFloats>("min_layer_height", true)->values = {0.08, 0.08};
    printer.config.option<ConfigOptionFloats>("max_layer_height", true)->values = {0.3, 0.3};

    DynamicPrintConfig project_config;
    project_config.option<ConfigOptionFloats>("project_nozzle_diameter", true)->values = {0.4, 0.2};
    const DynamicPrintConfig composed = PresetBundle::construct_full_config(
        printer, process, project_config, filaments, false, std::nullopt);

    CHECK((composed.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.4, 0.2}));
    CHECK((composed.option<ConfigOptionFloats>("min_layer_height")->values == std::vector<double>{0.08, 0.}));
    CHECK((composed.option<ConfigOptionFloats>("max_layer_height")->values == std::vector<double>{0.3, 0.}));
}

TEST_CASE("Device extruder ids translate through the logical to physical map", "[NozzleOverlay]")
{
    DynamicPrintConfig config;
    config.option<ConfigOptionInts>("physical_extruder_map", true)->values = {1, 0};
    CHECK(logical_index_for_device_extruder(config, 0) == 1);
    CHECK(logical_index_for_device_extruder(config, 1) == 0);

    config.erase("physical_extruder_map");
    CHECK(logical_index_for_device_extruder(config, 0) == 0);
    CHECK(logical_index_for_device_extruder(config, 1) == 1);

    config.option<ConfigOptionInts>("physical_extruder_map", true)->values = {1};
    CHECK(logical_index_for_device_extruder(config, 0) == 0);
}

TEST_CASE("Project nozzle diameters round trip as project config", "[NozzleOverlay]")
{
    PresetBundle bundle;
    const auto *default_overlay = bundle.project_config.option<ConfigOptionFloats>("project_nozzle_diameter");
    REQUIRE(default_overlay != nullptr);
    CHECK(default_overlay->values.empty());

    DynamicPrintConfig source;
    source.set_deserialize_strict("project_nozzle_diameter", "0.4,0.2");
    DynamicPrintConfig restored;
    restored.set_deserialize_strict(
        "project_nozzle_diameter", source.option_throw("project_nozzle_diameter")->serialize());

    CHECK(restored.option_throw("project_nozzle_diameter")->serialize() == "0.4,0.2");
}

TEST_CASE("Feature process resolution uses the overlaid tool nozzle", "[NozzleOverlay]")
{
    OverlayResolverFixture fixture;
    const FeatureProcessResolution result = resolve_feature_process(fixture.request());

    REQUIRE(result.ok);
    CHECK(result.resolved_preset == OverlayResolverFixture::standard_02);
    CHECK(result.tool_id == 1);
    CHECK_THAT(result.nozzle_diameter, Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK_THAT(result.feature_layer_height, Catch::Matchers::WithinAbs(0.1, 1e-9));
}
