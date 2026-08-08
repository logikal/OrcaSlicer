#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "test_helpers.hpp"

#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>

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

void visit_paths(const ExtrusionEntity &entity, const std::function<void(const ExtrusionPath &)> &visitor)
{
    if (entity.is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection &>(entity).entities)
            visit_paths(*child, visitor);
    } else if (entity.is_loop()) {
        for (const ExtrusionPath &path : static_cast<const ExtrusionLoop &>(entity).paths)
            visitor(path);
    } else if (const auto *multi_path = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &path : multi_path->paths)
            visitor(path);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        visitor(*path);
    }
}

std::vector<const ExtrusionPath *> paths_with_role(const Layer &layer, ExtrusionRole role)
{
    std::vector<const ExtrusionPath *> paths;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.entities)
            visit_paths(*entity, [&](const ExtrusionPath &path) {
                if (path.role() == role)
                    paths.push_back(&path);
            });
    return paths;
}

std::set<ExtrusionRole> fill_roles(const Layer &layer)
{
    std::set<ExtrusionRole> roles;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.entities)
            visit_paths(*entity, [&](const ExtrusionPath &path) { roles.insert(path.role()); });
    return roles;
}

double total_fill_length(const PrintObject &object)
{
    double length = 0.;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->fills.entities)
                visit_paths(*entity, [&](const ExtrusionPath &path) { length += path.length(); });
    return length;
}

bool is_base_z(double z)
{
    return std::abs(z / 0.2 - std::round(z / 0.2)) <= 1e-4;
}

void process_stacked_regions(const DynamicPrintConfig &config, Print &print, Model &model)
{
    ModelObject *object = model.add_object();
    object->name = "stacked-regions.stl";
    object->add_volume(make_cube(20., 20., 10.));

    TriangleMesh upper = make_cube(20., 20., 10.);
    Transform3d transform = Transform3d::Identity();
    transform.translation().z() = 10.;
    upper.transform(transform, false);
    ModelVolume *upper_volume = object->add_volume(std::move(upper));
    upper_volume->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(20.));

    object->config.set("wall_layer_height", 0.1);
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
}

void process_bottom_surface_with_separate_solid_interior(const DynamicPrintConfig &config, Print &print, Model &model)
{
    ModelObject *object = model.add_object();
    object->name = "bottom-and-interior.stl";
    object->add_volume(make_cube(8., 8., 10.));

    TriangleMesh upper = make_cube(8., 8., 10.);
    Transform3d upper_transform = Transform3d::Identity();
    upper_transform.translation().z() = 10.;
    upper.transform(upper_transform, false);
    ModelVolume *upper_volume = object->add_volume(std::move(upper));
    upper_volume->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(100.));

    TriangleMesh pillar = make_cube(4., 4., 20.);
    Transform3d pillar_transform = Transform3d::Identity();
    pillar_transform.translation().x() = 12.;
    pillar.transform(pillar_transform, false);
    ModelVolume *pillar_volume = object->add_volume(std::move(pillar));
    pillar_volume->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(100.));

    object->config.set("wall_layer_height", 0.1);
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
}

bool is_shell_fill_role(ExtrusionRole role)
{
    return role == erSolidInfill || role == erTopSolidInfill || role == erBottomSurface;
}

std::set<int> shell_layer_z_tenths(const PrintObject &object)
{
    std::set<int> result;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->fills.entities)
                visit_paths(*entity, [&](const ExtrusionPath &path) {
                    if (is_shell_fill_role(path.role()))
                        result.insert(int(std::lround(layer->print_z * 10.)));
                });
    return result;
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
        CHECK(fill_roles(*default_layers[i]) == fill_roles(*control_layers[i]));
    }
    CHECK_THAT(total_fill_length(*default_print.objects().front()),
               Catch::Matchers::WithinAbs(total_fill_length(*control_print.objects().front()), EPSILON));
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

TEST_CASE("Interior sparse infill recombines to base cadence", "[FeatureCadence]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({{"sparse_infill_density", 15.}});
    Print print;
    Model model;
    process_cube_with_overrides(config, {{"wall_layer_height", 0.1}}, print, model);

    size_t combined_path_count = 0;
    for (const Layer *layer : print.objects().front()->layers()) {
        const auto paths = paths_with_role(*layer, erInternalInfill);
        if (paths.empty())
            continue;
        for (const ExtrusionPath *path : paths) {
            CAPTURE(layer->print_z, path->height);
            if (std::abs(path->height - 0.2) <= EPSILON) {
                ++combined_path_count;
                CHECK(is_base_z(layer->print_z));
            } else {
                // Shell boundaries may leave clearance slivers at the native fine cadence.
                CHECK_THAT(path->height, Catch::Matchers::WithinAbs(0.1, EPSILON));
            }
        }
    }
    CHECK(combined_path_count > 0);
}

TEST_CASE("Internal solid infill recombines to base cadence", "[FeatureCadence]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({
        {"sparse_infill_density", 100.},
        {"top_shell_layers", 0},
        {"bottom_shell_layers", 0},
    });
    Print print;
    Model model;
    process_cube_with_overrides(config, {{"wall_layer_height", 0.1}}, print, model);

    size_t path_count = 0;
    for (const Layer *layer : print.objects().front()->layers()) {
        const auto paths = paths_with_role(*layer, erSolidInfill);
        for (const ExtrusionPath *path : paths) {
            ++path_count;
            CHECK(is_base_z(layer->print_z));
            CHECK_THAT(path->height, Catch::Matchers::WithinAbs(0.2, EPSILON));
        }
    }
    CHECK(path_count > 0);
}

TEST_CASE("Top surfaces keep their Z and gain thickness", "[FeatureCadence]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({{"sparse_infill_density", 15.}, {"top_shell_layers", 2}});
    Print print;
    Model model;
    process_cube_with_overrides(config, {{"wall_layer_height", 0.1}}, print, model);

    const ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
    const Layer *top_layer = layers.back();
    const auto paths = paths_with_role(*top_layer, erTopSolidInfill);
    REQUIRE_FALSE(paths.empty());
    CHECK_THAT(top_layer->print_z, Catch::Matchers::WithinAbs(20., EPSILON));
    for (const ExtrusionPath *path : paths)
        CHECK_THAT(path->height, Catch::Matchers::WithinAbs(0.2, EPSILON));
    REQUIRE(layers.size() >= 2);
    CHECK(layers[layers.size() - 2]->regions().front()->fill_surfaces.has(stInternalVoid));
}

TEST_CASE("Bottom surfaces recombine upward preserving the bottom Z", "[FeatureCadence]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({
        {"sparse_infill_density", 15.},
        {"interface_shells", true},
        {"bottom_shell_layers", 2},
        {"bottom_shell_thickness", 0.},
    });
    Print print;
    Model model;
    process_stacked_regions(config, print, model);

    bool found_combined_bottom = false;
    size_t plain_bottom_surfaces = 0;
    size_t combined_bottom_surfaces = 0;
    const ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
    for (size_t layer_idx = 1; layer_idx < layers.size(); ++layer_idx) {
        for (const Surface &surface : layers[layer_idx]->regions().front()->fill_surfaces.surfaces)
            if (surface.surface_type == stBottom) {
                ++plain_bottom_surfaces;
                if (std::abs(surface.thickness - 0.2) <= EPSILON)
                    ++combined_bottom_surfaces;
            }
        for (const ExtrusionPath *path : paths_with_role(*layers[layer_idx], erBottomSurface)) {
            if (std::abs(path->height - 0.2) > EPSILON)
                continue;
            found_combined_bottom = true;
            CHECK(is_base_z(layers[layer_idx]->print_z));
            CHECK_THAT(layers[layer_idx]->print_z - path->height,
                       Catch::Matchers::WithinAbs(layers[layer_idx - 1]->bottom_z(), EPSILON));
        }
    }
    CAPTURE(plain_bottom_surfaces, combined_bottom_surfaces);
    CHECK(found_combined_bottom);
}

TEST_CASE("Fine-only G-code layers contain walls but no recombinable interior extrusion", "[FeatureCadence]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({
        {"sparse_infill_density", 15.},
        {"top_shell_layers", 2},
        {"bottom_shell_layers", 1},
        {"top_shell_thickness", 0.},
        {"bottom_shell_thickness", 0.},
        {"ensure_vertical_shell_thickness", "none"},
        {"enable_prime_tower", false},
    });
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"wall_layer_height", 0.1}}};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);
    const std::string output = gcode(print);

    struct LayerExtrusions {
        bool wall = false;
        bool interior = false;
        std::set<int> tools;
        std::set<std::string> interior_comments;
    };
    std::map<int, LayerExtrusions> by_tenth;
    int current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string command(line.cmd());
        if (command.size() >= 2 && command.front() == 'T' && std::isdigit(static_cast<unsigned char>(command[1]))) {
            current_tool = std::stoi(command.substr(1));
            return;
        }
        if (!line.extruding(self))
            return;
        const std::string comment(line.comment());
        const int z_tenth = int(std::lround(line.new_Z(self) * 10.));
        // Bridge roles are deliberately excluded from recombination and may remain on a fine layer.
        const bool recombinable_interior =
            (comment.find("infill") != std::string::npos || comment.find("surface") != std::string::npos) &&
            comment.find("bridge") == std::string::npos;
        if (comment.find("perimeter") != std::string::npos) {
            by_tenth[z_tenth].wall = true;
            by_tenth[z_tenth].tools.insert(current_tool);
        } else if (recombinable_interior) {
            by_tenth[z_tenth].interior = true;
            by_tenth[z_tenth].tools.insert(current_tool);
            by_tenth[z_tenth].interior_comments.insert(comment);
        }
    });

    size_t fine_only_layers = 0;
    for (const auto &[z_tenth, extrusions] : by_tenth) {
        if (z_tenth <= 2 || z_tenth % 2 == 0)
            continue;
        CAPTURE(z_tenth, extrusions.interior_comments);
        ++fine_only_layers;
        CHECK(extrusions.wall);
        CHECK_FALSE(extrusions.interior);
        CHECK(extrusions.tools == std::set<int>{0});
    }
    CHECK(fine_only_layers > 0);
}

TEST_CASE("A coarse-tool cap leaves unsafe interiors at fine cadence", "[FeatureCadence]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({{"sparse_infill_density", 15.}, {"max_layer_height", "0.15,0.15"}});
    Print print;
    Model model;
    process_cube_with_overrides(config, {{"wall_layer_height", 0.1}}, print, model);

    bool found_fine_interior = false;
    for (const Layer *layer : print.objects().front()->layers())
        for (const ExtrusionPath *path : paths_with_role(*layer, erInternalInfill))
            if (std::abs(path->height - 0.1) <= EPSILON)
                found_fine_interior = true;
    CHECK(found_fine_interior);
}

TEST_CASE("Shell thickness in millimeters survives fine cadence", "[FeatureCadence]")
{
    constexpr int top_shell_layers = 3;
    constexpr int bottom_shell_layers = 2;
    constexpr double base_height = 0.2;
    constexpr double cube_height = 20.;

    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({
        {"sparse_infill_density", 15.},
        {"top_shell_layers", top_shell_layers},
        {"bottom_shell_layers", bottom_shell_layers},
        {"top_shell_thickness", 0.},
        {"bottom_shell_thickness", 0.},
        {"ensure_vertical_shell_thickness", "none"},
    });
    Print print;
    Model model;
    process_cube_with_overrides(config, {{"wall_layer_height", 0.1}}, print, model);

    std::map<int, double> shell_area_by_tenth;
    for (const Layer *layer : print.objects().front()->layers()) {
        for (const LayerRegion *region : layer->regions())
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.surface_type == stTop || surface.surface_type == stBottom ||
                    surface.surface_type == stInternalSolid)
                    shell_area_by_tenth[int(std::lround(layer->print_z * 10.))] += surface.expolygon.area();
    }

    REQUIRE_FALSE(shell_area_by_tenth.empty());
    const double full_shell_area = std::max_element(shell_area_by_tenth.begin(), shell_area_by_tenth.end(),
        [](const auto &lhs, const auto &rhs) { return lhs.second < rhs.second; })->second;
    double top_shell_bottom = cube_height;
    double bottom_shell_top = 0.;
    size_t top_solid_layers = 0;
    size_t bottom_solid_layers = 0;
    for (const auto &[z_tenth, area] : shell_area_by_tenth) {
        const double print_z = z_tenth / 10.;
        // Horizontal-shell clipping may leave small fine-cadence edge slivers. Count full-area,
        // base-aligned solid-bearing layers, which define the configured shell thickness.
        if (!is_base_z(print_z) || area < 0.5 * full_shell_area)
            continue;
        if (print_z > cube_height / 2.) {
            ++top_solid_layers;
            top_shell_bottom = std::min(top_shell_bottom, print_z - base_height);
        } else {
            ++bottom_solid_layers;
            bottom_shell_top = std::max(bottom_shell_top, print_z);
        }
    }

    const double expected_top_span = top_shell_layers * base_height;
    const double expected_bottom_span = bottom_shell_layers * base_height;
    CHECK_THAT(cube_height - top_shell_bottom, Catch::Matchers::WithinAbs(expected_top_span, EPSILON));
    CHECK_THAT(bottom_shell_top, Catch::Matchers::WithinAbs(expected_bottom_span, EPSILON));
    CHECK(top_solid_layers == size_t(top_shell_layers));
    CHECK(bottom_solid_layers == size_t(bottom_shell_layers));
}

TEST_CASE("Ratio one shell layer counts match the control slice", "[FeatureCadence][Regression]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({
        {"nozzle_diameter", "0.4,0.4"},
        {"min_layer_height", "0.1,0.1"},
        {"max_layer_height", "0.3,0.3"},
        {"top_shell_layers", 3},
        {"bottom_shell_layers", 2},
        {"top_shell_thickness", 0.},
        {"bottom_shell_thickness", 0.},
        {"ensure_vertical_shell_thickness", "none"},
    });

    Print cadence_print;
    Model cadence_model;
    process_cube_with_overrides(config, {{"wall_layer_height", 0.2}}, cadence_print, cadence_model);
    Print control_print;
    Model control_model;
    process_cube_with_overrides(config, {}, control_print, control_model);

    REQUIRE(cadence_print.objects().front()->slicing_parameters().cadence_ratio == 1);
    REQUIRE(control_print.objects().front()->slicing_parameters().cadence_ratio == 1);
    CHECK(shell_layer_z_tenths(*cadence_print.objects().front()) ==
          shell_layer_z_tenths(*control_print.objects().front()));
}

TEST_CASE("Bottom recombination preserves separate combined solid metadata", "[FeatureCadence][Regression]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({
        {"sparse_infill_density", 15.},
        {"interface_shells", true},
        {"top_shell_layers", 0},
        {"bottom_shell_layers", 1},
        {"top_shell_thickness", 0.},
        {"bottom_shell_thickness", 0.},
        {"ensure_vertical_shell_thickness", "none"},
    });
    Print print;
    Model model;
    process_bottom_surface_with_separate_solid_interior(config, print, model);

    const ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
    const auto lower_it = std::find_if(layers.begin(), layers.end(), [](const Layer *layer) {
        return std::abs(layer->print_z - 10.1) <= EPSILON;
    });
    REQUIRE(lower_it != layers.end());
    REQUIRE(std::next(lower_it) != layers.end());
    const Layer &lower = **lower_it;
    const Layer &upper = **std::next(lower_it);
    REQUIRE_THAT(upper.print_z, Catch::Matchers::WithinAbs(10.2, EPSILON));

    Polygons voids;
    for (const LayerRegion *region : lower.regions())
        polygons_append(voids, to_polygons(region->fill_surfaces.filter_by_type(stInternalVoid)));
    REQUIRE_FALSE(voids.empty());

    const auto bottom_paths = paths_with_role(upper, erBottomSurface);
    REQUIRE_FALSE(bottom_paths.empty());
    for (const ExtrusionPath *path : bottom_paths)
        CHECK_THAT(path->height, Catch::Matchers::WithinAbs(0.2, EPSILON));

    size_t combined_solid_paths = 0;
    size_t fine_solid_segments_over_void = 0;
    for (const ExtrusionPath *path : paths_with_role(upper, erSolidInfill)) {
        if (std::abs(path->height - 0.2) <= EPSILON)
            ++combined_solid_paths;
        if (std::abs(path->height - 0.1) <= EPSILON)
            fine_solid_segments_over_void += intersection_pl({path->polyline.to_polyline()}, voids).size();
    }
    CHECK(combined_solid_paths > 0);
    CHECK(fine_solid_segments_over_void == 0);
}

TEST_CASE("A non-divisor wall cadence reports nearby valid heights", "[FeatureCadence][Validate]")
{
    const DynamicPrintConfig config = mixed_nozzle_grid_config();
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"wall_layer_height", 0.13}}};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    const StringObjectException error = print.validate();
    REQUIRE_FALSE(error.string.empty());
    CHECK(error.string.find("0.13") != std::string::npos);
    CHECK((error.string.find("0.2") != std::string::npos || error.string.find("0.1") != std::string::npos));
    CHECK(error.opt_key == "wall_layer_height");
}

TEST_CASE("Wall cadence rejects an extrusion taller than its wall nozzle", "[FeatureCadence][Validate]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({
        {"layer_height", 0.5},
        {"max_layer_height", "0.3,0.5"},
    });
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"wall_layer_height", 0.25}}};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    const StringObjectException error = print.validate();
    REQUIRE_FALSE(error.string.empty());
    CHECK(error.string.find("Outer wall") != std::string::npos);
    CHECK(error.string.find("0.25") != std::string::npos);
    CHECK(error.string.find("0.2") != std::string::npos);
    CHECK(error.opt_key == "wall_layer_height");
}

TEST_CASE("Fine inner and outer walls must share a nozzle diameter", "[FeatureCadence][Validate]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({{"inner_wall_filament_id", 2}});
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"wall_layer_height", 0.1}}};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    const StringObjectException error = print.validate();
    REQUIRE_FALSE(error.string.empty());
    CHECK(error.string.find("Inner and outer walls") != std::string::npos);
    CHECK(error.opt_key == "inner_wall_filament_id");
}

TEST_CASE("Wall cadence rejects layer-range height overrides", "[FeatureCadence][Validate]")
{
    const DynamicPrintConfig config = mixed_nozzle_grid_config();
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"wall_layer_height", 0.1}}};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    DynamicPrintConfig range_config;
    range_config.set_key_value("layer_height", new ConfigOptionFloat(0.15));
    model.objects.front()->layer_config_ranges[{1., 5.}].assign_config(std::move(range_config));
    print.apply(model, config);

    const StringObjectException error = print.validate();
    REQUIRE_FALSE(error.string.empty());
    CHECK(error.string.find("layer range") != std::string::npos);
    CHECK(error.string.find("wall cadence") != std::string::npos);
    CHECK(error.opt_key == "layer_height");
}

TEST_CASE("A valid dual-tool wall cadence passes validation", "[FeatureCadence][Validate]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({
        {"before_layer_change_gcode", "G92 E0"},
        {"bridge_line_width", 0.},
        {"skin_infill_line_width", 0.},
        {"skeleton_infill_line_width", 0.},
    });
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"wall_layer_height", 0.1}}};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    const StringObjectException error = print.validate();
    CAPTURE(error.string, error.opt_key);
    CHECK(error.string.empty());
}

TEST_CASE("Ratio-one validation preserves the original layer-height error", "[FeatureCadence][Validate]")
{
    const DynamicPrintConfig config = multifilament_config(1, {
        {"layer_height", 0.25},
        {"initial_layer_print_height", 0.2},
        {"nozzle_diameter", "0.2"},
        {"min_layer_height", "0.05"},
        {"max_layer_height", "0.15"},
        {"skirt_loops", 0},
    });
    Print print;
    Model model;
    init_print({cube(20.)}, print, model, config);

    const StringObjectException error = print.validate();
    CHECK(error.string == "Layer height cannot exceed nozzle diameter.");
    CHECK(error.opt_key == "layer_height");
}
