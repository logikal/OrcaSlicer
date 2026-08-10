#pragma once

#include "PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class Preset;
class PresetBundle;
class Model;

enum class FeatureRole {
    Wall,
    SparseInfill,
    InternalSolid,
    TopSurface,
    BottomSurface,
    Support,
    SupportInterface,
};

enum class FeatureProcessRejection {
    None,
    PresetMissing,
    PrinterModelMismatch,
    NozzleMismatch,
    LayerHeightNotDivisor,
    LayerHeightOutOfToolRange,
    NoVariantFound,
    PolicyRequiresMatchingNozzle,
    NoBundle,
};

struct FeatureProcessRequest {
    FeatureRole               role = FeatureRole::Wall;
    FeatureProcessPolicy      policy = FeatureProcessPolicy::AutoNozzleVariant;
    std::string               pinned_preset_name;
    int                       feature_filament = 0;
    double                    requested_layer_height = 0.;
    const DynamicPrintConfig *effective_object_config = nullptr;
    std::string               object_process_preset;
    const PresetBundle       *bundle = nullptr;
    const DynamicPrintConfig *printer_config = nullptr;
};

struct FeatureProcessResolution {
    bool                      ok = false;
    FeatureProcessRejection   rejection = FeatureProcessRejection::None;
    std::vector<std::string>  rejection_args;
    std::string               resolved_preset;
    unsigned int              tool_id = 0;
    double                    nozzle_diameter = 0.;
    double                    feature_layer_height = 0.;
    double                    base_layer_height = 0.;
    double                    grid_height = 0.;
    int                       cadence_ratio = 1;
    // Vector options contain only source index zero. P1.3 expands that value into
    // the resolved tool slot while consuming the projection.
    DynamicPrintConfig        projection;
    std::string               display_label;
};

FeatureProcessResolution resolve_feature_process(const FeatureProcessRequest &request);

struct FeatureProcessCandidate {
    std::string             preset_name;
    bool                    compatible = false;
    FeatureProcessRejection why_not = FeatureProcessRejection::None;
    double                  preset_layer_height = 0.;
    double                  preset_nozzle = 0.;
    std::string             label;
};

std::vector<FeatureProcessCandidate> enumerate_feature_process_candidates(const FeatureProcessRequest &request);

struct FilamentProcessRequest {
    unsigned int              filament_id = 0;
    FilamentProcessPolicy     policy = FilamentProcessPolicy::AutoNozzleVariant;
    std::string               pinned_preset_name;
    const PresetBundle       *bundle = nullptr;
    const DynamicPrintConfig *full_config = nullptr;
};

struct FilamentProcessResolution {
    bool                    ok = false;
    FeatureProcessRejection rejection = FeatureProcessRejection::None;
    std::string             resolved_preset;
    unsigned int            tool_id = 0;
    double                  tool_nozzle = 0.;
    double                  reference_nozzle = 0.;
    double                  layer_height = 0.;
    DynamicPrintConfig      delta;
    std::string             display_label;
};

FilamentProcessResolution resolve_filament_process(const FilamentProcessRequest &request);
std::vector<FeatureProcessCandidate> enumerate_filament_process_candidates(const FilamentProcessRequest &request);
bool update_filament_process_projections(const PresetBundle &bundle, DynamicPrintConfig &full_config);

FeatureProcessRejection preset_compatible_with_tool(const Preset       &process_preset,
                                                    const PresetBundle &bundle,
                                                    double              tool_nozzle_diameter);

const std::vector<std::string> &feature_projection_keys(FeatureRole role);
const std::vector<std::string> &filament_delta_keys_for_role(FeatureRole role);

int feature_cadence_ratio(double base_layer_height, double feature_layer_height);

// Refresh derived feature projections stored on object and volume scopes.
bool update_feature_process_projections(Model &model, const PresetBundle &bundle,
                                        const DynamicPrintConfig &full_config);

} // namespace Slic3r
