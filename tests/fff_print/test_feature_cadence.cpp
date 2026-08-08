#include <catch2/catch_all.hpp>

#include "libslic3r/Layer.hpp"
#include "test_helpers.hpp"

#include <cmath>

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

DynamicPrintConfig mixed_nozzle_grid_config()
{
    DynamicPrintConfig config = multifilament_config(2, {
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
        {"nozzle_diameter", "0.2,0.4"},
        {"min_layer_height", "0.05,0.1"},
        {"max_layer_height", "0.15,0.3"},
        {"printer_extruder_id", "1,2"},
        {"printer_extruder_variant", "Direct Drive Standard,Direct Drive Standard"},
        {"outer_wall_filament_id", 1},
        {"inner_wall_filament_id", 1},
        {"sparse_infill_filament_id", 2},
        {"internal_solid_filament_id", 2},
        {"top_surface_filament_id", 2},
        {"bottom_surface_filament_id", 2},
        {"skirt_loops", 0},
    });
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2};
    return config;
}

void process_cube_with_overrides(const DynamicPrintConfig &config,
                                 const std::vector<ConfigBase::SetDeserializeItem> &object_overrides,
                                 Print &print, Model &model)
{
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{object_overrides};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);
    print.process();
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

TEST_CASE("A finer wall layer height refines the object grid", "[FeatureCadence]")
{
    const DynamicPrintConfig config = mixed_nozzle_grid_config();
    Print print;
    Model model;
    process_cube_with_overrides(config, {{"wall_layer_height", 0.1}}, print, model);

    REQUIRE(print.objects().size() == 1);
    const SlicingParameters preview = PrintObject::slicing_parameters(
        config, *model.objects.front(), 20., Vec3d::Ones());
    CHECK_THAT(preview.layer_height, Catch::Matchers::WithinAbs(0.1, EPSILON));
    CHECK_THAT(preview.base_layer_height, Catch::Matchers::WithinAbs(0.2, EPSILON));
    CHECK(preview.cadence_ratio == 2);

    const ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
    const size_t expected_layers = static_cast<size_t>(std::lround((20. - 0.2) / 0.1)) + 1;
    REQUIRE(layers.size() == expected_layers);
    REQUIRE_THAT(layers.front()->height, Catch::Matchers::WithinAbs(0.2, EPSILON));
    for (size_t i = 1; i < layers.size(); ++i) {
        CHECK_THAT(layers[i]->height, Catch::Matchers::WithinAbs(0.1, EPSILON));
        CHECK_THAT(layers[i]->print_z - layers[i - 1]->print_z, Catch::Matchers::WithinAbs(0.1, EPSILON));
    }
    CHECK_THAT(layers.back()->print_z, Catch::Matchers::WithinAbs(20., EPSILON));
}

TEST_CASE("Default wall cadence leaves the object grid untouched", "[FeatureCadence][Regression]")
{
    const DynamicPrintConfig config = mixed_nozzle_grid_config();
    Print default_print;
    Model default_model;
    process_cube_with_overrides(config, {{"wall_layer_height", 0.}}, default_print, default_model);

    Print control_print;
    Model control_model;
    process_cube_with_overrides(config, {}, control_print, control_model);

    const ConstLayerPtrsAdaptor default_layers = default_print.objects().front()->layers();
    const ConstLayerPtrsAdaptor control_layers = control_print.objects().front()->layers();
    CHECK(equal_layering(default_print.objects().front()->slicing_parameters(),
                         control_print.objects().front()->slicing_parameters()));
    REQUIRE(default_layers.size() == control_layers.size());
    for (size_t i = 0; i < control_layers.size(); ++i) {
        CHECK_THAT(default_layers[i]->height, Catch::Matchers::WithinAbs(control_layers[i]->height, EPSILON));
        CHECK_THAT(default_layers[i]->print_z, Catch::Matchers::WithinAbs(control_layers[i]->print_z, EPSILON));
        CHECK_THAT(default_layers[i]->slice_z, Catch::Matchers::WithinAbs(control_layers[i]->slice_z, EPSILON));
    }
}

TEST_CASE("Fine grid bounds ignore coarse-cadence tools", "[FeatureCadence]")
{
    const DynamicPrintConfig config = mixed_nozzle_grid_config();
    Print print;
    Model model;
    process_cube_with_overrides(config, {{"wall_layer_height", 0.1}}, print, model);

    const PrintObject &object = *print.objects().front();
    CHECK_THAT(object.slicing_parameters().min_layer_height, Catch::Matchers::WithinAbs(0.05, EPSILON));
    CHECK_THAT(object.slicing_parameters().max_layer_height, Catch::Matchers::WithinAbs(0.15, EPSILON));
    REQUIRE(object.layers().size() > 1);
    for (size_t i = 1; i < object.layers().size(); ++i)
        CHECK_THAT(object.layers()[i]->height, Catch::Matchers::WithinAbs(0.1, EPSILON));
}

TEST_CASE("Slicing parameters preserve base and grid cadence", "[FeatureCadence]")
{
    const DynamicPrintConfig config = mixed_nozzle_grid_config();
    PrintConfig print_config;
    PrintObjectConfig object_config;
    print_config.apply(config, true);
    object_config.apply(config, true);

    const FeatureCadencePlan plan{0.2, 0.1, 2};
    const SlicingParameters fine = SlicingParameters::create_from_config(
        print_config, object_config, 20., {0, 1}, Vec3d::Ones(), &plan, {0});
    CHECK_THAT(fine.layer_height, Catch::Matchers::WithinAbs(0.1, EPSILON));
    CHECK_THAT(fine.base_layer_height, Catch::Matchers::WithinAbs(0.2, EPSILON));
    CHECK(fine.cadence_ratio == 2);

    const SlicingParameters unchanged = SlicingParameters::create_from_config(
        print_config, object_config, 20., {0, 1}, Vec3d::Ones());
    CHECK_THAT(unchanged.base_layer_height, Catch::Matchers::WithinAbs(unchanged.layer_height, EPSILON));
    CHECK(unchanged.cadence_ratio == 1);
}
