#include <catch2/catch_all.hpp>

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

    size_t path_count = 0;
    for (const Layer *layer : print.objects().front()->layers()) {
        const auto paths = paths_with_role(*layer, erInternalInfill);
        if (paths.empty())
            continue;
        ++path_count;
        CHECK(is_base_z(layer->print_z));
        for (const ExtrusionPath *path : paths)
            CHECK_THAT(path->height, Catch::Matchers::WithinAbs(0.2, EPSILON));
    }
    CHECK(path_count > 0);
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
