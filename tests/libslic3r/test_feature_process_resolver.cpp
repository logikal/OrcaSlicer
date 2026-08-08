#include <catch2/catch_all.hpp>

#include "libslic3r/FeatureProcessResolver.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
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
    CHECK(result.projection.option("wall_loops") == nullptr);

    const auto *speed = result.projection.option<ConfigOptionFloats>("outer_wall_speed");
    REQUIRE(speed != nullptr);
    REQUIRE(speed->values.size() == 1);
    CHECK_THAT(speed->values.front(), Catch::Matchers::WithinAbs(42., 1e-9));
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
