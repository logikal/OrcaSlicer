#include "FeatureProcessResolver.hpp"

#include "PlaceholderParser.hpp"
#include "PresetBundle.hpp"
#include "Slicing.hpp"
#include "format.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <regex>
#include <set>
#include <sstream>

namespace Slic3r {

namespace {

constexpr double NOZZLE_EPSILON = 1e-6;
constexpr double DIVISOR_EPSILON = 0.0005;

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
    const unsigned int filament = object_filament > 0 ? unsigned(object_filament) : 0;
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
