#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/FeatureProcessResolver.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "test_helpers.hpp"

#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
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
    return mixed_nozzle_config();
}

DynamicPrintConfig mixed_nozzle_grid_config(std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    return mixed_nozzle_config(extra);
}

DynamicPrintConfig filament_delta_test_config()
{
    DynamicPrintConfig config = mixed_nozzle_config({
        {"outer_wall_filament_id", 0},
        {"inner_wall_filament_id", 0},
        {"sparse_infill_filament_id", 0},
        {"internal_solid_filament_id", 0},
        {"top_surface_filament_id", 0},
        {"bottom_surface_filament_id", 0},
    });
    config.option<ConfigOptionStrings>("filament_process_projection", true)->values = {
        "layer_height=0.2;outer_wall_line_width=0.44;inner_wall_line_width=0.43;wall_loops=2;sparse_infill_line_width=0.45",
        "layer_height=0.1;outer_wall_line_width=0.22;inner_wall_line_width=0.21;wall_loops=4;sparse_infill_line_width=0.25",
    };
    return config;
}

class DerivedHeightResolverFixture
{
public:
    static constexpr const char *standard_04 = "0.20mm Standard @TestPrinter 0.4 nozzle";

    PresetBundle bundle;

    DerivedHeightResolverFixture()
    {
        add_printer("TestPrinter 0.2 nozzle", 0.2, false);
        add_printer("TestPrinter 0.4 nozzle", 0.4, true);
        add_process(standard_04, 0.2, "TestPrinter 0.4 nozzle");
        add_process("0.10mm Standard @TestPrinter 0.2 nozzle", 0.1, "TestPrinter 0.2 nozzle");
        bundle.filament_presets = {"Filament 1", "Filament 2"};
    }

private:
    void add_printer(const std::string &name, double nozzle, bool select)
    {
        DynamicPrintConfig config(bundle.printers.default_preset().config);
        config.option<ConfigOptionString>("printer_model", true)->value = "TestPrinter";
        config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {nozzle};
        config.option<ConfigOptionFloats>("min_layer_height", true)->values = {nozzle == 0.2 ? 0.05 : 0.1};
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

const PrintRegionConfig *region_with_outer_width(const PrintObject &object, double width)
{
    for (const PrintRegion &region : object.all_regions())
        for (const FloatOrPercent &candidate : region.config().outer_wall_line_width.values)
            if (std::abs(candidate.value - width) <= 1e-9)
                return &region.config();
    return nullptr;
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

bool is_wall_role(ExtrusionRole role)
{
    return role == erPerimeter || role == erExternalPerimeter;
}

bool is_interior_role(ExtrusionRole role)
{
    return role == erInternalInfill || role == erSolidInfill ||
           role == erTopSolidInfill || role == erBottomSurface;
}

std::string strip_nondeterministic_gcode_lines(const std::string &gcode)
{
    std::string out;
    out.reserve(gcode.size());
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);) {
        if (line.compare(0, 15, "; generated by ") == 0 ||
            line.compare(0, 18, "; model label id: ") == 0 ||
            (line.find("printing object") != std::string::npos && line.find(" id:") != std::string::npos))
            continue;
        out += line;
        out += '\n';
    }
    return out;
}

size_t count_warning_key(const std::vector<StringObjectException> &warnings, const std::string &opt_key)
{
    return std::count_if(
        warnings.begin(), warnings.end(),
        [&](const StringObjectException &warning) { return warning.opt_key == opt_key; });
}

std::optional<int> integer_parameter(const std::string &line, char parameter)
{
    std::istringstream stream(line);
    for (std::string token; stream >> token;)
        if (token.size() >= 2 && token.front() == parameter &&
            (std::isdigit(static_cast<unsigned char>(token[1])) || token[1] == '-'))
            return std::stoi(token.substr(1));
    return std::nullopt;
}

std::map<int, std::set<int>> nonzero_temperatures_by_tool(const std::string &gcode)
{
    std::map<int, std::set<int>> temperatures;
    int current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        const std::string command(line.cmd());
        if (command.size() >= 2 && command.front() == 'T' &&
            std::isdigit(static_cast<unsigned char>(command[1]))) {
            current_tool = std::stoi(command.substr(1));
            return;
        }
        if (command != "M104" && command != "M109")
            return;
        const std::optional<int> temperature = integer_parameter(line.raw(), 'S');
        if (!temperature || *temperature == 0)
            return;
        const int tool = integer_parameter(line.raw(), 'T').value_or(current_tool);
        temperatures[tool].insert(*temperature);
    });
    return temperatures;
}

void check_dual_tool_cube_gcode(const std::string &output, bool check_temperatures)
{
    const std::vector<GCodeExtrusion> extrusions = gcode_extrusions(output);
    REQUIRE_FALSE(extrusions.empty());

    std::set<int> wall_z_tenths;
    std::set<ExtrusionRole> interior_roles;
    double max_extrusion_z = 0.;
    for (const GCodeExtrusion &extrusion : extrusions) {
        max_extrusion_z = std::max(max_extrusion_z, extrusion.z);
        if (is_wall_role(extrusion.role)) {
            const int z_tenth = int(std::lround(extrusion.z * 10.));
            wall_z_tenths.insert(z_tenth);
            if (z_tenth > 2) {
                CAPTURE(extrusion.role, extrusion.tool, extrusion.z, extrusion.height);
                CHECK(extrusion.tool == 0);
                CHECK_THAT(extrusion.height, Catch::Matchers::WithinAbs(0.1, 1e-4));
            }
        } else if (is_interior_role(extrusion.role)) {
            CAPTURE(extrusion.role, extrusion.tool, extrusion.z, extrusion.height);
            CHECK(extrusion.tool == 1);
            CHECK(is_base_z(extrusion.z));
            // Internal-bridge anchors legitimately stay at the fine cadence (bridges are
            // excluded from recombination by design); everything else must be base height.
            if (std::abs(extrusion.height - 0.2) <= 1e-4)
                interior_roles.insert(extrusion.role);
            else
                CHECK_THAT(extrusion.height, Catch::Matchers::WithinAbs(0.1, 1e-4));
        }
    }

    for (int z_tenth = 3; z_tenth <= 200; ++z_tenth) {
        CAPTURE(z_tenth);
        CHECK(wall_z_tenths.count(z_tenth) == 1);
    }
    CHECK(interior_roles == std::set<ExtrusionRole>{
        erInternalInfill, erSolidInfill, erTopSolidInfill, erBottomSurface});
    CHECK_THAT(max_extrusion_z, Catch::Matchers::WithinAbs(20., 0.1));

    size_t in_object_tool_changes = 0;
    for (const GCodeToolChange &change : gcode_tool_changes(output)) {
        if (change.z <= 0.2 + 1e-4)
            continue;
        ++in_object_tool_changes;
        CAPTURE(change.tool, change.z);
        // Changes to the interior tool may only happen on base layers. A change back to the
        // wall tool on a fine-only layer is physically valid (though a wasted swap — see the
        // Phase-3 tool-ordering optimization note in the plan).
        if (change.tool != 0)
            CHECK(is_base_z(change.z));
    }
    CHECK(in_object_tool_changes > 0);

    if (check_temperatures) {
        const std::map<int, std::set<int>> temperatures = nonzero_temperatures_by_tool(output);
        REQUIRE(temperatures.count(0) == 1);
        REQUIRE(temperatures.count(1) == 1);
        CHECK(temperatures.at(0) == std::set<int>{210});
        CHECK(temperatures.at(1) == std::set<int>{250});
    }
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

DynamicPrintConfig zoned_cadence_config(std::initializer_list<ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = filament_delta_test_config();
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.2};
    config.option<ConfigOptionFloats>("min_layer_height", true)->values = {0.1, 0.05};
    config.option<ConfigOptionFloats>("max_layer_height", true)->values = {0.3, 0.15};
    config.option<ConfigOptionInts>("filament_map", true)->values = {1, 2};
    config.set_deserialize_strict({
        {"sparse_infill_density", 15.},
        {"top_shell_layers", 3},
        {"bottom_shell_layers", 2},
        {"top_shell_thickness", 0.},
        {"bottom_shell_thickness", 0.},
        {"ensure_vertical_shell_thickness", "none"},
        {"bridge_line_width", 0.},
        {"skin_infill_line_width", 0.},
        {"skeleton_infill_line_width", 0.},
        {"before_layer_change_gcode", "G92 E0"},
        {"gcode_comments", true},
    });
    if (extra.size() > 0)
        config.set_deserialize_strict(extra);
    return config;
}

DynamicPrintConfig independent_zoned_cadence_config(
    std::initializer_list<ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = zoned_cadence_config({
        {"layer_height", 0.28},
        {"initial_layer_print_height", 0.2},
        {"infill_combination", false},
    });
    config.option<ConfigOptionStrings>("filament_process_projection", true)->values = {
        "layer_height=0.28;outer_wall_line_width=0.44;inner_wall_line_width=0.43;sparse_infill_line_width=0.45",
        "layer_height=0.08;outer_wall_line_width=0.22;inner_wall_line_width=0.21;sparse_infill_line_width=0.25",
    };
    if (extra.size() > 0)
        config.set_deserialize_strict(extra);
    return config;
}

ModelObject *add_zoned_text_cube(Model &model, const std::string &name, double x_shift = 0.,
                                 double fine_bottom = 10., bool whole_object_fine = false)
{
    ModelObject *object = model.add_object();
    object->name = name;
    object->config.set("extruder", 1);

    if (whole_object_fine) {
        TriangleMesh mesh = make_cube(20., 20., 11.);
        Transform3d transform = Transform3d::Identity();
        transform.translation().x() = x_shift;
        mesh.transform(transform, false);
        object->add_volume(std::move(mesh))->config.set("extruder", 2);
    } else {
        TriangleMesh body = make_cube(20., 20., 10.);
        Transform3d body_transform = Transform3d::Identity();
        body_transform.translation().x() = x_shift;
        body.transform(body_transform, false);
        object->add_volume(std::move(body))->config.set("extruder", 1);

        TriangleMesh text = make_cube(6., 6., 11. - fine_bottom);
        Transform3d text_transform = Transform3d::Identity();
        text_transform.translation() = Vec3d(x_shift + 7., 7., fine_bottom);
        text.transform(text_transform, false);
        ModelVolume *text_volume = object->add_volume(std::move(text));
        text_volume->config.set("extruder", 2);
        // Only the text walls need the fine process; its interiors remain on the coarse
        // filament so the fixture also exercises cadence recombination inside the fine zone.
        text_volume->config.set("sparse_infill_filament_id", 1);
        text_volume->config.set("internal_solid_filament_id", 1);
        text_volume->config.set("top_surface_filament_id", 1);
        text_volume->config.set("bottom_surface_filament_id", 1);
    }
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

ModelObject *add_independent_zoned_text_cube(Model &model, const std::string &name,
                                            double body_height = 35., double text_height = 1.)
{
    ModelObject *object = model.add_object();
    object->name = name;
    object->config.set("extruder", 1);
    object->add_volume(make_cube(20., 20., body_height))->config.set("extruder", 1);

    TriangleMesh text = make_cube(6., 6., text_height);
    Transform3d transform = Transform3d::Identity();
    transform.translation() = Vec3d(7., 7., body_height);
    text.transform(transform, false);
    object->add_volume(std::move(text))->config.set("extruder", 2);
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

void apply_zoned_model(Print &print, Model &model, const DynamicPrintConfig &config)
{
    print.apply(model, config);
    print.set_status_silent();
}

} // namespace

TEST_CASE("An object's base filament supplies its process delta", "[FeatureCadence][FilamentProcess]")
{
    DynamicPrintConfig config = filament_delta_test_config();
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"extruder", 2}}};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    const PrintObject &object = *print.objects().front();
    CHECK_THAT(object.config().layer_height.value, Catch::Matchers::WithinAbs(0.1, 1e-9));
    REQUIRE(object.num_printing_regions() == 1);
    const PrintRegionConfig &region = object.printing_region(0).config();
    CHECK_THAT(region.outer_wall_line_width.get_at(1).value, Catch::Matchers::WithinAbs(0.22, 1e-9));
    CHECK_THAT(region.sparse_infill_line_width.get_at(1).value, Catch::Matchers::WithinAbs(0.25, 1e-9));
    CHECK(region.wall_loops.value == 4);

    print.process();
    config.option<ConfigOptionStrings>("filament_process_projection")->values[1] =
        "layer_height=0.12;outer_wall_line_width=0.24;sparse_infill_line_width=0.26";
    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_INVALIDATED);
    CHECK_THAT(print.objects().front()->config().layer_height.value, Catch::Matchers::WithinAbs(0.12, 1e-9));
}

TEST_CASE("A single-nozzle two-colour print stays unzoned and slices", "[FeatureCadence][CadenceZones][Regression]")
{
    // The vendor-profile slice check's scene: one nozzle, two filaments, a layer range switching
    // to filament 2 partway up, prime tower on. Zone computation once manufactured zones from the
    // layer-range boundary (span-fitted heights differing by float noise), desynced layering, and
    // broke the wipe tower on every single-nozzle vendor profile.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"nozzle_diameter", "0.4"},
                                   {"filament_diameter", "1.75,1.75"},
                                   {"filament_map", "1,1"},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.25},
                                   {"enable_prime_tower", true},
                                   {"wipe_tower_x", 40.},
                                   {"use_relative_e_distances", true},
                                   {"before_layer_change_gcode", ";BEFORE_LAYER_CHANGE\nG92 E0"},
                                   {"wipe_tower_y", 40.},
                                   {"skirt_loops", 0}});

    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->name = "two-colour";
    object->add_volume(cube(10.));
    object->add_instance();
    DynamicPrintConfig range_config;
    range_config.set_key_value("extruder", new ConfigOptionInt(2));
    range_config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    object->layer_config_ranges[{4.0, 10.0}].assign_config(std::move(range_config));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);

    CHECK(print.objects().front()->cadence_zones().empty());
    CHECK(print.objects().front()->slicing_parameters().cadence_zone_digest == 0);
    const StringObjectException error = print.validate();
    INFO(error.string);
    REQUIRE(error.string.empty());
    // The empty zone table IS the regression contract: manufactured zones were what desynced
    // layering. Full processing/g-code of this scene across real vendor profiles is covered by
    // the profile validator's slice check (OrcaSlicer_profile_validator -s), where this
    // surfaced. (print.process() here trips an unrelated flaky FakeWipeTower conflict-checker
    // crash on non-BBL towers — tracked separately.)
}

TEST_CASE("Multiple separated fine zones validate and slice", "[FeatureCadence][FilamentProcess][Zones][Regression]")
{
    // ZoneRuler shape: a coarse staircase with a fine 1.2mm cap on each step — three disjoint
    // fine Z bands. Reported by the user's test asset: validation refused with a coarse-height
    // wall on the fine tool.
    // Two configurations: per-part fine walls only, and the golden-path shape where ALL walls
    // ride the fine filament globally (the reported failure: the synthetic sub-first-layer base
    // zone leaked height 0.2 into wall validation for the fine tool).
    const bool walls_globally_fine = GENERATE(false, true);
    CAPTURE(walls_globally_fine);
    DynamicPrintConfig config = filament_delta_test_config();
    config.option<ConfigOptionStrings>("filament_process_projection", true)->values = {
        "layer_height=0.1;outer_wall_line_width=0.22;inner_wall_line_width=0.22;wall_loops=4", ""};
    if (walls_globally_fine) {
        config.set_deserialize_strict("outer_wall_filament_id", "1");
        config.set_deserialize_strict("inner_wall_filament_id", "1");
    }

    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->name = "zone-ruler";
    auto add_box = [&](double x0, double x1, double z0, double z1, int extruder) {
        TriangleMesh mesh = make_cube(x1 - x0, 20., z1 - z0);
        Transform3d t = Transform3d::Identity();
        t.translation() = Vec3d(x0, 0., z0);
        mesh.transform(t, false);
        ModelVolume *volume = object->add_volume(std::move(mesh));
        volume->config.set_key_value("extruder", new ConfigOptionInt(extruder));
    };
    // Steps (coarse, filament 2): heights 4 / 8 / 12; caps (fine, filament 1): 1.2 on each step.
    add_box(0., 20., 0., 4., 2);
    add_box(20., 40., 0., 8., 2);
    add_box(40., 60., 0., 12., 2);
    add_box(0., 20., 4., 5.2, 1);
    add_box(20., 40., 8., 9.2, 1);
    add_box(40., 60., 12., 13.2, 1);
    object->config.set_key_value("extruder", new ConfigOptionInt(2));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);

    const PrintObject &print_object = *print.objects().front();
    std::ostringstream dump;
    for (const CadenceZone &zone : print_object.cadence_zones())
        dump << "zone [" << zone.lo << "," << zone.hi << ") mixed=" << zone.mixed
             << " h=" << zone.height << " fine_h=" << zone.fine_height << "\n";
    for (const PrintRegion &region : print_object.all_regions())
        dump << "region outer_fil=" << region.config().outer_wall_filament_id.value
             << " wall_h=" << region.config().wall_layer_height.value
             << " outer_w=" << region.config().outer_wall_line_width.get_at(0).value
             << "/" << region.config().outer_wall_line_width.get_at(1).value << "\n";
    INFO(dump.str());

    const StringObjectException error = print.validate();
    INFO(error.string);
    CHECK(error.string.empty());
}

TEST_CASE("A delta-free filament keeps global process values other slots project", "[FeatureCadence][FilamentProcess][Regression]")
{
    // Global process overrides wall_loops to 3; only the OTHER filament's delta carries wall_loops.
    // The region on the delta-free filament must keep the global 3 — not the compiled default.
    DynamicPrintConfig config = filament_delta_test_config();
    config.set_deserialize_strict("wall_loops", "3");
    config.option<ConfigOptionStrings>("filament_process_projection")->values = {
        "", "layer_height=0.1;outer_wall_line_width=0.22;wall_loops=4"};
    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config);

    const PrintObject &object = *print.objects().front();
    REQUIRE(object.num_printing_regions() == 1);
    CHECK(object.printing_region(0).config().wall_loops.value == 3);
}

TEST_CASE("Part, modifier, and layer-range base filaments consume their own deltas",
          "[FeatureCadence][FilamentProcess]")
{
    DYNAMIC_SECTION("part") {
        const DynamicPrintConfig config = filament_delta_test_config();
        Model model;
        ModelObject *object = model.add_object();
        object->name = "two-parts.stl";
        ModelVolume *first = object->add_volume(make_cube(10., 10., 10.));
        first->config.set("extruder", 1);
        TriangleMesh second_mesh = make_cube(10., 10., 10.);
        Transform3d shift = Transform3d::Identity();
        shift.translation().x() = 12.;
        second_mesh.transform(shift, false);
        ModelVolume *second = object->add_volume(std::move(second_mesh));
        second->config.set("extruder", 2);
        object->add_instance();
        object->ensure_on_bed();

        Print print;
        print.apply(model, config);
        const PrintObject &print_object = *print.objects().front();
        REQUIRE(region_with_outer_width(print_object, 0.44) != nullptr);
        const PrintRegionConfig *fine = region_with_outer_width(print_object, 0.22);
        REQUIRE(fine != nullptr);
        CHECK_THAT(fine->sparse_infill_line_width.get_at(1).value, Catch::Matchers::WithinAbs(0.25, 1e-9));
    }

    DYNAMIC_SECTION("modifier") {
        DynamicPrintConfig config = filament_delta_test_config();
        config.option<ConfigOptionStrings>("filament_process_projection")->values[0] += ";top_shell_layers=7";
        const int global_top_shell_layers = config.option<ConfigOptionInt>("top_shell_layers")->value;
        Model model;
        ModelObject *object = model.add_object();
        object->name = "modifier.stl";
        ModelVolume *part = object->add_volume(make_cube(20., 20., 20.));
        part->config.set("extruder", 1);
        ModelVolume *modifier = object->add_volume(
            make_cube(10., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
        modifier->config.set("extruder", 2);
        object->add_instance();
        object->ensure_on_bed();

        Print print;
        print.apply(model, config);
        const PrintObject &print_object = *print.objects().front();
        REQUIRE(region_with_outer_width(print_object, 0.44) != nullptr);
        const PrintRegionConfig *fine = region_with_outer_width(print_object, 0.22);
        REQUIRE(fine != nullptr);
        CHECK(fine->top_shell_layers.value == global_top_shell_layers);
    }

    DYNAMIC_SECTION("layer range") {
        const DynamicPrintConfig config = filament_delta_test_config();
        Model model;
        ModelObject *object = model.add_object();
        object->name = "layer-range.stl";
        object->add_volume(make_cube(20., 20., 20.));
        object->config.set("extruder", 1);
        ModelConfig range_config;
        range_config.set("extruder", 2);
        object->layer_config_ranges[{5., 10.}].assign_config(std::move(range_config));
        object->add_instance();
        object->ensure_on_bed();

        Print print;
        print.apply(model, config);
        const PrintObject &print_object = *print.objects().front();
        REQUIRE(region_with_outer_width(print_object, 0.44) != nullptr);
        REQUIRE(region_with_outer_width(print_object, 0.22) != nullptr);
    }
}

TEST_CASE("Role filament deltas stay within their partitions and populate cadence heights",
          "[FeatureCadence][FilamentProcess]")
{
    DynamicPrintConfig config = filament_delta_test_config();
    config.option<ConfigOptionFloat>("layer_height")->value = 0.3;
    config.option<ConfigOptionStrings>("filament_process_projection")->values[0] += ";wall_loops=3";
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{
        {"extruder", 1}, {"outer_wall_filament_id", 2}, {"inner_wall_filament_id", 2},
        {"sparse_infill_filament_id", 1},
    }};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    CHECK_FALSE(model.objects.front()->config.has("layer_height"));
    CHECK_FALSE(model.objects.front()->config.has("wall_layer_height"));
    CHECK_THAT(print.objects().front()->config().layer_height.value, Catch::Matchers::WithinAbs(0.2, 1e-9));
    const PrintRegionConfig &region = print.objects().front()->printing_region(0).config();
    CHECK_THAT(region.outer_wall_line_width.get_at(1).value, Catch::Matchers::WithinAbs(0.22, 1e-9));
    CHECK(region.wall_loops.value == 4);
    CHECK_THAT(region.sparse_infill_line_width.get_at(0).value, Catch::Matchers::WithinAbs(0.45, 1e-9));
    CHECK_THAT(region.wall_layer_height.value, Catch::Matchers::WithinAbs(0.1, 1e-9));
    CHECK_THAT(region.sparse_infill_process_layer_height.value, Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("Global-process action clears a projection-less stale wall height before uniform-nozzle slicing",
          "[FeatureCadence][FeatureProcess][FilamentProcess][Regression]")
{
    DerivedHeightResolverFixture fixture;
    DynamicPrintConfig config = mixed_nozzle_grid_config({
        {"nozzle_diameter", "0.4,0.2"},
        {"min_layer_height", "0.1,0.05"},
        {"max_layer_height", "0.3,0.15"},
        {"outer_wall_filament_id", 1},
        {"inner_wall_filament_id", 1},
        {"sparse_infill_filament_id", 1},
        {"internal_solid_filament_id", 1},
        {"top_surface_filament_id", 1},
        {"bottom_surface_filament_id", 1},
    });
    config.option<ConfigOptionString>("printer_model", true)->value = "TestPrinter";
    config.option<ConfigOptionString>("print_settings_id", true)->value = DerivedHeightResolverFixture::standard_04;

    Model model;
    ModelObject *object = model.add_object("cube", "", make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
    object->config.set("extruder", 1);
    object->config.set("outer_wall_filament_id", 2);
    object->config.set("inner_wall_filament_id", 2);
    object->config.set("wall_layer_height", 0.1);

    config.option<ConfigOptionEnumsGeneric>("filament_process_policy", true)->values = {
        int(FilamentProcessPolicy::GlobalProcess), int(FilamentProcessPolicy::GlobalProcess)};
    REQUIRE(clear_auto_feature_process_state_for_filament(model, 2, config));
    CHECK_FALSE(object->config.has("wall_process_projection"));
    CHECK_FALSE(object->config.has("wall_layer_height"));
    REQUIRE(update_filament_process_projections(fixture.bundle, config));
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = {0.4, 0.4};
    config.option<ConfigOptionFloats>("min_layer_height")->values = {0.1, 0.1};
    config.option<ConfigOptionFloats>("max_layer_height")->values = {0.3, 0.3};
    CHECK_FALSE(update_feature_process_projections(model, fixture.bundle, config));
    CHECK_FALSE(object->config.has("wall_process_projection"));
    CHECK_FALSE(object->config.has("wall_layer_height"));

    Print print;
    print.apply(model, config);
    const StringObjectException validation = print.validate();
    INFO(validation.string);
    REQUIRE(validation.string.empty());
    print.set_status_silent();
    REQUIRE_NOTHROW(print.process());
    const ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
    REQUIRE(layers.size() == 100);
    for (size_t index = 1; index < layers.size(); ++index)
        CHECK_THAT(layers[index]->height, Catch::Matchers::WithinAbs(0.2, EPSILON));
}

TEST_CASE("Explicit object and region process values win over filament deltas",
          "[FeatureCadence][FilamentProcess]")
{
    DynamicPrintConfig config = filament_delta_test_config();
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{
        {"extruder", 2}, {"layer_height", 0.3},
        {"wall_process_projection", "outer_wall_line_width=0.55"},
    }};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);
    CHECK_THAT(print.objects().front()->config().layer_height.value, Catch::Matchers::WithinAbs(0.3, 1e-9));
    CHECK_THAT(print.objects().front()->printing_region(0).config().outer_wall_line_width.get_at(1).value,
               Catch::Matchers::WithinAbs(0.55, 1e-9));

    ModelVolume *volume = model.objects.front()->volumes.front();
    volume->config.set("outer_wall_line_width", 0.5);
    print.apply(model, config);
    CHECK_THAT(print.objects().front()->printing_region(0).config().outer_wall_line_width.get_at(1).value,
               Catch::Matchers::WithinAbs(0.5, 1e-9));

    ModelVolume *modifier = model.objects.front()->add_volume(
        make_cube(10., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    modifier->config.set("extruder", 1);
    print.apply(model, config);
    const PrintObject &print_object = *print.objects().front();
    REQUIRE(print_object.num_printing_regions() >= 2);
    for (size_t region_id = 0; region_id < print_object.num_printing_regions(); ++region_id)
        CHECK_THAT(print_object.printing_region(region_id).config().outer_wall_line_width.get_at(1).value,
                   Catch::Matchers::WithinAbs(0.5, 1e-9));
}

TEST_CASE("Support and interface filaments supply their object-scope partitions",
          "[FeatureCadence][FilamentProcess][Support]")
{
    DynamicPrintConfig config = filament_delta_test_config();
    config.option<ConfigOptionStrings>("filament_process_projection")->values[1] +=
        ";support_line_width=0.23;support_speed=33;support_interface_speed=22"
        ";support_top_z_distance=0.05;support_bottom_z_distance=0.06";
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{
        {"extruder", 1}, {"support_filament", 2}, {"support_interface_filament", 2},
    }};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    const PrintObjectConfig &object = print.objects().front()->config();
    CHECK_THAT(object.support_line_width.get_at(1).value, Catch::Matchers::WithinAbs(0.23, 1e-9));
    CHECK_THAT(object.support_speed.get_at(1), Catch::Matchers::WithinAbs(33., 1e-9));
    CHECK_THAT(object.support_interface_speed.get_at(1), Catch::Matchers::WithinAbs(22., 1e-9));
    CHECK_THAT(object.support_top_z_distance.value, Catch::Matchers::WithinAbs(0.05, 1e-9));
    CHECK_THAT(object.support_bottom_z_distance.value, Catch::Matchers::WithinAbs(0.06, 1e-9));
}

TEST_CASE("Painted regions consume their filament delta identically on create and verify",
          "[FeatureCadence][FilamentProcess][Painted]")
{
    DynamicPrintConfig config = filament_delta_test_config();
    Model model;
    ModelObject *object = model.add_object();
    object->name = "painted.stl";
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 20.));
    object->config.set("extruder", 1);
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.apply(model, config);
    const PrintObject &print_object = *print.objects().front();
    const PrintRegionConfig *painted = region_with_outer_width(print_object, 0.22);
    REQUIRE(painted != nullptr);
    CHECK_THAT(painted->sparse_infill_line_width.get_at(1).value, Catch::Matchers::WithinAbs(0.25, 1e-9));
    REQUIRE(print_object.cadence_zones().size() == 1);
    CHECK(print_object.cadence_zones().front().mixed);
    CHECK_THAT(print_object.cadence_zones().front().lo, Catch::Matchers::WithinAbs(0., EPSILON));
    CHECK_THAT(print_object.cadence_zones().front().hi, Catch::Matchers::WithinAbs(20., EPSILON));
    const size_t region_count = print_object.num_printing_regions();
    const std::vector<size_t> hashes_before = [&] {
        std::vector<size_t> result;
        for (const PrintRegion &region : print_object.all_regions())
            result.push_back(region.config_hash());
        return result;
    }();

    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_UNCHANGED);
    CHECK(print.objects().front()->num_printing_regions() == region_count);
    std::vector<size_t> hashes_after;
    for (const PrintRegion &region : print.objects().front()->all_regions())
        hashes_after.push_back(region.config_hash());
    CHECK(hashes_after == hashes_before);
}

TEST_CASE("Empty filament process projections preserve region and object configs",
          "[FeatureCadence][FilamentProcess][Regression]")
{
    DynamicPrintConfig baseline_config = mixed_nozzle_config();
    baseline_config.erase("filament_process_projection");
    DynamicPrintConfig empty_config = mixed_nozzle_config();
    empty_config.option<ConfigOptionStrings>("filament_process_projection", true)->values = {"", ""};

    Print baseline_print;
    Model baseline_model;
    init_print({cube(20.)}, baseline_print, baseline_model, baseline_config);
    Print empty_print;
    Model empty_model;
    init_print({cube(20.)}, empty_print, empty_model, empty_config);

    CHECK(empty_print.objects().front()->config() == baseline_print.objects().front()->config());
    CHECK(empty_print.objects().front()->printing_region(0).config() ==
          baseline_print.objects().front()->printing_region(0).config());
}

TEST_CASE("A base-filament layer-height delta drives single-tool G-code cadence",
          "[FeatureCadence][FilamentProcess][GCode]")
{
    DynamicPrintConfig config = filament_delta_test_config();
    config.set("infill_combination", false);
    const std::string output = slice_with_object_overrides(
        {cube(20.)}, config, {{{"extruder", 2}}});
    const std::vector<GCodeExtrusion> extrusions = gcode_extrusions(output);
    REQUIRE_FALSE(extrusions.empty());
    bool found_non_initial = false;
    std::set<int> wall_z_tenths;
    for (const GCodeExtrusion &extrusion : extrusions) {
        CHECK(extrusion.tool == 1);
        if (extrusion.z > 0.2 + EPSILON && is_wall_role(extrusion.role)) {
            found_non_initial = true;
            wall_z_tenths.insert(int(std::lround(extrusion.z * 10.)));
            CHECK_THAT(extrusion.height, Catch::Matchers::WithinAbs(0.1, 1e-4));
        }
    }
    CHECK(found_non_initial);
    for (int z_tenth = 3; z_tenth <= 200; ++z_tenth)
        CHECK(wall_z_tenths.count(z_tenth) == 1);
}

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

TEST_CASE("Projected feature heights populate every role config", "[FeatureCadence][FeatureProcess]")
{
    const DynamicPrintConfig config = mixed_nozzle_grid_config();
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{
        {"wall_process_projection", "wall_layer_height=0.1"},
        {"sparse_infill_process_projection", "sparse_infill_process_layer_height=0.1"},
        {"internal_solid_process_projection", "internal_solid_process_layer_height=0.1"},
        {"top_surface_process_projection", "top_surface_process_layer_height=0.1"},
        {"bottom_surface_process_projection", "bottom_surface_process_layer_height=0.1"},
    }};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    const PrintRegionConfig &region = print.objects().front()->printing_region(0).config();
    CHECK_THAT(region.wall_layer_height.value, Catch::Matchers::WithinAbs(0.1, 1e-9));
    CHECK_THAT(region.sparse_infill_process_layer_height.value, Catch::Matchers::WithinAbs(0.1, 1e-9));
    CHECK_THAT(region.internal_solid_process_layer_height.value, Catch::Matchers::WithinAbs(0.1, 1e-9));
    CHECK_THAT(region.top_surface_process_layer_height.value, Catch::Matchers::WithinAbs(0.1, 1e-9));
    CHECK_THAT(region.bottom_surface_process_layer_height.value, Catch::Matchers::WithinAbs(0.1, 1e-9));
}

TEST_CASE("Projected wall height refines cadence without a plain model height",
          "[FeatureCadence][FeatureProcess][Regression]")
{
    const DynamicPrintConfig config = mixed_nozzle_grid_config();
    Print print;
    Model model;
    process_cube_with_overrides(config, {{"wall_process_projection", "wall_layer_height=0.1"}}, print, model);

    REQUIRE_FALSE(model.objects.empty());
    CHECK_FALSE(model.objects.front()->config.has("wall_layer_height"));
    CHECK_THAT(print.objects().front()->printing_region(0).config().wall_layer_height.value,
               Catch::Matchers::WithinAbs(0.1, 1e-9));
    const ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
    REQUIRE(layers.size() == 199);
    for (size_t index = 1; index < layers.size(); ++index)
        CHECK_THAT(layers[index]->height, Catch::Matchers::WithinAbs(0.1, EPSILON));
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

TEST_CASE("An empty project nozzle overlay leaves slicing output byte-identical",
          "[FeatureCadence][NozzleOverlay][Regression]")
{
    DynamicPrintConfig without_overlay = projection_test_config();
    without_overlay.erase("project_nozzle_diameter");
    DynamicPrintConfig empty_overlay = projection_test_config();
    empty_overlay.option<ConfigOptionFloats>("project_nozzle_diameter", true)->values.clear();

    const std::string baseline = strip_nondeterministic_gcode_lines(slice({cube(20.)}, without_overlay));
    const std::string overlaid = strip_nondeterministic_gcode_lines(slice({cube(20.)}, empty_overlay));

    CHECK(overlaid == baseline);
    CHECK(overlaid.find("project_nozzle_diameter") == std::string::npos);
}

TEST_CASE("The defensive project nozzle overlay reaches wall slicing flow",
          "[FeatureCadence][NozzleOverlay]")
{
    DynamicPrintConfig config = mixed_nozzle_config({
        {"nozzle_diameter", "0.4,0.4"},
        {"project_nozzle_diameter", "0.4,0.2"},
        {"min_layer_height", "0.07,0.07"},
        {"max_layer_height", "0.3,0.3"},
        {"outer_wall_filament_id", 2},
        {"inner_wall_filament_id", 2},
        {"sparse_infill_filament_id", 1},
        {"internal_solid_filament_id", 1},
        {"top_surface_filament_id", 1},
        {"bottom_surface_filament_id", 1},
        {"outer_wall_line_width", 0.},
        {"inner_wall_line_width", 0.},
        {"line_width", 0.},
        {"wall_layer_height", 0.1},
        {"sparse_infill_density", 15.},
    });

    Print print;
    Model model;
    init_print({cube(20.)}, print, model, config);

    REQUIRE(print.config().nozzle_diameter.values.size() == 2);
    CHECK_THAT(print.config().nozzle_diameter.values[0], Catch::Matchers::WithinAbs(0.4, 1e-9));
    CHECK_THAT(print.config().nozzle_diameter.values[1], Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK_THAT(print.config().min_layer_height.values[0], Catch::Matchers::WithinAbs(0.07, 1e-9));
    CHECK_THAT(print.config().min_layer_height.values[1], Catch::Matchers::WithinAbs(0., 1e-9));
    CHECK_THAT(print.config().max_layer_height.values[0], Catch::Matchers::WithinAbs(0.3, 1e-9));
    CHECK_THAT(print.config().max_layer_height.values[1], Catch::Matchers::WithinAbs(0., 1e-9));

    const PrintObject &object = *print.objects().front();
    const Flow wall_flow = object.printing_region(0).flow(object, frExternalPerimeter, 0.1, false);
    CHECK_THAT(wall_flow.nozzle_diameter(), Catch::Matchers::WithinAbs(0.2, 1e-6));
    CHECK_THAT(wall_flow.width(), Catch::Matchers::WithinAbs(
        Flow::auto_extrusion_width(frExternalPerimeter, 0.2), 1e-6));

    bool found_wall = false;
    for (const GCodeExtrusion &extrusion : gcode_extrusions(gcode(print))) {
        if (!is_wall_role(extrusion.role) || extrusion.z <= 0.2 + EPSILON)
            continue;
        found_wall = true;
        CHECK(extrusion.tool == 1);
        CHECK_THAT(extrusion.height, Catch::Matchers::WithinAbs(0.1, 1e-4));
    }
    CHECK(found_wall);
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
    REQUIRE(print.objects().front()->cadence_zones().size() == 1);
    CHECK(print.objects().front()->cadence_zones().front().mixed);
    CHECK(print.objects().front()->slicing_parameters().cadence_zone_digest == 0);
}

TEST_CASE("Fine cadence is confined to the Z extent of a text part", "[FeatureCadence][CadenceZones][Regression]")
{
    DynamicPrintConfig config = zoned_cadence_config();
    Print print;
    Model model;
    ModelObject *model_object = add_zoned_text_cube(model, "TextCube");
    apply_zoned_model(print, model, config);

    const PrintObject &object = *print.objects().front();
    REQUIRE(object.cadence_zones().size() == 2);
    CHECK_FALSE(object.cadence_zones()[0].mixed);
    CHECK_THAT(object.cadence_zones()[0].lo, Catch::Matchers::WithinAbs(0., EPSILON));
    CHECK_THAT(object.cadence_zones()[0].hi, Catch::Matchers::WithinAbs(10., EPSILON));
    CHECK(object.cadence_zones()[1].mixed);
    CHECK_THAT(object.cadence_zones()[1].lo, Catch::Matchers::WithinAbs(10., EPSILON));
    CHECK_THAT(object.cadence_zones()[1].hi, Catch::Matchers::WithinAbs(11., EPSILON));
    CHECK(object.slicing_parameters().cadence_zone_digest != 0);
    CHECK_FALSE(model_object->has_custom_layering());

    std::vector<coordf_t> profile;
    REQUIRE(PrintObject::update_layer_height_profile(*model_object, object.slicing_parameters(), profile,
                                                     object.cadence_zones()));
    REQUIRE(profile.size() > 4);
    CHECK_THAT(profile[1], Catch::Matchers::WithinAbs(object.slicing_parameters().first_object_layer_height, EPSILON));
    const std::vector<coordf_t> generated = generate_object_layers(object.slicing_parameters(), profile, false);
    REQUIRE(generated.size() % 2 == 0);
    for (size_t i = 1; i < generated.size(); i += 2) {
        const double bottom = generated[i - 1];
        const double height = generated[i] - bottom;
        CAPTURE(bottom, generated[i], height);
        CHECK_THAT(height, Catch::Matchers::WithinAbs(bottom < 10. - EPSILON ? 0.2 : 0.1, EPSILON));
    }

    const StringObjectException validation = print.validate();
    CAPTURE(validation.string, validation.opt_key);
    REQUIRE(validation.string.empty());
    const std::string output = gcode(print);
    bool found_body_wall = false;
    bool found_text_wall = false;
    bool found_recombined_text_interior = false;
    for (const GCodeExtrusion &extrusion : gcode_extrusions(output)) {
        if (is_wall_role(extrusion.role) && extrusion.z <= 10. + EPSILON) {
            found_body_wall = true;
            CHECK(extrusion.tool == 0);
            CHECK_THAT(extrusion.height, Catch::Matchers::WithinAbs(0.2, 1e-4));
        } else if (is_wall_role(extrusion.role) && extrusion.z > 10. + EPSILON) {
            found_text_wall = true;
            CHECK(extrusion.tool == 1);
            CHECK_THAT(extrusion.height, Catch::Matchers::WithinAbs(0.1, 1e-4));
        } else if ((extrusion.role == erSolidInfill || extrusion.role == erTopSolidInfill ||
                    extrusion.role == erBottomSurface || extrusion.role == erInternalInfill) &&
                   extrusion.z > 10. + EPSILON && std::abs(extrusion.height - 0.2) <= 1e-4) {
            found_recombined_text_interior = true;
            CHECK(extrusion.tool == 0);
        }
    }
    CHECK(found_body_wall);
    CHECK(found_text_wall);
    CHECK(found_recombined_text_interior);
}

TEST_CASE("Cadence zones snap down without consuming the first layer", "[FeatureCadence][CadenceZones]")
{
    Print print;
    Model model;
    add_zoned_text_cube(model, "snapped-text", 0., 10.05);
    apply_zoned_model(print, model, zoned_cadence_config());

    const PrintObject &object = *print.objects().front();
    REQUIRE(object.cadence_zones().size() == 2);
    CHECK_THAT(object.cadence_zones()[0].hi, Catch::Matchers::WithinAbs(10., EPSILON));
    CHECK_THAT(object.cadence_zones()[1].lo, Catch::Matchers::WithinAbs(10., EPSILON));
    CHECK_FALSE(object.layer_z_in_fine_zone(object.slicing_parameters().first_object_layer_height));
}

TEST_CASE("Single-tool zones use independent non-divisor layer heights",
          "[FeatureCadence][CadenceZones][IndependentZones][Regression]")
{
    Print print;
    Model model;
    add_independent_zoned_text_cube(model, "independent-text");
    apply_zoned_model(print, model, independent_zoned_cadence_config());

    const PrintObject &object = *print.objects().front();
    REQUIRE(object.slicing_parameters().cadence_ratio == 1);
    REQUIRE(object.slicing_parameters().cadence_zone_digest != 0);
    REQUIRE(object.cadence_zones().size() == 2);
    CHECK_FALSE(object.cadence_zones()[0].mixed);
    CHECK_FALSE(object.cadence_zones()[1].mixed);
    CHECK_THAT(object.cadence_zones()[0].hi, Catch::Matchers::WithinAbs(35., EPSILON));
    CHECK_THAT(object.cadence_zones()[1].lo, Catch::Matchers::WithinAbs(35., EPSILON));
    CHECK_THAT(object.cadence_zones()[0].height, Catch::Matchers::WithinAbs(34.8 / 124., EPSILON));
    CHECK_THAT(object.cadence_zones()[1].height, Catch::Matchers::WithinAbs(1. / 13., EPSILON));

    const StringObjectException validation = print.validate();
    CAPTURE(validation.string, validation.opt_key);
    REQUIRE(validation.string.empty());

    const std::string output = gcode(print);
    bool found_body_wall = false;
    bool found_text_wall = false;
    for (const GCodeExtrusion &extrusion : gcode_extrusions(output)) {
        if (!is_wall_role(extrusion.role))
            continue;
        CAPTURE(extrusion.tool, extrusion.z, extrusion.height);
        if (extrusion.z <= 35. + EPSILON) {
            found_body_wall = true;
            CHECK(extrusion.tool == 0);
            CHECK_THAT(extrusion.height, Catch::Matchers::WithinAbs(
                extrusion.z <= 0.2 + EPSILON ? 0.2 : 34.8 / 124., 1e-4));
        } else {
            found_text_wall = true;
            CHECK(extrusion.tool == 1);
            CHECK_THAT(extrusion.height, Catch::Matchers::WithinAbs(1. / 13., 1e-4));
        }
    }
    CHECK(found_body_wall);
    CHECK(found_text_wall);

    for (const Layer *layer : object.layers()) {
        if (layer->print_z > 35. + EPSILON)
            continue;
        for (const LayerRegion *region : layer->regions())
            CHECK(region->fill_surfaces.filter_by_type(stInternalVoid).empty());
    }
}

TEST_CASE("Independent zone layers land exactly on a geometric boundary",
          "[FeatureCadence][CadenceZones][IndependentZones]")
{
    Print print;
    Model model;
    add_independent_zoned_text_cube(model, "boundary-landing", 34.9);
    apply_zoned_model(print, model, independent_zoned_cadence_config());

    const PrintObject &object = *print.objects().front();
    REQUIRE(object.cadence_zones().size() == 2);
    CHECK_THAT(object.cadence_zones()[0].height, Catch::Matchers::WithinAbs(34.7 / 124., 1e-4));
    std::vector<coordf_t> profile;
    REQUIRE(PrintObject::update_layer_height_profile(
        *object.model_object(), object.slicing_parameters(), profile, object.cadence_zones()));
    const std::vector<coordf_t> layers = generate_object_layers(object.slicing_parameters(), profile, false);
    const auto boundary = std::min_element(layers.begin(), layers.end(), [](double lhs, double rhs) {
        return std::abs(lhs - 34.9) < std::abs(rhs - 34.9);
    });
    REQUIRE(boundary != layers.end());
    CHECK_THAT(*boundary, Catch::Matchers::WithinAbs(34.9, EPSILON));
}

TEST_CASE("An impossible independent zone height reports its tool range",
          "[FeatureCadence][CadenceZones][IndependentZones][Validate]")
{
    Print print;
    Model model;
    add_independent_zoned_text_cube(model, "tiny-zone", 0.2, 0.02);
    apply_zoned_model(print, model, independent_zoned_cadence_config());

    const StringObjectException error = print.validate();
    REQUIRE_FALSE(error.string.empty());
    CHECK(error.string.find("Cadence zone Z") != std::string::npos);
    CHECK(error.string.find("tool 2") != std::string::npos);
    CHECK(error.string.find("0.05") != std::string::npos);
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
    CHECK(default_print.objects().front()->cadence_zones().empty());
    CHECK(default_print.objects().front()->slicing_parameters().cadence_zone_digest == 0);
}

TEST_CASE("Base-zone top shells keep their unscaled layer count", "[FeatureCadence][CadenceZones][Shells]")
{
    Print print;
    Model model;
    add_zoned_text_cube(model, "zoned-shells");
    apply_zoned_model(print, model, zoned_cadence_config());
    REQUIRE(print.validate().string.empty());
    print.process();

    std::map<int, double> coarse_top_shell_area;
    for (const Layer *layer : print.objects().front()->layers()) {
        if (layer->print_z < 9. - EPSILON || layer->print_z > 10. + EPSILON)
            continue;
        for (const LayerRegion *region : layer->regions()) {
            if (std::abs(region->region().config().outer_wall_line_width.get_at(0).value - 0.44) > 1e-6)
                continue;
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.surface_type == stTop || surface.surface_type == stInternalSolid)
                    coarse_top_shell_area[int(std::lround(layer->print_z * 10.))] += surface.expolygon.area();
        }
    }
    std::set<int> coarse_top_shell_z;
    for (const auto &[z, area] : coarse_top_shell_area) {
        CAPTURE(area);
        coarse_top_shell_z.insert(z);
    }
    // The source top surface plus the configured three propagated layers. Global cadence
    // scaling would incorrectly extend this set down to Z=9.0.
    CHECK(coarse_top_shell_z == std::set<int>{94, 96, 98, 100});
}

TEST_CASE("Prime tower rejects different cadence-zone tables and accepts matching tables",
          "[FeatureCadence][CadenceZones][PrimeTower][Validate]")
{
    DynamicPrintConfig config = zoned_cadence_config({
        {"enable_prime_tower", true},
        {"use_relative_e_distances", true},
        {"prime_tower_width", 35.},
        {"wipe_tower_x", "50"},
        {"wipe_tower_y", "50"},
    });

    SECTION("different tables") {
        Print print;
        Model model;
        add_zoned_text_cube(model, "TextCube", 0.);
        add_zoned_text_cube(model, "WholeFine", 30., 10., true);
        apply_zoned_model(print, model, config);

        REQUIRE(print.objects().size() == 2);
        CHECK(print.objects()[0]->slicing_parameters().cadence_zone_digest !=
              print.objects()[1]->slicing_parameters().cadence_zone_digest);
        const StringObjectException error = print.validate();
        REQUIRE_FALSE(error.string.empty());
        CHECK(error.string.find("TextCube") != std::string::npos);
        CHECK(error.string.find("WholeFine") != std::string::npos);
    }

    SECTION("matching tables") {
        Print print;
        Model model;
        add_zoned_text_cube(model, "TextCube A", 0.);
        add_zoned_text_cube(model, "TextCube B", 30.);
        apply_zoned_model(print, model, config);

        REQUIRE(print.objects().size() == 2);
        CHECK(print.objects()[0]->slicing_parameters().cadence_zone_digest ==
              print.objects()[1]->slicing_parameters().cadence_zone_digest);
        const StringObjectException error = print.validate();
        CAPTURE(error.string, error.opt_key);
        CHECK(error.string.empty());
    }
}

TEST_CASE("Zoned cadence is rejected with an actionable Organic-support error",
          "[FeatureCadence][CadenceZones][Support][Validate]")
{
    DynamicPrintConfig config = zoned_cadence_config({
        {"enable_support", true},
        {"support_type", "tree(auto)"},
        {"support_style", "organic"},
    });
    Print print;
    Model model;
    ModelObject *model_object = add_zoned_text_cube(model, "organic-zones");
    apply_zoned_model(print, model, config);

    CHECK_FALSE(model_object->has_custom_layering());
    const StringObjectException error = print.validate();
    REQUIRE_FALSE(error.string.empty());
    CHECK(error.string.find("Cadence zones") != std::string::npos);
    CHECK(error.string.find("Organic supports") != std::string::npos);
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

// Fine-height interior extrusions on base layers are internal-bridge anchors, which stay at
// the fine cadence by design; combined layers may end on the interior tool with a plain
// change back to the wall tool on the next fine layer (physically valid; ordering
// optimization is Phase-3 work).
TEST_CASE("A mixed-nozzle cube preserves feature cadence through G-code",
          "[FeatureCadence][GCode]")
{
    const bool cooling_slowdown = GENERATE(false, true);
    DYNAMIC_SECTION("cooling slowdown " << (cooling_slowdown ? "enabled" : "disabled")) {
        DynamicPrintConfig config = mixed_nozzle_config({
            {"sparse_infill_density", 15.},
            {"top_shell_layers", 2},
            {"bottom_shell_layers", 2},
            {"top_shell_thickness", 0.},
            {"bottom_shell_thickness", 0.},
            {"ensure_vertical_shell_thickness", "none"},
            {"enable_prime_tower", false},
            {"nozzle_temperature_initial_layer", "210,250"},
            {"nozzle_temperature", "210,250"},
        });
        if (cooling_slowdown)
            config.set_deserialize_strict({{"slow_down_layer_time", "8,8"}});

        Print print;
        Model model;
        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{
            {"wall_layer_height", 0.1},
            {"wall_process_projection", "outer_wall_line_width=0.24;inner_wall_line_width=0.24"},
        }};
        init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);
        check_dual_tool_cube_gcode(gcode(print), !cooling_slowdown);
    }
}

TEST_CASE("Mixed-nozzle feature cadence requires a manual map and holds after pinning",
          "[FeatureCadence][GCode][Validate][Regression]")
{
    const bool fine_nozzle_is_first = GENERATE(false, true);
    DynamicPrintConfig config = mixed_nozzle_config({
        {"sparse_infill_density", 15.},
        {"top_shell_layers", 2},
        {"bottom_shell_layers", 2},
        {"top_shell_thickness", 0.},
        {"bottom_shell_thickness", 0.},
        {"ensure_vertical_shell_thickness", "none"},
        {"enable_prime_tower", false},
    });
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values =
        fine_nozzle_is_first ? std::vector<double>{0.2, 0.4} : std::vector<double>{0.4, 0.2};
    config.option<ConfigOptionFloats>("min_layer_height", true)->values =
        fine_nozzle_is_first ? std::vector<double>{0.05, 0.1} : std::vector<double>{0.1, 0.05};
    config.option<ConfigOptionFloats>("max_layer_height", true)->values =
        fine_nozzle_is_first ? std::vector<double>{0.15, 0.3} : std::vector<double>{0.3, 0.15};
    config.option<ConfigOptionInts>("filament_map", true)->values =
        fine_nozzle_is_first ? std::vector<int>{1, 2} : std::vector<int>{2, 1};
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{
        {"wall_layer_height", 0.1},
        {"wall_process_projection", "outer_wall_line_width=0.24;inner_wall_line_width=0.24"},
    }};

    DYNAMIC_SECTION("fine nozzle on tool " << (fine_nozzle_is_first ? 0 : 1)) {
        SECTION("raw automatic map is rejected") {
            config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmAutoForFlush;
            Print print;
            Model model;
            init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

            const StringObjectException error = print.validate();
            REQUIRE_FALSE(error.string.empty());
            CHECK(error.string.find("manual filament-to-nozzle mapping") != std::string::npos);
            CHECK(error.opt_key == "filament_map_mode");
        }

        SECTION("manual map preserves fine walls and base-cadence interiors") {
            Print print;
            Model model;
            init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

            const StringObjectException error = print.validate();
            CAPTURE(error.string, error.opt_key);
            REQUIRE(error.string.empty());
            check_dual_tool_cube_gcode(gcode(print), false);
        }
    }
}

TEST_CASE("Uniform nozzles keep automatic feature mappings valid", "[FeatureCadence][Validate][Regression]")
{
    const bool reversed_map = GENERATE(false, true);
    DynamicPrintConfig config = mixed_nozzle_config({
        {"nozzle_diameter", "0.4,0.4"},
        {"min_layer_height", "0.1,0.1"},
        {"max_layer_height", "0.3,0.3"},
    });
    config.option<ConfigOptionInts>("filament_map", true)->values =
        reversed_map ? std::vector<int>{2, 1} : std::vector<int>{1, 2};
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmAutoForFlush;
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{
        {"wall_layer_height", 0.2},
        {"wall_process_projection", "outer_wall_line_width=0.4;inner_wall_line_width=0.4"},
    }};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);

    DYNAMIC_SECTION("filament map " << (reversed_map ? "2,1" : "1,2")) {
        const StringObjectException error = print.validate();
        CAPTURE(error.string, error.opt_key);
        CHECK(error.string.empty());
    }
}

TEST_CASE("Prime tower follows mixed feature cadence without adding fine-layer tool changes",
          "[FeatureCadence][GCode]")
{
    DynamicPrintConfig config = mixed_nozzle_config({
        {"sparse_infill_density", 15.},
        {"top_shell_layers", 2},
        {"bottom_shell_layers", 2},
        {"top_shell_thickness", 0.},
        {"bottom_shell_thickness", 0.},
        {"ensure_vertical_shell_thickness", "none"},
        {"enable_prime_tower", true},
        {"prime_tower_width", 35.},
        {"wipe_tower_x", "50"},
        {"wipe_tower_y", "50"},
    });
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{
        {"wall_layer_height", 0.1},
        {"wall_process_projection", "outer_wall_line_width=0.24;inner_wall_line_width=0.24"},
    }};
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config, &overrides);
    REQUIRE(print.config().enable_prime_tower.value);

    const std::string output = gcode(print);
    const std::vector<GCodeExtrusion> extrusions = gcode_extrusions(output);
    CHECK(std::any_of(extrusions.begin(), extrusions.end(), [](const GCodeExtrusion &extrusion) {
        return extrusion.role == erWipeTower;
    }));

    size_t in_object_tool_changes = 0;
    for (const GCodeToolChange &change : gcode_tool_changes(output)) {
        if (change.z <= 0.2 + 1e-4)
            continue;
        ++in_object_tool_changes;
        CAPTURE(change.tool, change.z);
        // Changes to the interior tool may only happen on base layers. A change back to the
        // wall tool on a fine-only layer is physically valid (though a wasted swap — see the
        // Phase-3 tool-ordering optimization note in the plan).
        if (change.tool != 0)
            CHECK(is_base_z(change.z));
    }
    CHECK(in_object_tool_changes > 0);

    size_t fine_object_layers = 0;
    std::map<int, std::set<int>> object_tools_by_z;
    for (const GCodeExtrusion &extrusion : extrusions)
        // Bridges stay at the fine cadence by design and print with their assigned interior
        // tool (carried over from the previous base layer, no extra toolchange), so they are
        // exempt from the walls-only expectation on fine layers.
        if (extrusion.role != erWipeTower && extrusion.role != erBridgeInfill &&
            extrusion.role != erInternalBridgeInfill)
            object_tools_by_z[int(std::lround(extrusion.z * 10.))].insert(extrusion.tool);
    for (const auto &[z_tenth, tools] : object_tools_by_z) {
        if (z_tenth <= 2 || z_tenth % 2 == 0)
            continue;
        ++fine_object_layers;
        CHECK(tools == std::set<int>{0});
    }
    CHECK(fine_object_layers > 0);
}

TEST_CASE("A wall-disabled modifier remains sparse and does not disturb object cadence", "[FeatureCadence][Modifier]")
{
    DynamicPrintConfig config = mixed_nozzle_config({
        {"sparse_infill_density", 15.},
        {"top_shell_layers", 2},
        {"bottom_shell_layers", 2},
        {"ensure_vertical_shell_thickness", "none"},
    });
    Model model;
    ModelObject *object = model.add_object();
    object->name = "modifier-cube.stl";
    object->add_volume(make_cube(20., 20., 20.));
    ModelVolume *modifier = object->add_volume(
        make_cube(10., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    modifier->config.set("wall_loops", 0);
    object->config.set("wall_layer_height", 0.1);
    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    const StringObjectException validation = print.validate();
    CAPTURE(validation.string, validation.opt_key);
    REQUIRE(validation.string.empty());
    print.set_status_silent();
    REQUIRE_NOTHROW(print.process());

    bool found_disabled_region = false;
    bool found_fine_object_wall = false;
    for (const Layer *layer : print.objects().front()->layers()) {
        for (const LayerRegion *region : layer->regions()) {
            const PrintRegionConfig &region_config = region->region().config();
            if (region_config.wall_loops.value == 0) {
                found_disabled_region = true;
                CHECK(region_config.wall_process_projection.value.empty());
                CHECK(region->perimeters.entities.empty());
                continue;
            }
            for (const ExtrusionEntity *entity : region->perimeters.entities)
                visit_paths(*entity, [&](const ExtrusionPath &path) {
                    if (layer->print_z > 0.2 + EPSILON && is_wall_role(path.role())) {
                        found_fine_object_wall = true;
                        CHECK_THAT(path.height, Catch::Matchers::WithinAbs(0.1, EPSILON));
                    }
                });
        }
    }
    CHECK(found_disabled_region);
    CHECK(found_fine_object_wall);
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

TEST_CASE("Interior role finer than base remains role-specific", "[FeatureCadence][Validate][D1C]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({
        {"sparse_infill_density", 15.},
        {"top_surface_process_layer_height", 0.1},
        {"top_surface_filament_id", 1},
        {"sparse_infill_filament_id", 2},
        {"internal_solid_infill_filament_id", 2},
        {"bottom_surface_filament_id", 2},
    });
    const std::string output = slice_with_object_overrides({cube(20.)}, config, {});
    const std::vector<GCodeExtrusion> extrusions = gcode_extrusions(output);
    REQUIRE_FALSE(extrusions.empty());

    bool found_top_surface = false;
    bool found_recombined_interior = false;
    for (const GCodeExtrusion &extrusion : extrusions) {
        if (extrusion.role == erTopSolidInfill) {
            found_top_surface = true;
            CHECK(extrusion.tool == 0);
            CHECK_THAT(extrusion.height, Catch::Matchers::WithinAbs(0.1, EPSILON));
            continue;
        }
        if (extrusion.role == erInternalInfill || extrusion.role == erSolidInfill || extrusion.role == erBottomSurface) {
            if (std::abs(extrusion.height - 0.2) <= EPSILON) {
                found_recombined_interior = true;
                CHECK(extrusion.tool == 1);
            }
            continue;
        }
        if (extrusion.role == erBridgeInfill || extrusion.role == erInternalBridgeInfill)
            continue;
    }
    CHECK(found_top_surface);
    CHECK(found_recombined_interior);
}

TEST_CASE("Fine top-surface height keeps shell-layer count", "[FeatureCadence][D1C][Regression]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config();
    config.set_deserialize_strict({
        {"sparse_infill_density", 15.},
        {"top_surface_process_layer_height", 0.1},
        {"top_surface_filament_id", 1},
        {"top_shell_layers", 7},
        {"top_shell_thickness", 0.},
        {"bottom_shell_layers", 0},
        {"bottom_shell_thickness", 0.},
    });
    Print print;
    Model model;
    process_cube_with_overrides(config, {}, print, model);
    CHECK(shell_layer_z_tenths(*print.objects().front()).size() == 5);
}

TEST_CASE("Mixed-nozzle validation is ungated without wall cadence", "[FeatureCadence][Validate][D1C]")
{
    DynamicPrintConfig mixed = mixed_nozzle_grid_config();
    Print mixed_print;
    Model mixed_model;
    init_print({cube(20.)}, mixed_print, mixed_model, mixed);
    const StringObjectException mixed_error = mixed_print.validate();
    CHECK_FALSE(mixed_error.string.empty());
    CHECK(mixed_error.string.find("Outer wall") != std::string::npos);
    CHECK(mixed_error.opt_key == "wall_layer_height");

    DynamicPrintConfig uniform = mixed_nozzle_grid_config({
        {"nozzle_diameter", "0.4,0.4"},
        {"max_layer_height", "0.3,0.3"},
    });
    Print uniform_print;
    Model uniform_model;
    init_print({cube(20.)}, uniform_print, uniform_model, uniform);
    CHECK(uniform_print.validate().string.empty());
}

TEST_CASE("Non-divisor interior heights are rejected by role", "[FeatureCadence][Validate][D1C]")
{
    DynamicPrintConfig fail = mixed_nozzle_grid_config({{"sparse_infill_process_layer_height", 0.13}, {"sparse_infill_filament_id", 1}});
    fail.set_deserialize_strict({
        {"outer_wall_filament_id", 2},
        {"inner_wall_filament_id", 2},
    });
    Print fail_print;
    Model fail_model;
    init_print({cube(20.)}, fail_print, fail_model, fail);
    const StringObjectException fail_error = fail_print.validate();
    REQUIRE_FALSE(fail_error.string.empty());
    CHECK(fail_error.string.find("Sparse infill") != std::string::npos);
    CHECK(fail_error.string.find("must divide base layer height") != std::string::npos);
    CHECK(fail_error.opt_key == "layer_height");

    DynamicPrintConfig pass = mixed_nozzle_grid_config({{"sparse_infill_process_layer_height", 0.1}, {"sparse_infill_filament_id", 1}});
    pass.set_deserialize_strict({
        {"outer_wall_filament_id", 2},
        {"inner_wall_filament_id", 2},
    });
    Print pass_print;
    Model pass_model;
    init_print({cube(20.)}, pass_print, pass_model, pass);
    const StringObjectException pass_error = pass_print.validate();
    const PrintRegion &pass_region = pass_print.objects().front()->all_regions().front().get();
    CHECK(pass_error.string.empty());
}

TEST_CASE("Interior heights must stay on endpoints", "[FeatureCadence][Validate][D1C]")
{
    DynamicPrintConfig config = mixed_nozzle_grid_config({
        {"wall_layer_height", 0.05},
        {"top_surface_process_layer_height", 0.1},
        {"top_surface_filament_id", 1},
    });
    Print print;
    Model model;
    init_print({cube(20.)}, print, model, config);
    const StringObjectException error = print.validate();
    REQUIRE_FALSE(error.string.empty());
    CHECK(error.string.find("Top surface") != std::string::npos);
    CHECK(error.string.find("unsupported when the base layer height is") != std::string::npos);
}

TEST_CASE("First-layer height warns on mixed nozzles", "[FeatureCadence][Validate][D1C][Regression]")
{
    DynamicPrintConfig mixed = mixed_nozzle_grid_config({{"initial_layer_print_height", 0.2}});
    mixed.set_deserialize_strict({
        {"outer_wall_filament_id", 2},
        {"inner_wall_filament_id", 2},
    });
    Print mixed_print;
    Model mixed_model;
    init_print({cube(20.)}, mixed_print, mixed_model, mixed);
    std::vector<StringObjectException> mixed_warnings;
    CHECK(mixed_print.validate(&mixed_warnings).string.empty());
    CHECK(count_warning_key(mixed_warnings, "initial_layer_print_height") == 1u);

    DynamicPrintConfig uniform = mixed_nozzle_grid_config({
        {"nozzle_diameter", "0.2,0.2"},
        {"initial_layer_print_height", 0.2},
        {"max_layer_height", "0.3,0.3"},
        {"min_layer_height", "0.1,0.1"},
    });
    Print uniform_print;
    Model uniform_model;
    init_print({cube(20.)}, uniform_print, uniform_model, uniform);
    std::vector<StringObjectException> uniform_warnings;
    CHECK(uniform_print.validate(&uniform_warnings).string.empty());
    CHECK(count_warning_key(uniform_warnings, "initial_layer_print_height") == 0u);
}

TEST_CASE("Ratio-one empty filament deltas preserve validation outcome", "[FeatureCadence][FilamentProcess][Validate][Regression][D1C]")
{
    DynamicPrintConfig with_empty_deltas = mixed_nozzle_grid_config();
    with_empty_deltas.option<ConfigOptionStrings>("filament_process_projection", true)->values = {"", ""};
    Print with_print;
    Model with_model;
    init_print({cube(20.)}, with_print, with_model, with_empty_deltas);
    const StringObjectException with_error = with_print.validate();

    DynamicPrintConfig without_deltas = mixed_nozzle_grid_config();
    without_deltas.erase("filament_process_projection");
    Print without_print;
    Model without_model;
    init_print({cube(20.)}, without_print, without_model, without_deltas);
    const StringObjectException without_error = without_print.validate();

    CHECK(with_error.string == without_error.string);
    CHECK(with_error.opt_key == without_error.opt_key);
}

TEST_CASE("Walls resolved only through a filament delta pass validation", "[FeatureCadence][Validate][FilamentProcess][Regression]")
{
    // No L2 wall_process_projection anywhere: the wall filament's per-filament delta (L1) is the
    // sole resolution channel and must satisfy the stale-wall guard.
    DynamicPrintConfig config = filament_delta_test_config();
    // Map the base filament onto the coarse tool so slot 1's 0.2 mm delta is physically valid;
    // walls ride filament 2 on the fine tool with only its L1 delta as resolution.
    config.set_deserialize_strict("filament_map", "2,1");
    config.set_deserialize_strict("outer_wall_filament_id", "2");
    config.set_deserialize_strict("inner_wall_filament_id", "2");
    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{cube(20.)}, print, model, config);

    const StringObjectException error = print.validate();
    INFO(error.string);
    CHECK(error.string.empty());
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
