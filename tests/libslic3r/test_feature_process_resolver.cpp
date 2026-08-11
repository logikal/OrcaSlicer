#include <catch2/catch_all.hpp>

#include "libslic3r/FeatureProcessResolver.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>

using namespace Slic3r;

namespace {

class ResolverFixture
{
public:
    static constexpr const char *printer_04 = "TestPrinter 0.4 nozzle";
    static constexpr const char *printer_02 = "TestPrinter 0.2 nozzle";
    static constexpr const char *standard_04 = "0.20mm Standard @TestPrinter 0.4 nozzle";
    static constexpr const char *standard_02 = "0.10mm Standard @TestPrinter 0.2 nozzle";
    static constexpr const char *fine_012 = "0.12mm Fine @TestPrinter 0.2 nozzle";
    static constexpr const char *quality_02 = "0.10mm High Quality @TestPrinter 0.2 nozzle";

    PresetBundle       bundle;
    DynamicPrintConfig printer_config = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig object_config = DynamicPrintConfig::full_print_config();

    ResolverFixture()
    {
        add_printer(printer_02, 0.2, false);
        add_printer(printer_04, 0.4, true);

        add_process(standard_04, 0.2, printer_04);
        add_process(standard_02, 0.1, printer_02);
        add_process(fine_012, 0.12, printer_02);
        add_process(quality_02, 0.1, printer_02);

        printer_config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.2}));
        printer_config.set_key_value("filament_map", new ConfigOptionInts({1, 2}));
        printer_config.set_key_value("min_layer_height", new ConfigOptionFloats({0.07, 0.07}));
        printer_config.set_key_value("max_layer_height", new ConfigOptionFloats({0.3, 0.15}));
        printer_config.option<ConfigOptionString>("printer_model", true)->value = "TestPrinter";

        object_config.option<ConfigOptionFloat>("layer_height", true)->value = 0.2;
        object_config.option<ConfigOptionInt>("extruder", true)->value = 1;
    }

    FeatureProcessRequest request(FeatureProcessPolicy policy = FeatureProcessPolicy::AutoNozzleVariant) const
    {
        FeatureProcessRequest result;
        result.role = FeatureRole::Wall;
        result.policy = policy;
        result.feature_filament = 2;
        result.effective_object_config = &object_config;
        result.object_process_preset = standard_04;
        result.bundle = &bundle;
        result.printer_config = &printer_config;
        return result;
    }

    Preset &add_process(const std::string &name, double height, const std::string &compatible_printer,
                        const std::string &condition = {}, const std::string &family = {})
    {
        DynamicPrintConfig config(bundle.prints.default_preset().config);
        config.option<ConfigOptionFloat>("layer_height", true)->value = height;
        config.option<ConfigOptionStrings>("compatible_printers", true)->values =
            compatible_printer.empty() ? std::vector<std::string>() : std::vector<std::string>{compatible_printer};
        config.option<ConfigOptionString>("compatible_printers_condition", true)->value = condition;
        config.option<ConfigOptionFloats>("outer_wall_speed", true)->values = {42., 99.};
        config.option<ConfigOptionInt>("wall_loops", true)->value = 9;
        config.option<ConfigOptionInts>("nozzle_temperature", true)->values = {275};
        if (!family.empty())
            config.set_key_value("process_family", new ConfigOptionString(family));
        Preset &preset = bundle.prints.load_preset({}, name, std::move(config), false);
        preset.is_system = true;
        return preset;
    }

private:
    void add_printer(const std::string &name, double nozzle, bool select)
    {
        DynamicPrintConfig config(bundle.printers.default_preset().config);
        config.option<ConfigOptionString>("printer_model", true)->value = "TestPrinter";
        config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {nozzle};
        config.option<ConfigOptionFloats>("min_layer_height", true)->values = {0.07};
        config.option<ConfigOptionFloats>("max_layer_height", true)->values = {nozzle == 0.2 ? 0.15 : 0.3};
        Preset &preset = bundle.printers.load_preset({}, name, std::move(config), select);
        preset.is_system = true;
    }
};

const FeatureProcessCandidate *find_candidate(const std::vector<FeatureProcessCandidate> &candidates, const std::string &name)
{
    const auto found = std::find_if(candidates.begin(), candidates.end(), [&name](const FeatureProcessCandidate &candidate) {
        return candidate.preset_name == name;
    });
    return found == candidates.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("Automatic feature process picks the nozzle matched family divisor", "[FeatureProcessResolver]")
{
    ResolverFixture fixture;
    const FeatureProcessResolution result = resolve_feature_process(fixture.request());

    REQUIRE(result.ok);
    CHECK(result.rejection == FeatureProcessRejection::None);
    CHECK(result.resolved_preset == ResolverFixture::standard_02);
    CHECK(result.tool_id == 1);
    CHECK_THAT(result.nozzle_diameter, Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK_THAT(result.feature_layer_height, Catch::Matchers::WithinAbs(0.1, 1e-9));
    CHECK(result.cadence_ratio == 2);
    CHECK_THAT(result.grid_height, Catch::Matchers::WithinAbs(0.1, 1e-9));
}

TEST_CASE("Explicit process family outranks the system name grammar", "[FeatureProcessResolver]")
{
    ResolverFixture fixture;
    Preset *object_process = fixture.bundle.prints.find_preset(ResolverFixture::standard_04, false, true);
    REQUIRE(object_process != nullptr);
    object_process->config.set_key_value("process_family", new ConfigOptionString("Engineered Standard"));
    fixture.add_process("0.10mm High Quality Explicit @TestPrinter 0.2 nozzle", 0.1,
                        ResolverFixture::printer_02, {}, "Engineered Standard");

    const FeatureProcessResolution result = resolve_feature_process(fixture.request());
    REQUIRE(result.ok);
    CHECK(result.resolved_preset == "0.10mm High Quality Explicit @TestPrinter 0.2 nozzle");
}

TEST_CASE("Automatic feature process uses the object process when nozzles match", "[FeatureProcessResolver]")
{
    ResolverFixture fixture;
    FeatureProcessRequest request = fixture.request();
    request.feature_filament = 1;

    const FeatureProcessResolution result = resolve_feature_process(request);
    REQUIRE(result.ok);
    CHECK(result.resolved_preset.empty());
    CHECK(result.cadence_ratio == 1);
    CHECK(result.display_label.find("same as object") != std::string::npos);
}

TEST_CASE("Default object filament resolves through either mixed-nozzle map", "[FeatureProcessResolver][Regression]")
{
    const bool fine_nozzle_is_first = GENERATE(false, true);
    ResolverFixture fixture;
    fixture.printer_config.option<ConfigOptionFloats>("nozzle_diameter", true)->values =
        fine_nozzle_is_first ? std::vector<double>{0.2, 0.4} : std::vector<double>{0.4, 0.2};
    fixture.printer_config.option<ConfigOptionInts>("filament_map", true)->values =
        fine_nozzle_is_first ? std::vector<int>{2, 1} : std::vector<int>{1, 2};
    fixture.object_config.option<ConfigOptionInt>("extruder", true)->value = 0;

    DYNAMIC_SECTION("fine nozzle on tool " << (fine_nozzle_is_first ? 0 : 1)) {
        const FeatureProcessResolution result = resolve_feature_process(fixture.request());

        REQUIRE(result.ok);
        CHECK(result.resolved_preset == ResolverFixture::standard_02);
        CHECK(result.tool_id == (fine_nozzle_is_first ? 0 : 1));
        CHECK_THAT(result.nozzle_diameter, Catch::Matchers::WithinAbs(0.2, 1e-9));
        CHECK(result.display_label.find("same as object") == std::string::npos);
    }
}

TEST_CASE("Pinned feature process projects only the wall whitelist", "[FeatureProcessResolver]")
{
    ResolverFixture fixture;
    FeatureProcessRequest request = fixture.request(FeatureProcessPolicy::Pinned);
    request.pinned_preset_name = ResolverFixture::standard_02;

    const FeatureProcessResolution result = resolve_feature_process(request);
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.projection.empty());
    const std::set<std::string> whitelist(feature_projection_keys(FeatureRole::Wall).begin(),
                                          feature_projection_keys(FeatureRole::Wall).end());
    for (const std::string &key : result.projection.keys())
        CHECK(whitelist.count(key) == 1);
    CHECK(result.projection.option("nozzle_temperature") == nullptr);
    CHECK(result.projection.option("layer_height") == nullptr);
    CHECK(result.projection.option("wall_layer_height") == nullptr);
    CHECK(result.projection.option("wall_loops") == nullptr);

    const auto *speed = result.projection.option<ConfigOptionFloats>("outer_wall_speed");
    REQUIRE(speed != nullptr);
    REQUIRE(speed->values.size() == 1);
    CHECK_THAT(speed->values.front(), Catch::Matchers::WithinAbs(42., 1e-9));
}

TEST_CASE("Feature process projection whitelists carry each resolved role height", "[FeatureProcessResolver]")
{
    const std::array<std::pair<FeatureRole, std::string>, 5> role_heights{{
        {FeatureRole::Wall, "wall_layer_height"},
        {FeatureRole::SparseInfill, "sparse_infill_process_layer_height"},
        {FeatureRole::InternalSolid, "internal_solid_process_layer_height"},
        {FeatureRole::TopSurface, "top_surface_process_layer_height"},
        {FeatureRole::BottomSurface, "bottom_surface_process_layer_height"},
    }};
    for (const auto &[role, height_key] : role_heights) {
        const std::vector<std::string> &keys = feature_projection_keys(role);
        CHECK(std::find(keys.begin(), keys.end(), height_key) != keys.end());
    }
}

TEST_CASE("Pinned incompatible and missing feature processes preserve their identity", "[FeatureProcessResolver]")
{
    ResolverFixture fixture;
    FeatureProcessRequest request = fixture.request(FeatureProcessPolicy::Pinned);

    request.pinned_preset_name = ResolverFixture::standard_04;
    FeatureProcessResolution result = resolve_feature_process(request);
    CHECK_FALSE(result.ok);
    CHECK(result.rejection == FeatureProcessRejection::NozzleMismatch);
    CHECK(result.resolved_preset == ResolverFixture::standard_04);
    CHECK_THAT(result.feature_layer_height, Catch::Matchers::WithinAbs(0.2, 1e-9));

    request.pinned_preset_name = "Missing process";
    result = resolve_feature_process(request);
    CHECK_FALSE(result.ok);
    CHECK(result.rejection == FeatureProcessRejection::PresetMissing);
    REQUIRE(result.rejection_args.size() == 1);
    CHECK(result.rejection_args.front() == "Missing process");
}

TEST_CASE("Same as object rejects a feature tool with a different nozzle", "[FeatureProcessResolver]")
{
    ResolverFixture fixture;
    const FeatureProcessResolution result = resolve_feature_process(fixture.request(FeatureProcessPolicy::SameAsObject));
    CHECK_FALSE(result.ok);
    CHECK(result.rejection == FeatureProcessRejection::PolicyRequiresMatchingNozzle);
    CHECK(result.resolved_preset.empty());
}

TEST_CASE("Feature cadence accepts integer divisors and rejects other heights", "[FeatureProcessResolver]")
{
    CHECK(feature_cadence_ratio(0.2, 0.1) == 2);
    CHECK(feature_cadence_ratio(0.2, 0.2) == 1);
    CHECK(feature_cadence_ratio(0.2, 0.12) == 0);

    ResolverFixture fixture;
    FeatureProcessRequest request = fixture.request(FeatureProcessPolicy::Pinned);
    request.pinned_preset_name = ResolverFixture::fine_012;
    const FeatureProcessResolution result = resolve_feature_process(request);
    CHECK_FALSE(result.ok);
    CHECK(result.rejection == FeatureProcessRejection::LayerHeightNotDivisor);
    REQUIRE(result.rejection_args.size() >= 2);
    CHECK(result.rejection_args[0] == "0.12");
    CHECK(result.rejection_args[1] == "0.2");
}

TEST_CASE("Printer compatibility conditions evaluate with the feature tool nozzle", "[FeatureProcessResolver]")
{
    ResolverFixture fixture;
    Preset &condition = fixture.add_process("Condition process", 0.1, {}, "nozzle_diameter[0] == 0.2");

    CHECK(preset_compatible_with_tool(condition, fixture.bundle, 0.2) == FeatureProcessRejection::None);
    CHECK(preset_compatible_with_tool(condition, fixture.bundle, 0.4) == FeatureProcessRejection::NozzleMismatch);
}

TEST_CASE("Headless feature resolution retains persisted cadence numbers", "[FeatureProcessResolver]")
{
    ResolverFixture fixture;
    FeatureProcessRequest request = fixture.request();
    request.bundle = nullptr;
    request.requested_layer_height = 0.1;

    const FeatureProcessResolution result = resolve_feature_process(request);
    CHECK_FALSE(result.ok);
    CHECK(result.rejection == FeatureProcessRejection::NoBundle);
    CHECK(result.tool_id == 1);
    CHECK_THAT(result.nozzle_diameter, Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK_THAT(result.base_layer_height, Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK_THAT(result.feature_layer_height, Catch::Matchers::WithinAbs(0.1, 1e-9));
    CHECK(result.cadence_ratio == 2);
    CHECK_THAT(result.grid_height, Catch::Matchers::WithinAbs(0.1, 1e-9));
}

TEST_CASE("Feature process candidates retain incompatible presets with reasons", "[FeatureProcessResolver]")
{
    ResolverFixture fixture;
    const std::vector<FeatureProcessCandidate> candidates = enumerate_feature_process_candidates(fixture.request());

    const FeatureProcessCandidate *compatible = find_candidate(candidates, ResolverFixture::standard_02);
    REQUIRE(compatible != nullptr);
    CHECK(compatible->compatible);
    CHECK(compatible->why_not == FeatureProcessRejection::None);

    const FeatureProcessCandidate *wrong_nozzle = find_candidate(candidates, ResolverFixture::standard_04);
    REQUIRE(wrong_nozzle != nullptr);
    CHECK_FALSE(wrong_nozzle->compatible);
    CHECK(wrong_nozzle->why_not == FeatureProcessRejection::NozzleMismatch);

    const FeatureProcessCandidate *non_divisor = find_candidate(candidates, ResolverFixture::fine_012);
    REQUIRE(non_divisor != nullptr);
    CHECK_FALSE(non_divisor->compatible);
    CHECK(non_divisor->why_not == FeatureProcessRejection::LayerHeightNotDivisor);
}

TEST_CASE("Feature process projection updater refreshes model scopes without changing intent", "[FeatureProcessResolver]")
{
    ResolverFixture fixture;
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(fixture.printer_config);
    full_config.option<ConfigOptionFloat>("layer_height", true)->value = 0.2;
    full_config.option<ConfigOptionInt>("extruder", true)->value = 1;
    full_config.option<ConfigOptionString>("print_settings_id", true)->value = ResolverFixture::standard_04;
    full_config.option<ConfigOptionInt>("wall_loops", true)->value = 2;
    full_config.option<ConfigOptionInt>("outer_wall_filament_id", true)->value = 1;

    Model model;
    ModelObject *object = model.add_object("cube", "", make_cube(20., 20., 20.));
    object->add_instance();

    CHECK_FALSE(update_feature_process_projections(model, fixture.bundle, full_config));
    CHECK_FALSE(object->config.has("wall_process_projection"));

    object->config.set("outer_wall_filament_id", 2);
    REQUIRE(update_feature_process_projections(model, fixture.bundle, full_config));
    const auto *projection = dynamic_cast<const ConfigOptionString *>(object->config.option("wall_process_projection"));
    REQUIRE(projection != nullptr);
    CHECK(projection->value.find("outer_wall_speed=") != std::string::npos);
    CHECK(projection->value.find("nozzle_temperature") == std::string::npos);
    CHECK(projection->value.find("wall_layer_height=0.1") != std::string::npos);
    CHECK_FALSE(object->config.has("wall_layer_height"));
    CHECK_FALSE(update_feature_process_projections(model, fixture.bundle, full_config));

    full_config.option<ConfigOptionFloats>("nozzle_diameter")->values = {0.4, 0.4};
    REQUIRE(update_feature_process_projections(model, fixture.bundle, full_config));
    CHECK_FALSE(object->config.has("wall_process_projection"));
    CHECK_FALSE(object->config.has("wall_layer_height"));

    full_config.option<ConfigOptionFloats>("nozzle_diameter")->values = {0.4, 0.2};
    REQUIRE(update_feature_process_projections(model, fixture.bundle, full_config));

    object->config.set_key_value("wall_process_policy", new ConfigOptionEnum<FeatureProcessPolicy>(FeatureProcessPolicy::Pinned));
    object->config.set("wall_process_preset", std::string("Missing process"));
    REQUIRE(update_feature_process_projections(model, fixture.bundle, full_config));
    CHECK_FALSE(object->config.has("wall_process_projection"));
    const auto *policy = dynamic_cast<const ConfigOptionEnum<FeatureProcessPolicy> *>(object->config.option("wall_process_policy"));
    REQUIRE(policy != nullptr);
    CHECK(policy->value == FeatureProcessPolicy::Pinned);
    CHECK(object->config.opt_serialize("wall_process_preset") == "Missing process");
}

TEST_CASE("User-set feature height survives resolution and nozzle re-resolution",
          "[FeatureProcessResolver][Regression]")
{
    ResolverFixture fixture;
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(fixture.printer_config);
    full_config.option<ConfigOptionFloat>("layer_height", true)->value = 0.2;
    full_config.option<ConfigOptionInt>("extruder", true)->value = 1;
    full_config.option<ConfigOptionString>("print_settings_id", true)->value = ResolverFixture::standard_04;
    full_config.option<ConfigOptionInt>("wall_loops", true)->value = 2;

    Model model;
    ModelObject *object = model.add_object("cube", "", make_cube(20., 20., 20.));
    object->add_instance();
    object->config.set("outer_wall_filament_id", 2);
    object->config.set("wall_layer_height", 0.05);

    REQUIRE(update_feature_process_projections(model, fixture.bundle, full_config));
    CHECK_THAT(object->config.opt_float("wall_layer_height"), Catch::Matchers::WithinAbs(0.05, 1e-9));
    const auto *projection = dynamic_cast<const ConfigOptionString *>(object->config.option("wall_process_projection"));
    REQUIRE(projection != nullptr);
    CHECK(projection->value.find("wall_layer_height=") == std::string::npos);
    CHECK_FALSE(update_feature_process_projections(model, fixture.bundle, full_config));
    CHECK_THAT(object->config.opt_float("wall_layer_height"), Catch::Matchers::WithinAbs(0.05, 1e-9));

    full_config.option<ConfigOptionFloats>("nozzle_diameter")->values = {0.4, 0.4};
    REQUIRE(update_feature_process_projections(model, fixture.bundle, full_config));
    CHECK_THAT(object->config.opt_float("wall_layer_height"), Catch::Matchers::WithinAbs(0.05, 1e-9));
    CHECK_FALSE(object->config.has("wall_process_projection"));
}

TEST_CASE("Legacy derived feature height migrates into its projection", "[FeatureProcessResolver][Regression]")
{
    ResolverFixture fixture;
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(fixture.printer_config);
    full_config.option<ConfigOptionFloat>("layer_height", true)->value = 0.2;
    full_config.option<ConfigOptionInt>("extruder", true)->value = 1;
    full_config.option<ConfigOptionString>("print_settings_id", true)->value = ResolverFixture::standard_04;
    full_config.option<ConfigOptionInt>("wall_loops", true)->value = 2;

    Model model;
    ModelObject *object = model.add_object("cube", "", make_cube(20., 20., 20.));
    object->add_instance();
    object->config.set("outer_wall_filament_id", 2);
    object->config.set("wall_process_projection", std::string("outer_wall_speed=42"));
    object->config.set("wall_layer_height", 0.1);

    ModelObject *explicit_object = model.add_object("explicit cube", "", make_cube(20., 20., 20.));
    explicit_object->add_instance();
    explicit_object->config.set("outer_wall_filament_id", 2);
    explicit_object->config.set("wall_process_projection", std::string("outer_wall_speed=42"));
    explicit_object->config.set("wall_layer_height", 0.2);

    REQUIRE(update_feature_process_projections(model, fixture.bundle, full_config));
    CHECK_FALSE(object->config.has("wall_layer_height"));
    const auto *projection = dynamic_cast<const ConfigOptionString *>(object->config.option("wall_process_projection"));
    REQUIRE(projection != nullptr);
    CHECK(projection->value.find("wall_layer_height=0.1") != std::string::npos);
    CHECK_THAT(explicit_object->config.opt_float("wall_layer_height"), Catch::Matchers::WithinAbs(0.2, 1e-9));
    const auto *explicit_projection = dynamic_cast<const ConfigOptionString *>(
        explicit_object->config.option("wall_process_projection"));
    REQUIRE(explicit_projection != nullptr);
    CHECK(explicit_projection->value.find("wall_layer_height=") == std::string::npos);
    CHECK_FALSE(update_feature_process_projections(model, fixture.bundle, full_config));
}

TEST_CASE("Global filament action clears only matching automatic feature state",
          "[FeatureProcessResolver][Regression]")
{
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.option<ConfigOptionInt>("extruder", true)->value = 1;
    full_config.option<ConfigOptionInt>("outer_wall_filament_id", true)->value = 0;

    Model model;
    const auto add_scope = [&model](const std::string &name, int filament, FeatureProcessPolicy policy) {
        ModelObject *object = model.add_object(name.c_str(), "", make_cube(20., 20., 20.));
        object->add_instance();
        object->config.set("outer_wall_filament_id", filament);
        object->config.set_key_value("wall_process_policy", new ConfigOptionEnum<FeatureProcessPolicy>(policy));
        object->config.set("wall_process_projection", std::string("outer_wall_speed=42"));
        object->config.set("wall_layer_height", 0.1);
        return object;
    };

    ModelObject *automatic = add_scope("automatic", 2, FeatureProcessPolicy::AutoNozzleVariant);
    ModelObject *pinned = add_scope("pinned", 2, FeatureProcessPolicy::Pinned);
    ModelObject *different = add_scope("different", 1, FeatureProcessPolicy::AutoNozzleVariant);
    ModelObject *same_as_object = add_scope("same", 2, FeatureProcessPolicy::SameAsObject);

    ModelObject *volume_object = model.add_object("volume", "", make_cube(20., 20., 20.));
    volume_object->add_instance();
    volume_object->config.set("extruder", 2);
    ModelVolume *volume = volume_object->volumes.front();
    volume->config.set("wall_process_projection", std::string("outer_wall_speed=42"));
    volume->config.set("wall_layer_height", 0.1);

    REQUIRE(clear_auto_feature_process_state_for_filament(model, 2, full_config));
    CHECK_FALSE(automatic->config.has("wall_process_projection"));
    CHECK_FALSE(automatic->config.has("wall_layer_height"));
    CHECK_FALSE(volume->config.has("wall_process_projection"));
    CHECK_FALSE(volume->config.has("wall_layer_height"));

    for (const ModelObject *preserved : {pinned, different, same_as_object}) {
        CHECK(preserved->config.has("wall_process_projection"));
        CHECK(preserved->config.has("wall_layer_height"));
    }
    CHECK_FALSE(clear_auto_feature_process_state_for_filament(model, 2, full_config));
}

TEST_CASE("Projection updater distinguishes default and feature tools through either mixed-nozzle map",
          "[FeatureProcessResolver][Regression]")
{
    const bool fine_nozzle_is_first = GENERATE(false, true);
    ResolverFixture fixture;
    fixture.printer_config.option<ConfigOptionFloats>("nozzle_diameter", true)->values =
        fine_nozzle_is_first ? std::vector<double>{0.2, 0.4} : std::vector<double>{0.4, 0.2};
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(fixture.printer_config);
    full_config.option<ConfigOptionInts>("filament_map", true)->values =
        fine_nozzle_is_first ? std::vector<int>{2, 1} : std::vector<int>{1, 2};
    full_config.option<ConfigOptionFloat>("layer_height", true)->value = 0.2;
    full_config.option<ConfigOptionInt>("extruder", true)->value = 0;
    full_config.option<ConfigOptionString>("print_settings_id", true)->value = ResolverFixture::standard_04;
    full_config.option<ConfigOptionInt>("wall_loops", true)->value = 2;
    full_config.option<ConfigOptionInt>("outer_wall_filament_id", true)->value = 2;

    Model model;
    ModelObject *object = model.add_object("cube", "", make_cube(20., 20., 20.));
    object->add_instance();

    DYNAMIC_SECTION("fine nozzle on tool " << (fine_nozzle_is_first ? 0 : 1)) {
        REQUIRE(update_feature_process_projections(model, fixture.bundle, full_config));
        const auto *projection = dynamic_cast<const ConfigOptionString *>(object->config.option("wall_process_projection"));
        REQUIRE(projection != nullptr);
        CHECK_FALSE(projection->value.empty());
        CHECK(projection->value.find("wall_layer_height=0.1") != std::string::npos);
        CHECK_FALSE(object->config.has("wall_layer_height"));
    }
}
