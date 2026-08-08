#include <catch2/catch_all.hpp>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

DynamicPrintConfig projection_test_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
        {"nozzle_temperature_initial_layer", "205"},
        {"skirt_loops", 0},
    });
    return config;
}

} // namespace

TEST_CASE("Wall process projections apply before explicit object overrides", "[FeatureCadence]")
{
    const DynamicPrintConfig config = projection_test_config();

    const std::string projected = slice_with_object_overrides(
        {cube(20.)}, config, {{{"wall_process_projection", "outer_wall_line_width=0.25"}}});
    CHECK(projected.find("; external perimeters extrusion width = 0.25mm") != std::string::npos);

    const std::string overridden = slice_with_object_overrides(
        {cube(20.)}, config,
        {{{"wall_process_projection", "outer_wall_line_width=0.25"}, {"outer_wall_line_width", 0.3}}});
    CHECK(overridden.find("; external perimeters extrusion width = 0.30mm") != std::string::npos);
}

TEST_CASE("Wall process projection consumption enforces its whitelist", "[FeatureCadence]")
{
    DynamicPrintConfig config = projection_test_config();
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{
        {"wall_process_projection",
         "outer_wall_line_width=0.25;nozzle_temperature_initial_layer=999;layer_height=0.05"},
    }};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    CHECK(print.config().nozzle_temperature_initial_layer.get_at(0) == 205);
    print.process();
    REQUIRE(print.objects().size() == 1);
    CHECK(print.objects().front()->layers().size() == 100);
}

TEST_CASE("Wall process vector projections update only the mapped tool slot", "[FeatureCadence]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        {"nozzle_diameter", "0.4,0.2"},
        {"printer_extruder_id", "1,2"},
        {"printer_extruder_variant", "Direct Drive Standard,Direct Drive Standard"},
        {"outer_wall_speed", "60,70"},
    });
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2};

    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{
        {"outer_wall_filament_id", 2},
        {"wall_process_projection", "outer_wall_speed=42"},
    }};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    const ConfigOptionFloatsNullable &speeds = print.objects().front()->printing_region(0).config().outer_wall_speed;
    REQUIRE(speeds.values.size() >= 2);
    CHECK_THAT(speeds.values[0], Catch::Matchers::WithinAbs(60., 1e-9));
    CHECK_THAT(speeds.values[1], Catch::Matchers::WithinAbs(42., 1e-9));
}
