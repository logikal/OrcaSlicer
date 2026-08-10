#include "FeatureProcessResolver.hpp"

#include "Model.hpp"
#include "PlaceholderParser.hpp"
#include "PresetBundle.hpp"
#include "Slicing.hpp"
#include "format.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iomanip>
#include <limits>
#include <regex>
#include <set>
#include <sstream>

namespace Slic3r {

namespace {

constexpr double NOZZLE_EPSILON = 1e-6;
constexpr double DIVISOR_EPSILON = 0.0005;

struct FeatureProjectionDescriptor {
    FeatureRole role;
    const char *policy_key;
    const char *preset_key;
    const char *height_key;
    const char *projection_key;
    const char *filament_key;
};

const std::array<FeatureProjectionDescriptor, 7> feature_projection_descriptors{{
    {FeatureRole::Wall,             "wall_process_policy",              "wall_process_preset",              "wall_layer_height",              "wall_process_projection",              "outer_wall_filament_id"},
    {FeatureRole::SparseInfill,     "sparse_infill_process_policy",     "sparse_infill_process_preset",     "sparse_infill_process_layer_height",     "sparse_infill_process_projection",     "sparse_infill_filament_id"},
    {FeatureRole::InternalSolid,    "internal_solid_process_policy",    "internal_solid_process_preset",    "internal_solid_process_layer_height",    "internal_solid_process_projection",    "internal_solid_filament_id"},
    {FeatureRole::TopSurface,       "top_surface_process_policy",       "top_surface_process_preset",       "top_surface_process_layer_height",       "top_surface_process_projection",       "top_surface_filament_id"},
    {FeatureRole::BottomSurface,    "bottom_surface_process_policy",    "bottom_surface_process_preset",    "bottom_surface_process_layer_height",    "bottom_surface_process_projection",    "bottom_surface_filament_id"},
    {FeatureRole::Support,          "support_process_policy",           "support_process_preset",           "support_process_layer_height",           "support_process_projection",           "support_filament"},
    {FeatureRole::SupportInterface, "support_interface_process_policy", "support_interface_process_preset", "support_interface_process_layer_height", "support_interface_process_projection", "support_interface_filament"},
}};

const ConfigOption *inherited_option(const Preset &preset, const PresetCollection &collection, const std::string &key)
{
    const Preset          *current = &preset;
    std::set<const Preset *> visited;
    while (current != nullptr && visited.insert(current).second) {
        if (const ConfigOption *option = current->config.option(key); option != nullptr)
            return option;
        current = collection.get_preset_parent(*current);
    }
    return nullptr;
}

const ConfigOptionString *string_option(const Preset &preset, const PresetCollection &collection, const std::string &key)
{
    return dynamic_cast<const ConfigOptionString *>(inherited_option(preset, collection, key));
}

DynamicPrintConfig materialized_preset_config(const Preset &preset, const PresetCollection &collection)
{
    std::vector<const Preset *> chain;
    const Preset               *current = &preset;
    std::set<const Preset *>    visited;
    while (current != nullptr && visited.insert(current).second) {
        chain.push_back(current);
        current = collection.get_preset_parent(*current);
    }

    DynamicPrintConfig result;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        result.apply((*it)->config);
    return result;
}

double preset_layer_height(const Preset &preset, const PresetCollection &prints)
{
    const auto *option = dynamic_cast<const ConfigOptionFloat *>(inherited_option(preset, prints, "layer_height"));
    return option == nullptr ? 0. : option->value;
}

DynamicPrintConfig materialized_printer_config(const DynamicPrintConfig *config)
{
    DynamicPrintConfig result = DynamicPrintConfig::full_print_config();
    if (config != nullptr)
        result.apply(*config);
    return result;
}

const DynamicPrintConfig *request_printer_config(const FeatureProcessRequest &request)
{
    if (request.printer_config != nullptr)
        return request.printer_config;
    if (request.bundle != nullptr)
        return &request.bundle->printers.get_selected_preset().config;
    return nullptr;
}

GCodeConfig gcode_config_from(const DynamicPrintConfig *config)
{
    GCodeConfig result;
    if (config != nullptr)
        result.apply(*config, true);
    return result;
}

double option_float_or_default(const DynamicPrintConfig *config, const std::string &key)
{
    if (config != nullptr) {
        if (const auto *option = config->option<ConfigOptionFloat>(key); option != nullptr)
            return option->value;
    }
    const ConfigOptionDef *definition = print_config_def.get(key);
    if (definition != nullptr) {
        if (const auto *option = dynamic_cast<const ConfigOptionFloat *>(definition->default_value.get()); option != nullptr)
            return option->value;
    }
    return 0.;
}

double request_base_layer_height(const FeatureProcessRequest &request)
{
    if (request.effective_object_config != nullptr) {
        if (const auto *option = request.effective_object_config->option<ConfigOptionFloat>("layer_height"); option != nullptr)
            return option->value;
    }
    return option_float_or_default(request_printer_config(request), "layer_height");
}

int option_int_or_default(const DynamicPrintConfig *config, const std::string &key)
{
    if (config != nullptr) {
        if (const auto *option = config->option<ConfigOptionInt>(key); option != nullptr)
            return option->value;
    }
    const ConfigOptionDef *definition = print_config_def.get(key);
    if (definition != nullptr) {
        if (const auto *option = dynamic_cast<const ConfigOptionInt *>(definition->default_value.get()); option != nullptr)
            return option->value;
    }
    return 0;
}

std::string option_string(const DynamicPrintConfig &config, const std::string &key)
{
    const auto *option = config.option<ConfigOptionString>(key);
    return option == nullptr ? std::string() : option->value;
}

std::string format_number(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << value;
    std::string result = stream.str();
    while (!result.empty() && result.back() == '0')
        result.pop_back();
    if (!result.empty() && result.back() == '.')
        result.pop_back();
    return result.empty() ? "0" : result;
}

std::string serialize_projection(const DynamicPrintConfig &projection)
{
    std::vector<std::string> keys = projection.keys();
    std::sort(keys.begin(), keys.end());
    std::ostringstream stream;
    for (const std::string &key : keys) {
        if (stream.tellp() > 0)
            stream << ';';
        stream << key << '=' << projection.opt_serialize(key);
    }
    return stream.str();
}

bool starts_with(const std::string &value, const char *prefix)
{
    return value.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

bool ends_with(const std::string &value, const char *suffix)
{
    const size_t length = std::char_traits<char>::length(suffix);
    return value.size() >= length && value.compare(value.size() - length, length, suffix) == 0;
}

const std::set<std::string> &filament_delta_excluded_keys()
{
    // Plate-global settings and L2 feature-process controls never ride the L1 filament delta.
    static const std::set<std::string> keys = [] {
        std::set<std::string> result{
            "print_extruder_id", "print_extruder_variant", "wall_layer_height", "farthest_point_timelapse",
            "outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
            "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id",
        };
        for (const std::string &key : print_config_def.keys()) {
            if (starts_with(key, "initial_layer_") || starts_with(key, "prime_tower_") ||
                starts_with(key, "wipe_tower_") || starts_with(key, "skirt_") || starts_with(key, "brim_") ||
                starts_with(key, "travel_") || starts_with(key, "raft_") || starts_with(key, "timelapse_") ||
                ends_with(key, "_process_policy") || ends_with(key, "_process_preset") ||
                ends_with(key, "_process_projection") || ends_with(key, "_process_layer_height") ||
                ends_with(key, "_gcode"))
                result.insert(key);
        }
        return result;
    }();
    return keys;
}

bool filament_delta_key_allowed(const std::string &key)
{
    static const std::set<std::string> region_keys(PrintRegionConfig::defaults().keys_ref().begin(),
                                                    PrintRegionConfig::defaults().keys_ref().end());
    static const std::set<std::string> object_keys{
        "layer_height", "support_line_width", "support_speed", "support_interface_speed",
        "support_top_z_distance", "support_bottom_z_distance",
    };
    return filament_delta_excluded_keys().count(key) == 0 &&
           (region_keys.count(key) != 0 || object_keys.count(key) != 0);
}

bool set_model_config_string(ModelConfig &config, const char *key, const std::string &value)
{
    const auto *current = dynamic_cast<const ConfigOptionString *>(config.option(key));
    if (current != nullptr && current->value == value)
        return false;
    config.set_key_value(key, new ConfigOptionString(value));
    return true;
}

bool set_model_config_float(ModelConfig &config, const char *key, double value)
{
    const ConfigOptionFloat replacement(value);
    const auto *current = dynamic_cast<const ConfigOptionFloat *>(config.option(key));
    if (current != nullptr && *current == replacement)
        return false;
    config.set_key_value(key, replacement.clone());
    return true;
}

FeatureProcessPolicy feature_policy(const DynamicPrintConfig &config, const char *key)
{
    const auto *option = config.option<ConfigOptionEnum<FeatureProcessPolicy>>(key);
    return option == nullptr ? FeatureProcessPolicy::AutoNozzleVariant : option->value;
}

int effective_filament(const DynamicPrintConfig &config, const char *key)
{
    const auto *feature = config.option<ConfigOptionInt>(key);
    if (feature != nullptr && feature->value > 0)
        return feature->value;
    const auto *object = config.option<ConfigOptionInt>("extruder");
    // In model configs, zero means the object's default filament, which is filament 1.
    // Resolve that filament through filament_map instead of assuming logical tool zero.
    return object == nullptr || object->value <= 0 ? 1 : object->value;
}

bool feature_tool_differs_from_object(const DynamicPrintConfig &config, int feature_filament)
{
    const GCodeConfig gcode_config = gcode_config_from(&config);
    const auto *diameters = config.option<ConfigOptionFloats>("nozzle_diameter");
    if (diameters == nullptr || diameters->values.empty())
        return false;
    const int object_filament = effective_filament(config, "extruder");
    const size_t feature_tool = get_extruder_index_from_filament_id(gcode_config, std::max(feature_filament, 0));
    const size_t object_tool = get_extruder_index_from_filament_id(gcode_config, std::max(object_filament, 0));
    return std::abs(diameters->get_at(feature_tool) - diameters->get_at(object_tool)) > NOZZLE_EPSILON;
}

// A volume scope only needs its own resolution when its sparse config actually contributes a
// feature-relevant key; otherwise it inherits the object's values and writing copies here would
// materialize overrides into every part's config.
bool volume_scope_affects_feature(const ModelConfig &config, const FeatureProjectionDescriptor &descriptor)
{
    const DynamicPrintConfig &c = config.get();
    return c.has(descriptor.policy_key) || c.has(descriptor.preset_key) || c.has(descriptor.height_key) ||
           c.has(descriptor.filament_key) || c.has("extruder") ||
           (descriptor.role == FeatureRole::Wall && c.has("wall_loops"));
}

bool update_scope_projection(ModelConfig &scope, const DynamicPrintConfig &effective,
                             const FeatureProjectionDescriptor &descriptor, const PresetBundle &bundle,
                             const DynamicPrintConfig &full_config)
{
    if (feature_projection_keys(descriptor.role).empty())
        return false;

    // Phase 1 treats wall_loops as the wall-activity test. Brim-only walls are handled
    // with the broader feature-presence rules when cadence generation lands in P1.4.
    if (descriptor.role == FeatureRole::Wall) {
        const auto *wall_loops = effective.option<ConfigOptionInt>("wall_loops");
        if (wall_loops != nullptr && wall_loops->value <= 0)
            return scope.erase(descriptor.projection_key);
    }

    const FeatureProcessPolicy policy = feature_policy(effective, descriptor.policy_key);
    const std::string pinned_preset = option_string(effective, descriptor.preset_key);
    const double requested_height = option_float_or_default(&effective, descriptor.height_key);
    const int filament = effective_filament(effective, descriptor.filament_key);
    if (policy == FeatureProcessPolicy::AutoNozzleVariant && pinned_preset.empty() && requested_height <= 0. &&
        !feature_tool_differs_from_object(effective, filament))
        return scope.erase(descriptor.projection_key);

    FeatureProcessRequest request;
    request.role = descriptor.role;
    request.policy = policy;
    request.pinned_preset_name = pinned_preset;
    request.feature_filament = filament;
    request.requested_layer_height = requested_height;
    request.effective_object_config = &effective;
    request.object_process_preset = option_string(full_config, "print_settings_id");
    if (request.object_process_preset.empty())
        request.object_process_preset = bundle.prints.get_selected_preset_name();
    request.bundle = &bundle;
    request.printer_config = &full_config;

    const FeatureProcessResolution resolution = resolve_feature_process(request);
    if (!resolution.ok)
        return scope.erase(descriptor.projection_key);

    bool changed = set_model_config_float(scope, descriptor.height_key, resolution.feature_layer_height);
    const std::string serialized = serialize_projection(resolution.projection);
    if (serialized.empty())
        changed = scope.erase(descriptor.projection_key) || changed;
    else
        changed = set_model_config_string(scope, descriptor.projection_key, serialized) || changed;
    return changed;
}

std::pair<double, double> tool_layer_height_range(const DynamicPrintConfig &printer_config, unsigned int tool_id)
{
    // Precondition: printer_config is materialized (min/max_layer_height and nozzle_diameter present).
    const int nozzle_index = int(tool_id) + 1;
    return {Slicing::min_layer_height_from_nozzle(printer_config, nozzle_index),
            Slicing::max_layer_height_from_nozzle(printer_config, nozzle_index)};
}

unsigned int tool_for_nozzle(const DynamicPrintConfig &printer_config, double nozzle)
{
    const auto *diameters = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    if (diameters != nullptr) {
        for (size_t index = 0; index < diameters->values.size(); ++index)
            if (std::abs(diameters->values[index] - nozzle) <= NOZZLE_EPSILON)
                return unsigned(index);
    }
    return 0;
}

const Preset *compatibility_source(const Preset &preset, const PresetCollection &prints)
{
    const auto *printers = dynamic_cast<const ConfigOptionStrings *>(preset.config.option("compatible_printers"));
    const auto *condition = dynamic_cast<const ConfigOptionString *>(preset.config.option("compatible_printers_condition"));
    if ((printers != nullptr && !printers->values.empty()) || (condition != nullptr && !condition->value.empty()))
        return &preset;
    if (const Preset *base = prints.get_preset_base(preset); base != nullptr)
        return base;
    return &preset;
}

std::string preset_printer_model(const Preset &printer, const PresetCollection &printers)
{
    const auto *model = dynamic_cast<const ConfigOptionString *>(inherited_option(printer, printers, "printer_model"));
    return model == nullptr ? std::string() : model->value;
}

double preset_nozzle_diameter(const Preset &printer, const PresetCollection &printers)
{
    const auto *diameters = dynamic_cast<const ConfigOptionFloats *>(inherited_option(printer, printers, "nozzle_diameter"));
    return diameters == nullptr || diameters->values.empty() ? 0. : diameters->get_at(0);
}

FeatureProcessRejection preset_compatible_with_tool_impl(const Preset             &process_preset,
                                                         const PresetBundle       &bundle,
                                                         double                    tool_nozzle_diameter,
                                                         const DynamicPrintConfig &printer_config,
                                                         unsigned int              tool_id)
{
    // printer_config must be materialized; it is shared across candidate evaluations, so it is
    // only copied in the condition branch that has to overwrite nozzle_diameter[0].
    const Preset &active_printer = bundle.printers.get_selected_preset();
    const Preset *source = compatibility_source(process_preset, bundle.prints);

    const auto *compatible_printers = dynamic_cast<const ConfigOptionStrings *>(source->config.option("compatible_printers"));
    if (compatible_printers != nullptr && !compatible_printers->values.empty()) {
        std::string active_model = preset_printer_model(active_printer, bundle.printers);
        if (active_model.empty())
            active_model = option_string(printer_config, "printer_model");

        bool model_matches = false;
        bool nozzle_matches = false;
        for (const std::string &printer_name : compatible_printers->values) {
            const Preset *printer = bundle.printers.find_preset(printer_name, false);
            if (printer == nullptr || preset_printer_model(*printer, bundle.printers) != active_model)
                continue;
            model_matches = true;
            if (std::abs(preset_nozzle_diameter(*printer, bundle.printers) - tool_nozzle_diameter) <= NOZZLE_EPSILON) {
                nozzle_matches = true;
                break;
            }
        }
        if (!model_matches)
            return FeatureProcessRejection::PrinterModelMismatch;
        if (!nozzle_matches)
            return FeatureProcessRejection::NozzleMismatch;
    } else {
        const auto *condition = dynamic_cast<const ConfigOptionString *>(source->config.option("compatible_printers_condition"));
        if (condition != nullptr && !condition->value.empty()) {
            DynamicPrintConfig active_config = printer_config;
            auto *diameters = active_config.option<ConfigOptionFloats>("nozzle_diameter", true);
            if (diameters->values.empty())
                diameters->values.push_back(tool_nozzle_diameter);
            else
                diameters->values[0] = tool_nozzle_diameter;

            DynamicPrintConfig extras;
            extras.set_key_value("printer_preset", new ConfigOptionString(active_printer.name));
            extras.set_key_value("num_extruders", new ConfigOptionInt(int(diameters->values.size())));
            try {
                if (!PlaceholderParser::evaluate_boolean_expression(condition->value, active_config, &extras))
                    return FeatureProcessRejection::NozzleMismatch;
            } catch (...) {
                // Existing preset compatibility treats malformed conditions as compatible.
            }
        }
    }

    const double height = preset_layer_height(process_preset, bundle.prints);
    const auto   range = tool_layer_height_range(printer_config, tool_id);
    if (height < range.first - NOZZLE_EPSILON || height > range.second + NOZZLE_EPSILON)
        return FeatureProcessRejection::LayerHeightOutOfToolRange;
    return FeatureProcessRejection::None;
}

double process_target_nozzle(const FeatureProcessRequest &request, const GCodeConfig &gcode_config,
                             const DynamicPrintConfig &printer_config)
{
    const int          object_filament = option_int_or_default(request.effective_object_config, "extruder");
    const unsigned int filament = object_filament > 0 ? unsigned(object_filament) : 1;
    const size_t       tool_id = get_extruder_index_from_filament_id(gcode_config, filament);
    return printer_config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(tool_id);
}

std::string process_family(const Preset &preset, const PresetCollection &prints)
{
    if (const auto *family = string_option(preset, prints, "process_family"); family != nullptr && !family->value.empty())
        return family->value;

    const Preset *base = prints.get_preset_base(preset);
    if (base == nullptr || !base->is_system)
        return {};
    static const std::regex family_pattern(R"(^[0-9]+(?:\.[0-9]+)?\s*mm\s+(.+?)\s+@.+$)");
    std::smatch match;
    return std::regex_match(base->name, match, family_pattern) ? match[1].str() : std::string();
}

double inferred_preset_nozzle(const Preset &preset, const PresetBundle &bundle, double fallback)
{
    const Preset *source = compatibility_source(preset, bundle.prints);
    const auto *compatible_printers = dynamic_cast<const ConfigOptionStrings *>(source->config.option("compatible_printers"));
    if (compatible_printers == nullptr)
        return fallback;

    const Preset &active_printer = bundle.printers.get_selected_preset();
    const std::string active_model = preset_printer_model(active_printer, bundle.printers);
    for (const std::string &printer_name : compatible_printers->values) {
        const Preset *printer = bundle.printers.find_preset(printer_name, false);
        if (printer != nullptr && preset_printer_model(*printer, bundle.printers) == active_model)
            return preset_nozzle_diameter(*printer, bundle.printers);
    }
    return fallback;
}

void set_cadence_values(FeatureProcessResolution &resolution, double feature_layer_height)
{
    resolution.feature_layer_height = feature_layer_height;
    resolution.cadence_ratio = feature_cadence_ratio(resolution.base_layer_height, feature_layer_height);
    resolution.grid_height = resolution.cadence_ratio > 0 ? resolution.base_layer_height / resolution.cadence_ratio : 0.;
}

void set_divisor_rejection(FeatureProcessResolution &resolution, const DynamicPrintConfig &printer_config)
{
    resolution.ok = false;
    resolution.rejection = FeatureProcessRejection::LayerHeightNotDivisor;
    resolution.rejection_args = {format_number(resolution.feature_layer_height), format_number(resolution.base_layer_height)};
    const auto range = tool_layer_height_range(printer_config, resolution.tool_id);
    for (int ratio = 1; ratio <= 5 && resolution.rejection_args.size() < 5; ++ratio) {
        const double height = resolution.base_layer_height / ratio;
        if (height >= range.first - NOZZLE_EPSILON && height <= range.second + NOZZLE_EPSILON)
            resolution.rejection_args.push_back(format_number(height));
    }
}

void append_cadence_label(FeatureProcessResolution &resolution)
{
    if (resolution.cadence_ratio > 1)
        resolution.display_label = format("%1% — %2% fine layers per %3% mm layer", resolution.display_label,
                                          resolution.cadence_ratio, format_number(resolution.base_layer_height));
}

void build_projection(FeatureProcessResolution &resolution, FeatureRole role, const Preset &preset, const PresetCollection &prints)
{
    for (const std::string &key : feature_projection_keys(role)) {
        const ConfigOption *source = inherited_option(preset, prints, key);
        if (source == nullptr)
            continue;
        ConfigOption *copy = source->clone();
        if (auto *vector = dynamic_cast<ConfigOptionVectorBase *>(copy); vector != nullptr) {
            if (vector->empty()) {
                delete copy;
                continue;
            }
            vector->resize(1);
        }
        resolution.projection.set_key_value(key, copy);
    }
}

void build_filament_delta(FilamentProcessResolution &resolution, const Preset &preset, const PresetCollection &prints)
{
    const DynamicPrintConfig materialized = materialized_preset_config(preset, prints);
    for (const std::string &key : materialized.keys()) {
        if (!filament_delta_key_allowed(key))
            continue;
        ConfigOption *copy = materialized.option(key)->clone();
        if (auto *vector = dynamic_cast<ConfigOptionVectorBase *>(copy); vector != nullptr) {
            if (vector->empty()) {
                delete copy;
                continue;
            }
            vector->resize(1);
        }
        resolution.delta.set_key_value(key, copy);
    }
}

FeatureProcessResolution resolve_feature_process_impl(const FeatureProcessRequest &request)
{
    FeatureProcessResolution resolution;
    const DynamicPrintConfig *source_printer = request_printer_config(request);
    const DynamicPrintConfig  printer_config = materialized_printer_config(source_printer);
    const GCodeConfig         gcode_config = gcode_config_from(&printer_config);

    const unsigned int feature_filament = request.feature_filament > 0 ? unsigned(request.feature_filament) : 0;
    resolution.tool_id = unsigned(get_extruder_index_from_filament_id(gcode_config, feature_filament));
    resolution.nozzle_diameter = printer_config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(resolution.tool_id);
    resolution.base_layer_height = request_base_layer_height(request);

    if (request.bundle == nullptr) {
        if (request.requested_layer_height > 0.)
            set_cadence_values(resolution, request.requested_layer_height);
        resolution.rejection = FeatureProcessRejection::NoBundle;
        return resolution;
    }

    const double object_nozzle = process_target_nozzle(request, gcode_config, printer_config);
    if (request.policy == FeatureProcessPolicy::SameAsObject ||
        (request.policy == FeatureProcessPolicy::AutoNozzleVariant &&
         std::abs(resolution.nozzle_diameter - object_nozzle) <= NOZZLE_EPSILON)) {
        resolution.resolved_preset.clear();
        set_cadence_values(resolution, request.requested_layer_height > 0. ? request.requested_layer_height : resolution.base_layer_height);
        resolution.display_label = request.policy == FeatureProcessPolicy::AutoNozzleVariant ?
                                       "Automatic: same as object" : "Same as object";
        if (std::abs(resolution.nozzle_diameter - object_nozzle) > NOZZLE_EPSILON) {
            resolution.rejection = FeatureProcessRejection::PolicyRequiresMatchingNozzle;
            resolution.rejection_args = {format_number(resolution.nozzle_diameter), format_number(object_nozzle)};
            return resolution;
        }
        if (resolution.cadence_ratio == 0) {
            set_divisor_rejection(resolution, printer_config);
            return resolution;
        }
        resolution.ok = true;
        append_cadence_label(resolution);
        return resolution;
    }

    const Preset *resolved = nullptr;
    if (request.policy == FeatureProcessPolicy::Pinned) {
        resolution.resolved_preset = request.pinned_preset_name;
        resolved = request.bundle->prints.find_preset(request.pinned_preset_name, false);
        if (resolved == nullptr) {
            resolution.rejection = FeatureProcessRejection::PresetMissing;
            resolution.rejection_args = {request.pinned_preset_name};
            resolution.display_label = format("Pinned: %1%", request.pinned_preset_name);
            return resolution;
        }
    } else {
        const Preset *object_process = request.bundle->prints.find_preset(request.object_process_preset, false);
        const std::string object_family = object_process == nullptr ? std::string() : process_family(*object_process, request.bundle->prints);
        struct RankedPreset {
            const Preset *preset;
            bool          same_family;
            int           divisor;
            double        height;
        };
        std::vector<RankedPreset> ranked;
        for (const Preset &candidate : request.bundle->prints) {
            if (candidate.is_default || !candidate.is_visible)
                continue;
            try {
                const FeatureProcessRejection rejection = preset_compatible_with_tool_impl(
                    candidate, *request.bundle, resolution.nozzle_diameter, printer_config, resolution.tool_id);
                if (rejection != FeatureProcessRejection::None)
                    continue;
                const double height = preset_layer_height(candidate, request.bundle->prints);
                const std::string family = process_family(candidate, request.bundle->prints);
                ranked.push_back({&candidate, !object_family.empty() && family == object_family,
                                  feature_cadence_ratio(resolution.base_layer_height, height), height});
            } catch (...) {
                // A malformed preset must not hide otherwise usable automatic candidates.
            }
        }
        std::sort(ranked.begin(), ranked.end(), [base = resolution.base_layer_height](const RankedPreset &left, const RankedPreset &right) {
            if (left.same_family != right.same_family)
                return left.same_family > right.same_family;
            if ((left.divisor > 0) != (right.divisor > 0))
                return left.divisor > 0;
            if (left.divisor > 0 && std::abs(left.height - right.height) > NOZZLE_EPSILON)
                return left.height > right.height;
            const double left_distance = std::abs(left.height - base);
            const double right_distance = std::abs(right.height - base);
            if (std::abs(left_distance - right_distance) > NOZZLE_EPSILON)
                return left_distance < right_distance;
            return left.preset->name < right.preset->name;
        });
        if (ranked.empty()) {
            resolution.rejection = FeatureProcessRejection::NoVariantFound;
            resolution.rejection_args = {format_number(resolution.nozzle_diameter)};
            return resolution;
        }
        resolved = ranked.front().preset;
        resolution.resolved_preset = resolved->name;
    }

    const double resolved_height = preset_layer_height(*resolved, request.bundle->prints);
    set_cadence_values(resolution, request.requested_layer_height > 0. ? request.requested_layer_height : resolved_height);
    resolution.display_label = request.policy == FeatureProcessPolicy::Pinned ?
                                   format("Pinned: %1%", request.pinned_preset_name) :
                                   format("Automatic: %1%", resolved->name);

    const FeatureProcessRejection compatibility = preset_compatible_with_tool_impl(
        *resolved, *request.bundle, resolution.nozzle_diameter, printer_config, resolution.tool_id);
    if (compatibility != FeatureProcessRejection::None) {
        resolution.rejection = compatibility;
        return resolution;
    }
    if (resolution.cadence_ratio == 0) {
        set_divisor_rejection(resolution, printer_config);
        return resolution;
    }

    build_projection(resolution, request.role, *resolved, request.bundle->prints);
    resolution.ok = true;
    append_cadence_label(resolution);
    return resolution;
}

const Preset *global_process_preset(const FilamentProcessRequest &request)
{
    if (request.bundle == nullptr)
        return nullptr;
    std::string name;
    if (request.full_config != nullptr)
        name = option_string(*request.full_config, "print_settings_id");
    if (name.empty())
        name = request.bundle->prints.get_selected_preset_name();
    return request.bundle->prints.find_preset(name, false);
}

FilamentProcessResolution resolve_filament_process_impl(const FilamentProcessRequest &request)
{
    FilamentProcessResolution resolution;
    const DynamicPrintConfig  full_config = materialized_printer_config(request.full_config);
    const GCodeConfig         gcode_config = gcode_config_from(&full_config);

    resolution.tool_id = unsigned(get_extruder_index_from_filament_id(gcode_config, request.filament_id));
    resolution.tool_nozzle = full_config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(resolution.tool_id);

    const Preset *global_process = global_process_preset(request);
    resolution.reference_nozzle = global_process == nullptr || request.bundle == nullptr ?
                                      resolution.tool_nozzle :
                                      inferred_preset_nozzle(*global_process, *request.bundle, resolution.tool_nozzle);

    if (request.policy == FilamentProcessPolicy::GlobalProcess) {
        resolution.ok = true;
        resolution.display_label = "Use the global process";
        return resolution;
    }
    if (request.bundle == nullptr) {
        resolution.rejection = FeatureProcessRejection::NoBundle;
        return resolution;
    }
    if (request.policy == FilamentProcessPolicy::AutoNozzleVariant &&
        std::abs(resolution.tool_nozzle - resolution.reference_nozzle) <= NOZZLE_EPSILON) {
        resolution.ok = true;
        resolution.display_label = "Automatic: use the global process";
        return resolution;
    }

    const Preset *resolved = nullptr;
    if (request.policy == FilamentProcessPolicy::Pinned) {
        resolution.resolved_preset = request.pinned_preset_name;
        resolved = request.bundle->prints.find_preset(request.pinned_preset_name, false);
        if (resolved == nullptr) {
            resolution.rejection = FeatureProcessRejection::PresetMissing;
            resolution.display_label = request.pinned_preset_name + " (missing)";
            return resolution;
        }
    } else {
        const std::string global_family = global_process == nullptr ? std::string() :
                                              process_family(*global_process, request.bundle->prints);
        // Rank by preserved height-to-nozzle ratio, not absolute height: a 0.10 mm process on a
        // 0.2 nozzle (ratio 0.5) maps to 0.20 mm on a 0.4 nozzle even though 0.16 is nearer to 0.10.
        const double reference_height = option_float_or_default(request.full_config, "layer_height");
        const double reference_ratio = resolution.reference_nozzle > EPSILON ?
                                           reference_height / resolution.reference_nozzle : 0.5;
        const double tool_nozzle = std::max(resolution.tool_nozzle, EPSILON);
        struct RankedPreset {
            const Preset *preset;
            bool          same_family;
            double        height;
        };
        std::vector<RankedPreset> ranked;
        for (const Preset &candidate : request.bundle->prints) {
            if (candidate.is_default || !candidate.is_visible)
                continue;
            try {
                if (preset_compatible_with_tool_impl(candidate, *request.bundle, resolution.tool_nozzle,
                                                     full_config, resolution.tool_id) != FeatureProcessRejection::None)
                    continue;
                const std::string family = process_family(candidate, request.bundle->prints);
                ranked.push_back({&candidate, !global_family.empty() && family == global_family,
                                  preset_layer_height(candidate, request.bundle->prints)});
            } catch (...) {
                // A malformed preset must not hide otherwise usable automatic candidates.
            }
        }
        std::sort(ranked.begin(), ranked.end(), [reference_ratio, tool_nozzle](const RankedPreset &left, const RankedPreset &right) {
            if (left.same_family != right.same_family)
                return left.same_family > right.same_family;
            const double left_distance = std::abs(left.height / tool_nozzle - reference_ratio);
            const double right_distance = std::abs(right.height / tool_nozzle - reference_ratio);
            if (std::abs(left_distance - right_distance) > NOZZLE_EPSILON)
                return left_distance < right_distance;
            return left.preset->name < right.preset->name;
        });
        if (ranked.empty()) {
            resolution.rejection = FeatureProcessRejection::NoVariantFound;
            return resolution;
        }
        resolved = ranked.front().preset;
        resolution.resolved_preset = resolved->name;
    }

    resolution.layer_height = preset_layer_height(*resolved, request.bundle->prints);
    resolution.display_label = request.policy == FilamentProcessPolicy::Pinned ?
                                   format("Pinned: %1%", resolved->name) : format("Automatic: %1%", resolved->name);
    const FeatureProcessRejection compatibility = preset_compatible_with_tool_impl(
        *resolved, *request.bundle, resolution.tool_nozzle, full_config, resolution.tool_id);
    if (compatibility != FeatureProcessRejection::None) {
        resolution.rejection = compatibility;
        return resolution;
    }

    build_filament_delta(resolution, *resolved, request.bundle->prints);
    resolution.ok = true;
    return resolution;
}

} // namespace

FeatureProcessResolution resolve_feature_process(const FeatureProcessRequest &request)
{
    try {
        return resolve_feature_process_impl(request);
    } catch (...) {
        FeatureProcessResolution resolution;
        resolution.rejection = request.bundle == nullptr ? FeatureProcessRejection::NoBundle : FeatureProcessRejection::NoVariantFound;
        return resolution;
    }
}

FilamentProcessResolution resolve_filament_process(const FilamentProcessRequest &request)
{
    try {
        return resolve_filament_process_impl(request);
    } catch (...) {
        FilamentProcessResolution resolution;
        resolution.rejection = request.bundle == nullptr ? FeatureProcessRejection::NoBundle : FeatureProcessRejection::NoVariantFound;
        return resolution;
    }
}

std::vector<FeatureProcessCandidate> enumerate_filament_process_candidates(const FilamentProcessRequest &request)
{
    std::vector<FeatureProcessCandidate> result;
    if (request.bundle == nullptr)
        return result;
    try {
        const DynamicPrintConfig full_config = materialized_printer_config(request.full_config);
        const GCodeConfig gcode_config = gcode_config_from(&full_config);
        const unsigned int tool_id = unsigned(get_extruder_index_from_filament_id(gcode_config, request.filament_id));
        const double tool_nozzle = full_config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(tool_id);

        for (const Preset &preset : request.bundle->prints) {
            if (preset.is_default || !preset.is_visible)
                continue;
            FeatureProcessCandidate candidate;
            candidate.preset_name = preset.name;
            try {
                candidate.preset_layer_height = preset_layer_height(preset, request.bundle->prints);
                candidate.preset_nozzle = inferred_preset_nozzle(preset, *request.bundle, tool_nozzle);
                candidate.why_not = preset_compatible_with_tool_impl(
                    preset, *request.bundle, tool_nozzle, full_config, tool_id);
            } catch (...) {
                candidate.why_not = FeatureProcessRejection::NoVariantFound;
            }
            candidate.compatible = candidate.why_not == FeatureProcessRejection::None;
            candidate.label = format("%1% — %2% mm, %3% mm nozzle", candidate.preset_name,
                                     format_number(candidate.preset_layer_height), format_number(candidate.preset_nozzle));
            result.push_back(std::move(candidate));
        }
    } catch (...) {
        // Candidate enumeration is advisory UI data; never let malformed presets escape.
    }
    return result;
}

bool update_filament_process_projections(const PresetBundle &bundle, DynamicPrintConfig &full_config)
{
    bool changed = false;
    try {
        size_t filament_count = bundle.filament_presets.size();
        if (filament_count == 0) {
            if (const auto *ids = full_config.option<ConfigOptionStrings>("filament_settings_id");
                ids != nullptr && !ids->values.empty())
                filament_count = ids->values.size();
            else if (const auto *map = full_config.option<ConfigOptionInts>("filament_map"); map != nullptr)
                filament_count = map->values.size();
        }

        const bool had_policy = full_config.has("filament_process_policy");
        const bool had_preset = full_config.has("filament_process_preset");
        const bool had_projection = full_config.has("filament_process_projection");
        auto *policies = full_config.option<ConfigOptionEnumsGeneric>("filament_process_policy", true);
        auto *presets = full_config.option<ConfigOptionStrings>("filament_process_preset", true);
        auto *projections = full_config.option<ConfigOptionStrings>("filament_process_projection", true);
        changed = !had_policy || !had_preset || !had_projection;
        if (policies->values.size() != filament_count) {
            policies->values.resize(filament_count, int(FilamentProcessPolicy::AutoNozzleVariant));
            changed = true;
        }
        if (presets->values.size() != filament_count) {
            presets->values.resize(filament_count, "");
            changed = true;
        }
        if (projections->values.size() != filament_count) {
            projections->values.resize(filament_count, "");
            changed = true;
        }

        for (size_t index = 0; index < filament_count; ++index) {
            FilamentProcessPolicy policy = FilamentProcessPolicy::AutoNozzleVariant;
            if (policies->values[index] == int(FilamentProcessPolicy::Pinned))
                policy = FilamentProcessPolicy::Pinned;
            else if (policies->values[index] == int(FilamentProcessPolicy::GlobalProcess))
                policy = FilamentProcessPolicy::GlobalProcess;

            FilamentProcessRequest request;
            request.filament_id = unsigned(index + 1);
            request.policy = policy;
            request.pinned_preset_name = presets->values[index];
            request.bundle = &bundle;
            request.full_config = &full_config;
            const FilamentProcessResolution resolution = resolve_filament_process(request);
            const std::string serialized = resolution.ok ? serialize_projection(resolution.delta) : std::string();
            if (projections->values[index] != serialized) {
                projections->values[index] = serialized;
                changed = true;
            }
        }
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Filament process projection refresh failed: " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "Filament process projection refresh failed with an unknown error";
    }
    return changed;
}

bool update_feature_process_projections(Model &model, const PresetBundle &bundle,
                                        const DynamicPrintConfig &full_config)
{
    bool changed = false;
    for (ModelObject *object : model.objects) {
        if (object == nullptr)
            continue;
        try {
            DynamicPrintConfig effective = full_config;
            effective.apply(object->config.get(), true);
            for (const FeatureProjectionDescriptor &descriptor : feature_projection_descriptors)
                changed = update_scope_projection(object->config, effective, descriptor, bundle, full_config) || changed;

            // Recompose after updating the object so volume scopes inherit the refreshed values.
            DynamicPrintConfig object_effective = full_config;
            object_effective.apply(object->config.get(), true);
            for (ModelVolume *volume : object->volumes) {
                if (volume == nullptr)
                    continue;
                DynamicPrintConfig volume_effective = object_effective;
                volume_effective.apply(volume->config.get(), true);
                for (const FeatureProjectionDescriptor &descriptor : feature_projection_descriptors) {
                    if (!volume_scope_affects_feature(volume->config, descriptor)) {
                        changed = volume->config.erase(descriptor.projection_key) || changed;
                        continue;
                    }
                    changed = update_scope_projection(volume->config, volume_effective, descriptor, bundle, full_config) || changed;
                }
            }
        } catch (const std::exception &error) {
            // Model configs and third-party presets are sparse and may be malformed. Projection
            // refresh is advisory and must never prevent loading or slicing the project.
            BOOST_LOG_TRIVIAL(warning) << "Feature process projection refresh failed: " << error.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "Feature process projection refresh failed with an unknown error";
        }
    }
    return changed;
}

std::vector<FeatureProcessCandidate> enumerate_feature_process_candidates(const FeatureProcessRequest &request)
{
    std::vector<FeatureProcessCandidate> result;
    if (request.bundle == nullptr)
        return result;
    try {
        const DynamicPrintConfig printer_config = materialized_printer_config(request_printer_config(request));
        const GCodeConfig gcode_config = gcode_config_from(&printer_config);
        const unsigned int filament = request.feature_filament > 0 ? unsigned(request.feature_filament) : 0;
        const unsigned int tool_id = unsigned(get_extruder_index_from_filament_id(gcode_config, filament));
        const double tool_nozzle = printer_config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(tool_id);
        const double base_height = request_base_layer_height(request);

        for (const Preset &preset : request.bundle->prints) {
            if (preset.is_default || !preset.is_visible)
                continue;
            FeatureProcessCandidate candidate;
            candidate.preset_name = preset.name;
            try {
                candidate.preset_layer_height = preset_layer_height(preset, request.bundle->prints);
                candidate.preset_nozzle = inferred_preset_nozzle(preset, *request.bundle, tool_nozzle);
                candidate.why_not = preset_compatible_with_tool_impl(
                    preset, *request.bundle, tool_nozzle, printer_config, tool_id);
                if (candidate.why_not == FeatureProcessRejection::None) {
                    const double cadence_height = request.requested_layer_height > 0. ?
                                                      request.requested_layer_height : candidate.preset_layer_height;
                    if (feature_cadence_ratio(base_height, cadence_height) == 0)
                        candidate.why_not = FeatureProcessRejection::LayerHeightNotDivisor;
                }
            } catch (...) {
                candidate.why_not = FeatureProcessRejection::NoVariantFound;
            }
            candidate.compatible = candidate.why_not == FeatureProcessRejection::None;
            candidate.label = format("%1% — %2% mm, %3% mm nozzle", candidate.preset_name,
                                     format_number(candidate.preset_layer_height), format_number(candidate.preset_nozzle));
            result.push_back(std::move(candidate));
        }
    } catch (...) {
        // Candidate enumeration is advisory UI data; never let malformed presets escape.
    }
    return result;
}

FeatureProcessRejection preset_compatible_with_tool(const Preset       &process_preset,
                                                    const PresetBundle &bundle,
                                                    double              tool_nozzle_diameter)
{
    try {
        const DynamicPrintConfig printer_config = materialized_printer_config(&bundle.printers.get_selected_preset().config);
        const unsigned int tool_id = tool_for_nozzle(printer_config, tool_nozzle_diameter);
        return preset_compatible_with_tool_impl(process_preset, bundle, tool_nozzle_diameter, printer_config, tool_id);
    } catch (...) {
        return FeatureProcessRejection::NoVariantFound;
    }
}

const std::vector<std::string> &feature_projection_keys(FeatureRole role)
{
    static const std::vector<std::string> wall_keys{
        "outer_wall_line_width",
        "inner_wall_line_width",
        "outer_wall_speed",
        "inner_wall_speed",
        "small_perimeter_speed",
        "small_perimeter_threshold",
        "enable_overhang_speed",
        "overhang_1_4_speed",
        "overhang_2_4_speed",
        "overhang_3_4_speed",
        "overhang_4_4_speed",
        "outer_wall_acceleration",
        "inner_wall_acceleration",
        "outer_wall_jerk",
        "inner_wall_jerk",
        "wall_sequence",
        "precise_outer_wall",
        "infill_wall_overlap",
    };
    static const std::vector<std::string> empty;
    // Interior and support projections are introduced with their Phase 3 consumers.
    return role == FeatureRole::Wall ? wall_keys : empty;
}

int feature_cadence_ratio(double base_layer_height, double feature_layer_height)
{
    if (!std::isfinite(base_layer_height) || !std::isfinite(feature_layer_height) ||
        base_layer_height <= 0. || feature_layer_height <= 0.)
        return 0;
    const double raw_ratio = base_layer_height / feature_layer_height;
    if (raw_ratio > double(std::numeric_limits<int>::max()))
        return 0;
    const long long ratio = std::llround(raw_ratio);
    if (ratio < 1 || ratio > std::numeric_limits<int>::max())
        return 0;
    return std::abs(base_layer_height - double(ratio) * feature_layer_height) <= DIVISOR_EPSILON ? int(ratio) : 0;
}

} // namespace Slic3r
