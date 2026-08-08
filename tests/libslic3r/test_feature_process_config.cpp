#include <catch2/catch_all.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <array>
#include <string>

using namespace Slic3r;

namespace {

struct FeatureProcessKeys {
    const char *policy;
    const char *preset;
    const char *layer_height;
    const char *projection;
};

constexpr std::array<FeatureProcessKeys, 7> feature_process_keys{{
    {"wall_process_policy", "wall_process_preset", "wall_layer_height", "wall_process_projection"},
    {"sparse_infill_process_policy", "sparse_infill_process_preset", "sparse_infill_process_layer_height", "sparse_infill_process_projection"},
    {"internal_solid_process_policy", "internal_solid_process_preset", "internal_solid_process_layer_height", "internal_solid_process_projection"},
    {"top_surface_process_policy", "top_surface_process_preset", "top_surface_process_layer_height", "top_surface_process_projection"},
    {"bottom_surface_process_policy", "bottom_surface_process_preset", "bottom_surface_process_layer_height", "bottom_surface_process_projection"},
    {"support_process_policy", "support_process_preset", "support_process_layer_height", "support_process_projection"},
    {"support_interface_process_policy", "support_interface_process_preset", "support_interface_process_layer_height", "support_interface_process_projection"},
}};

constexpr std::array<std::pair<const char *, FeatureProcessPolicy>, 3> policy_values{{
    {"auto_nozzle_variant", FeatureProcessPolicy::AutoNozzleVariant},
    {"pinned", FeatureProcessPolicy::Pinned},
    {"same_as_object", FeatureProcessPolicy::SameAsObject},
}};

} // namespace

TEST_CASE("Feature process config keys round trip and remain print options", "[FeatureProcessConfig]")
{
    DynamicPrintConfig source = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig restored;
    const std::vector<std::string> &print_options = Preset::print_options();

    for (size_t i = 0; i < feature_process_keys.size(); ++i) {
        const FeatureProcessKeys &keys = feature_process_keys[i];
        INFO(keys.policy);

        const auto *default_policy = source.option<ConfigOptionEnum<FeatureProcessPolicy>>(keys.policy);
        REQUIRE(default_policy != nullptr);
        CHECK(default_policy->value == FeatureProcessPolicy::AutoNozzleVariant);

        const auto &[policy_token, policy_value] = policy_values[i % policy_values.size()];
        const std::string preset_value = "Pinned process " + std::to_string(i);
        const std::string height_value = std::to_string(0.1 + 0.01 * i);
        const std::string projection_value = "outer_wall_line_width=" + std::to_string(0.2 + 0.01 * i);

        source.set_deserialize_strict(keys.policy, policy_token);
        source.set_deserialize_strict(keys.preset, preset_value);
        source.set_deserialize_strict(keys.layer_height, height_value);
        source.set_deserialize_strict(keys.projection, projection_value);

        for (const char *key : {keys.policy, keys.preset, keys.layer_height, keys.projection}) {
            restored.set_deserialize_strict(key, source.option_throw(key)->serialize());
            CHECK(restored.option_throw(key)->serialize() == source.option_throw(key)->serialize());
            CHECK(std::find(print_options.begin(), print_options.end(), key) != print_options.end());
        }

        REQUIRE(restored.option<ConfigOptionEnum<FeatureProcessPolicy>>(keys.policy) != nullptr);
        CHECK(restored.option<ConfigOptionEnum<FeatureProcessPolicy>>(keys.policy)->value == policy_value);
        CHECK(restored.option<ConfigOptionString>(keys.preset)->value == preset_value);
        CHECK_THAT(restored.option<ConfigOptionFloat>(keys.layer_height)->value,
                   Catch::Matchers::WithinAbs(std::stod(height_value), 1e-9));
        CHECK(restored.option<ConfigOptionString>(keys.projection)->value == projection_value);
    }

    for (const auto &[policy_token, policy_value] : policy_values) {
        DynamicPrintConfig token_round_trip;
        token_round_trip.set_deserialize_strict("wall_process_policy", policy_token);
        CHECK(token_round_trip.option<ConfigOptionEnum<FeatureProcessPolicy>>("wall_process_policy")->value == policy_value);
        CHECK(token_round_trip.option_throw("wall_process_policy")->serialize() == policy_token);
    }
}
