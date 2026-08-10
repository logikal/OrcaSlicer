#include <catch2/catch_all.hpp>

#include "libslic3r/FeatureProcessResolver.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <array>
#include <string>

using namespace Slic3r;

namespace {

class FilamentResolverFixture
{
public:
    static constexpr const char *printer_04 = "TestPrinter 0.4 nozzle";
    static constexpr const char *printer_02 = "TestPrinter 0.2 nozzle";
    static constexpr const char *standard_04 = "0.20mm Standard @TestPrinter 0.4 nozzle";
    static constexpr const char *standard_04_fine = "0.16mm Standard @TestPrinter 0.4 nozzle";
    static constexpr const char *standard_02 = "0.10mm Standard @TestPrinter 0.2 nozzle";
    static constexpr const char *quality_02 = "0.10mm High Quality @TestPrinter 0.2 nozzle";

    PresetBundle       bundle;
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();

    FilamentResolverFixture()
    {
        add_printer(printer_02, 0.2, false);
        add_printer(printer_04, 0.4, true);
        add_process(standard_04, 0.2, 0.42, printer_04);
        // Same family, absolute height nearer to the fine 0.10 process — a correct resolver must
        // still prefer 0.20 (preserved height/nozzle ratio 0.5) when mapping 0.10@0.2 to the 0.4 tool.
        add_process(standard_04_fine, 0.16, 0.42, printer_04);
        add_process(standard_02, 0.1, 0.22, printer_02);
        add_process(quality_02, 0.1, 0.24, printer_02);

        full_config.option<ConfigOptionString>("printer_model", true)->value = "TestPrinter";
        full_config.option<ConfigOptionFloats>("min_layer_height", true)->values = {0.07, 0.07};
        full_config.option<ConfigOptionFloats>("max_layer_height", true)->values = {0.3, 0.15};
        full_config.option<ConfigOptionFloat>("layer_height", true)->value = 0.2;
        full_config.option<ConfigOptionString>("print_settings_id", true)->value = standard_04;
        bundle.filament_presets = {"Filament 1", "Filament 2"};
        arrange_tools(false);
    }

    void arrange_tools(bool fine_nozzle_is_first)
    {
        full_config.option<ConfigOptionFloats>("nozzle_diameter", true)->values =
            fine_nozzle_is_first ? std::vector<double>{0.2, 0.4} : std::vector<double>{0.4, 0.2};
        full_config.option<ConfigOptionInts>("filament_map", true)->values =
            fine_nozzle_is_first ? std::vector<int>{2, 1} : std::vector<int>{1, 2};
        full_config.option<ConfigOptionFloats>("min_layer_height", true)->values =
            fine_nozzle_is_first ? std::vector<double>{0.07, 0.07} : std::vector<double>{0.07, 0.07};
        full_config.option<ConfigOptionFloats>("max_layer_height", true)->values =
            fine_nozzle_is_first ? std::vector<double>{0.15, 0.3} : std::vector<double>{0.3, 0.15};
    }

    FilamentProcessRequest request(unsigned int filament_id,
                                   FilamentProcessPolicy policy = FilamentProcessPolicy::AutoNozzleVariant) const
    {
        FilamentProcessRequest result;
        result.filament_id = filament_id;
        result.policy = policy;
        result.bundle = &bundle;
        result.full_config = &full_config;
        return result;
    }

    Preset &add_process(const std::string &name, double height, double line_width,
                        const std::string &compatible_printer, const std::string &condition = {})
    {
        DynamicPrintConfig config(bundle.prints.default_preset().config);
        config.option<ConfigOptionFloat>("layer_height", true)->value = height;
        config.option<ConfigOptionStrings>("compatible_printers", true)->values =
            compatible_printer.empty() ? std::vector<std::string>() : std::vector<std::string>{compatible_printer};
        config.option<ConfigOptionString>("compatible_printers_condition", true)->value = condition;
        config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(line_width, false));
        config.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(line_width + 0.01, false));
        config.option<ConfigOptionFloatsNullable>("outer_wall_speed", true)->values = {42., 99.};
        config.option<ConfigOptionInt>("wall_loops", true)->value = line_width < 0.3 ? 5 : 3;
        config.option<ConfigOptionFloat>("initial_layer_print_height", true)->value = 0.28;
        config.option<ConfigOptionFloat>("prime_tower_width", true)->value = 99.;
        config.option<ConfigOptionInt>("outer_wall_filament_id", true)->value = 2;
        config.option<ConfigOptionString>("wall_process_preset", true)->value = "must not project";
        config.option<ConfigOptionFloat>("wall_layer_height", true)->value = 0.05;
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

} // namespace

TEST_CASE("Filament process policy tokens round trip", "[FilamentProcessResolver]")
{
    DynamicPrintConfig config;
    config.set_deserialize_strict("filament_process_policy", "auto_nozzle_variant,pinned,global_process");

    const auto *policies = config.option<ConfigOptionEnumsGeneric>("filament_process_policy");
    REQUIRE(policies != nullptr);
    CHECK(policies->values == std::vector<int>{int(FilamentProcessPolicy::AutoNozzleVariant),
                                               int(FilamentProcessPolicy::Pinned),
                                               int(FilamentProcessPolicy::GlobalProcess)});
    CHECK(config.option_throw("filament_process_policy")->serialize() ==
          "auto_nozzle_variant,pinned,global_process");
}

TEST_CASE("Automatic filament process resolves the fine-nozzle Standard sibling", "[FilamentProcessResolver]")
{
    for (const bool fine_nozzle_is_first : std::array{false, true}) {
        FilamentResolverFixture fixture;
        fixture.arrange_tools(fine_nozzle_is_first);

        DYNAMIC_SECTION("fine nozzle on tool " << (fine_nozzle_is_first ? 0 : 1)) {
            const FilamentProcessResolution result = resolve_filament_process(fixture.request(2));

            REQUIRE(result.ok);
            CHECK(result.rejection == FeatureProcessRejection::None);
            CHECK(result.resolved_preset == FilamentResolverFixture::standard_02);
            CHECK(result.tool_id == (fine_nozzle_is_first ? 0 : 1));
            CHECK_THAT(result.tool_nozzle, Catch::Matchers::WithinAbs(0.2, 1e-9));
            CHECK_THAT(result.reference_nozzle, Catch::Matchers::WithinAbs(0.4, 1e-9));
            CHECK_THAT(result.layer_height, Catch::Matchers::WithinAbs(0.1, 1e-9));

            const auto *layer_height = result.delta.option<ConfigOptionFloat>("layer_height");
            const auto *outer_width = result.delta.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
            const auto *inner_width = result.delta.option<ConfigOptionFloatOrPercent>("inner_wall_line_width");
            const auto *wall_loops = result.delta.option<ConfigOptionInt>("wall_loops");
            REQUIRE(layer_height != nullptr);
            REQUIRE(outer_width != nullptr);
            REQUIRE(inner_width != nullptr);
            REQUIRE(wall_loops != nullptr);
            CHECK_THAT(layer_height->value, Catch::Matchers::WithinAbs(0.1, 1e-9));
            CHECK_THAT(outer_width->value, Catch::Matchers::WithinAbs(0.22, 1e-9));
            CHECK_THAT(inner_width->value, Catch::Matchers::WithinAbs(0.23, 1e-9));
            CHECK(wall_loops->value == 5);

            const auto *speed = result.delta.option<ConfigOptionFloatsNullable>("outer_wall_speed");
            REQUIRE(speed != nullptr);
            REQUIRE(speed->values.size() == 1);
            CHECK_THAT(speed->values.front(), Catch::Matchers::WithinAbs(42., 1e-9));
            CHECK(result.delta.option("initial_layer_print_height") == nullptr);
            CHECK(result.delta.option("prime_tower_width") == nullptr);
            CHECK(result.delta.option("outer_wall_filament_id") == nullptr);
            CHECK(result.delta.option("wall_process_preset") == nullptr);
            CHECK(result.delta.option("wall_layer_height") == nullptr);
        }
    }
}

TEST_CASE("Automatic filament process resolves the coarse-nozzle Standard sibling", "[FilamentProcessResolver]")
{
    for (const bool fine_nozzle_is_first : std::array{false, true}) {
        FilamentResolverFixture fixture;
        fixture.arrange_tools(fine_nozzle_is_first);
        fixture.full_config.option<ConfigOptionString>("print_settings_id", true)->value = FilamentResolverFixture::standard_02;
        fixture.full_config.option<ConfigOptionFloat>("layer_height", true)->value = 0.1;

        DYNAMIC_SECTION("coarse nozzle on tool " << (fine_nozzle_is_first ? 1 : 0)) {
            const FilamentProcessResolution result = resolve_filament_process(fixture.request(1));
            REQUIRE(result.ok);
            CHECK(result.resolved_preset == FilamentResolverFixture::standard_04);
            CHECK_THAT(result.tool_nozzle, Catch::Matchers::WithinAbs(0.4, 1e-9));
            CHECK_THAT(result.reference_nozzle, Catch::Matchers::WithinAbs(0.2, 1e-9));
            CHECK_THAT(result.layer_height, Catch::Matchers::WithinAbs(0.2, 1e-9));
        }
    }
}

TEST_CASE("Matching and global filament process policies preserve the global process", "[FilamentProcessResolver]")
{
    FilamentResolverFixture fixture;

    const FilamentProcessResolution matching = resolve_filament_process(fixture.request(1));
    REQUIRE(matching.ok);
    CHECK(matching.resolved_preset.empty());
    CHECK(matching.delta.empty());

    const FilamentProcessResolution global = resolve_filament_process(
        fixture.request(2, FilamentProcessPolicy::GlobalProcess));
    REQUIRE(global.ok);
    CHECK(global.resolved_preset.empty());
    CHECK(global.delta.empty());
    CHECK(global.display_label == "Use the global process");
}

TEST_CASE("Pinned filament processes resolve or preserve a missing name", "[FilamentProcessResolver]")
{
    FilamentResolverFixture fixture;
    FilamentProcessRequest request = fixture.request(2, FilamentProcessPolicy::Pinned);
    request.pinned_preset_name = FilamentResolverFixture::standard_02;

    FilamentProcessResolution result = resolve_filament_process(request);
    REQUIRE(result.ok);
    CHECK(result.resolved_preset == FilamentResolverFixture::standard_02);
    CHECK_FALSE(result.delta.empty());

    request.pinned_preset_name = "Missing process";
    result = resolve_filament_process(request);
    CHECK_FALSE(result.ok);
    CHECK(result.rejection == FeatureProcessRejection::PresetMissing);
    CHECK(result.delta.empty());
    CHECK(result.display_label == "Missing process (missing)");
}

TEST_CASE("Filament process projection refresh is idempotent", "[FilamentProcessResolver]")
{
    FilamentResolverFixture fixture;

    REQUIRE(update_filament_process_projections(fixture.bundle, fixture.full_config));
    const auto *projections = fixture.full_config.option<ConfigOptionStrings>("filament_process_projection");
    REQUIRE(projections != nullptr);
    REQUIRE(projections->values.size() == 2);
    CHECK(projections->values[0].empty());
    CHECK(projections->values[1].find("layer_height=0.1") != std::string::npos);
    CHECK_FALSE(update_filament_process_projections(fixture.bundle, fixture.full_config));
}

TEST_CASE("Filament slot resizing keeps process vectors aligned", "[FilamentProcessResolver]")
{
    PresetBundle bundle;
    bundle.filament_presets = {bundle.filaments.get_selected_preset_name()};
    bundle.set_num_filaments(3, "#FFFFFF");

    auto *policies = bundle.project_config.option<ConfigOptionEnumsGeneric>("filament_process_policy");
    auto *presets = bundle.project_config.option<ConfigOptionStrings>("filament_process_preset");
    auto *projections = bundle.project_config.option<ConfigOptionStrings>("filament_process_projection");
    REQUIRE(policies->values.size() == 3);
    REQUIRE(presets->values.size() == 3);
    REQUIRE(projections->values.size() == 3);
    CHECK(policies->values == std::vector<int>{0, 0, 0});
    CHECK(presets->values == std::vector<std::string>{"", "", ""});
    CHECK(projections->values == std::vector<std::string>{"", "", ""});

    policies->values = {int(FilamentProcessPolicy::Pinned), int(FilamentProcessPolicy::GlobalProcess),
                        int(FilamentProcessPolicy::AutoNozzleVariant)};
    presets->values = {"first", "second", "third"};
    projections->values = {"one", "two", "three"};
    bundle.project_config.option<ConfigOptionInts>("filament_map")->values = {1, 2, 1};
    bundle.update_num_filaments(1);

    CHECK(policies->values == std::vector<int>{int(FilamentProcessPolicy::Pinned),
                                               int(FilamentProcessPolicy::AutoNozzleVariant)});
    CHECK(presets->values == std::vector<std::string>{"first", "third"});
    CHECK(projections->values == std::vector<std::string>{"one", "three"});
    CHECK(bundle.project_config.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{1, 1});
}

TEST_CASE("Uninferable global process nozzle conservatively preserves the global process", "[FilamentProcessResolver]")
{
    FilamentResolverFixture fixture;
    fixture.add_process("Custom condition process", 0.15, 0.3, {}, "nozzle_diameter[0] == 0.4");
    fixture.full_config.option<ConfigOptionString>("print_settings_id", true)->value = "Custom condition process";

    const FilamentProcessResolution result = resolve_filament_process(fixture.request(2));
    REQUIRE(result.ok);
    CHECK_THAT(result.reference_nozzle, Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK(result.resolved_preset.empty());
    CHECK(result.delta.empty());
}
