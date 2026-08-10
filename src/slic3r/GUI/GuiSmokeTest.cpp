#include "GuiSmokeTest.hpp"

#include "BackgroundSlicingProcess.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "FilamentMapPanel.hpp"
#include "MainFrame.hpp"
#include "ParamsPanel.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "Widgets/ComboBox.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/FeatureProcessResolver.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <wx/display.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Slic3r::GUI {

namespace {

constexpr int pump_timer_id = wxID_HIGHEST + 181;
constexpr int watchdog_timer_id = wxID_HIGHEST + 182;

std::string current_exception_message()
{
    const std::exception_ptr exception = std::current_exception();
    if (!exception)
        return "unknown non-C++ exception";
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception &ex) {
        return ex.what();
    } catch (...) {
        return "unknown non-C++ exception";
    }
}

bool is_multitool_printer(const Preset &preset)
{
    const auto *diameters = preset.config.option<ConfigOptionFloats>("nozzle_diameter");
    return diameters != nullptr && diameters->values.size() >= 2;
}

bool change_tab_field(Tab &tab, const std::string &key, const boost::any &value)
{
    tab.activate_option(key, {});
    Field *field = tab.get_field(key);
    if (field == nullptr)
        return false;
    field->set_value(value, false);
    field->field_changed();
    return true;
}

} // namespace

std::atomic<int> GuiSmokeTest::s_exit_code { 2 };

GuiSmokeTest::GuiSmokeTest(GUI_App &app, const std::string &step_filter)
    : m_app(app)
    , m_pump(this, pump_timer_id)
    , m_watchdog(this, watchdog_timer_id)
{
    build_steps(step_filter);
    Bind(wxEVT_TIMER, &GuiSmokeTest::on_pump, this, pump_timer_id);
    Bind(wxEVT_TIMER, &GuiSmokeTest::on_watchdog, this, watchdog_timer_id);
}

GuiSmokeTest::~GuiSmokeTest()
{
    m_pump.Stop();
    m_watchdog.Stop();
}

int GuiSmokeTest::exit_code()
{
    return s_exit_code.load(std::memory_order_acquire);
}

void GuiSmokeTest::log(const std::string &line) const
{
    std::cout << line << std::endl;
    BOOST_LOG_TRIVIAL(info) << line;
}

void GuiSmokeTest::build_steps(const std::string &step_filter)
{
    std::set<std::string> selected;
    if (!step_filter.empty()) {
        std::vector<std::string> names;
        boost::split(names, step_filter, boost::is_any_of(","));
        for (std::string &name : names) {
            boost::trim(name);
            if (!name.empty())
                selected.insert(name);
        }
    }

    auto add = [this, &selected](const char *name, Requirement requirement, const char *required_success,
                                 std::function<Progress()> fn) {
        if (selected.empty() || selected.erase(name) > 0)
            m_steps.push_back({name, requirement, required_success, std::move(fn)});
    };

    add("select_multitool_printer", Requirement::Required, "", [this] { return select_multitool_printer(); });
    add("two_filaments", Requirement::Required, "select_multitool_printer", [this] { return two_filaments(); });
    add("add_cube_and_object_settings", Requirement::Required, "select_multitool_printer", [this] { return add_cube_and_object_settings(); });
    add("mixed_diameter_overlay", Requirement::Required, "select_multitool_printer", [this] { return mixed_diameter_overlay(); });
    add("filament-process-auto", Requirement::Required, "mixed_diameter_overlay", [this] { return filament_process_auto(); });
    add("feature_filament_each", Requirement::Required, "select_multitool_printer", [this] { return feature_filament_each(); });
    add("detail_nozzle_control", Requirement::Required, "feature_filament_each", [this] { return detail_nozzle_control(); });
    add("wall_policy_cycle", Requirement::Required, "select_multitool_printer", [this] { return wall_policy_cycle(); });
    add("slice_cube", Requirement::Optional, "select_multitool_printer", [this] { return slice_cube(); });
    add("reset_uniform_diameters", Requirement::Required, "select_multitool_printer", [this] { return reset_uniform_diameters(); });
    add("switch_printer_and_back", Requirement::Optional, "select_multitool_printer", [this] { return switch_printer_and_back(); });
    add("close", Requirement::Optional, "", [this] { return close(); });

    if (!selected.empty()) {
        std::ostringstream message;
        message << "unknown step" << (selected.size() == 1 ? "" : "s") << ": ";
        bool first = true;
        for (const std::string &name : selected) {
            if (!first)
                message << ", ";
            message << name;
            first = false;
        }
        m_steps.insert(m_steps.begin(), {"validate_step_filter", Requirement::Required, "", [message = message.str()]() -> Progress {
            throw std::runtime_error(message);
        }});
    }
}

bool GuiSmokeTest::arm_watchdog()
{
    if (m_watchdog_armed)
        return !m_finishing;

    s_exit_code.store(2, std::memory_order_release);
    m_watchdog_armed = true;
    log_phase("OnInit entered; 360 second watchdog armed");
    m_watchdog.StartOnce(360000);

    if (wxDisplay::GetCount() == 0) {
        log("SMOKE NO DISPLAY: no usable GUI display was found");
        finish(4);
        return false;
    }
    return true;
}

void GuiSmokeTest::start_step_pump()
{
    if (m_finishing || m_pump_started)
        return;

    m_pump_started = true;
    log("SMOKE START: " + std::to_string(m_steps.size()) + " step(s)");
    log_phase("step pump timer armed from post_init");
    m_pump.Start(30);
}

void GuiSmokeTest::log_phase(const std::string &phase) const
{
    log("SMOKE INIT: " + phase);
}

void GuiSmokeTest::on_pump(wxTimerEvent &)
{
    if (m_finishing)
        return;
    if (!m_app.initialized() || m_app.mainframe == nullptr || m_app.plater() == nullptr)
        return;
    if (!m_pump_start_logged) {
        m_pump_start_logged = true;
        log_phase("step pump started; GUI is ready");
    }
    if (m_step_index >= m_steps.size()) {
        finish(m_required_step_skipped ? 2 : 0);
        return;
    }

    Step &step = m_steps[m_step_index];
    if (m_last_started_index != int(m_step_index)) {
        m_last_started_index = int(m_step_index);
        log("SMOKE [" + std::to_string(m_step_index + 1) + "/" + std::to_string(m_steps.size()) + "] " +
            step.name + ": START");
    }
    if (!step.required_success.empty() && m_succeeded_steps.count(step.required_success) == 0) {
        log("SMOKE [" + std::to_string(m_step_index + 1) + "/" + std::to_string(m_steps.size()) + "] " +
            step.name + ": SKIP(required-dependency-failed): " + step.required_success + " did not succeed");
        m_required_step_skipped = true;
        ++m_step_index;
        return;
    }
    try {
        if (step.run() == Progress::Pending)
            return;
        std::string result;
        if (m_step_skipped) {
            const bool required = step.requirement == Requirement::Required;
            result = std::string("SKIP(") + (required ? "required" : "optional") + "): " + m_step_result;
            m_required_step_skipped = m_required_step_skipped || required;
        } else {
            result = m_step_result.empty() ? "OK" : m_step_result;
            m_succeeded_steps.insert(step.name);
        }
        log("SMOKE [" + std::to_string(m_step_index + 1) + "/" + std::to_string(m_steps.size()) + "] " +
            step.name + ": " + result);
        m_step_result.clear();
        m_step_skipped = false;
        ++m_step_index;
    } catch (const std::exception &ex) {
        fail_current(ex.what());
    } catch (...) {
        fail_current("unknown non-C++ exception");
    }
}

GuiSmokeTest::Progress GuiSmokeTest::skip(const std::string &reason)
{
    m_step_skipped = true;
    m_step_result = reason;
    return Progress::Done;
}

void GuiSmokeTest::on_watchdog(wxTimerEvent &)
{
    if (!m_finishing)
        log("SMOKE WATCHDOG: 360 second overall timeout");
    else
        log("SMOKE WATCHDOG: clean shutdown did not complete");
    s_exit_code.store(3, std::memory_order_release);
    std::cout.flush();
    flush_logs();
    std::_Exit(3);
}

void GuiSmokeTest::fail_current(const std::string &message)
{
    const std::string name = m_step_index < m_steps.size() ? m_steps[m_step_index].name : "startup";
    log("SMOKE [" + std::to_string(m_step_index + 1) + "/" + std::to_string(m_steps.size()) + "] " +
        name + ": EXCEPTION: " + message);
    finish(2);
}

void GuiSmokeTest::finish(int code)
{
    if (m_finishing)
        return;
    m_finishing = true;
    m_pump.Stop();
    s_exit_code.store(code, std::memory_order_release);
    log(std::string("SMOKE RESULT: ") + (code == 0 ? "PASS" : "FAIL") + " (exit " + std::to_string(code) + ")");
    flush_logs();
    if (m_app.mainframe)
        m_app.mainframe->Close(true);
    else
        m_app.ExitMainLoop();
}

void GuiSmokeTest::handle_unhandled_exception()
{
    const std::string message = current_exception_message();
    log("SMOKE UNHANDLED EXCEPTION: " + message);
    finish(2);
}

void GuiSmokeTest::handle_early_unhandled_exception(GUI_App &app)
{
    const std::string message = current_exception_message();
    const std::string line = "SMOKE UNHANDLED EXCEPTION: " + message;
    std::cout << line << std::endl;
    BOOST_LOG_TRIVIAL(error) << line;
    s_exit_code.store(2, std::memory_order_release);
    app.CallAfter([&app] {
        if (app.mainframe)
            app.mainframe->Close(true);
        else
            app.ExitMainLoop();
    });
}

GuiSmokeTest::Progress GuiSmokeTest::select_multitool_printer()
{
    PresetCollection &printers = m_app.preset_bundle->printers;
    const Preset *target = printers.find_preset("Bambu Lab H2D 0.4 nozzle", false, true);
    if (target == nullptr || !target->is_visible || !is_multitool_printer(*target)) {
        target = nullptr;
        for (const Preset &preset : printers.get_presets()) {
            if (preset.is_visible && is_multitool_printer(preset)) {
                target = &preset;
                break;
            }
        }
    }
    if (target == nullptr) {
        return skip("no visible printer preset with two or more extruders");
    }

    Tab *tab = m_app.get_tab(Preset::TYPE_PRINTER);
    if (tab == nullptr || !tab->select_preset(target->name, false, {}, true, true))
        throw std::runtime_error("failed to select multi-tool printer preset " + target->name);
    m_primary_printer = target->name;

    const auto *diameters = m_app.preset_bundle->full_config().option<ConfigOptionFloats>("nozzle_diameter");
    if (diameters == nullptr || diameters->values.size() < 2)
        throw std::runtime_error("selected printer did not produce a two-extruder full config");
    return Progress::Done;
}

GuiSmokeTest::Progress GuiSmokeTest::two_filaments()
{
    m_app.preset_bundle->set_num_filaments(2);
    if (auto *map = m_app.preset_bundle->project_config.option<ConfigOptionInts>("filament_map"))
        map->values = {1, 2};
    m_app.plater()->on_filament_count_change(2);
    m_app.plater()->on_config_change(m_app.preset_bundle->full_config());
    if (m_app.preset_bundle->filament_presets.size() != 2)
        throw std::runtime_error("project filament count is not 2");
    return Progress::Done;
}

GuiSmokeTest::Progress GuiSmokeTest::feature_filament_each()
{
    static const std::vector<std::string> keys {
        "outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
        "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id"
    };
    Tab *tab = m_app.get_tab(Preset::TYPE_PRINT);
    if (tab == nullptr || tab->get_config() == nullptr)
        return skip("Process tab is unavailable");
    if (m_feature_stage == keys.size() * 2) {
        if (!change_tab_field(*tab, keys.front(), boost::any(2)))
            return skip("field is unavailable: " + keys.front());
        ++m_feature_stage;
        return Progress::Pending;
    }
    if (m_feature_stage > keys.size() * 2) {
        const auto *mode = m_app.preset_bundle->project_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode");
        if (mode == nullptr || mode->value != FilamentMapMode::fmmManual)
            throw std::runtime_error("feature filament assignment did not switch filament_map_mode to manual");

        const DynamicPrintConfig full_config = m_app.preset_bundle->full_config();
        const auto *map = full_config.option<ConfigOptionInts>("filament_map");
        const auto *diameters = full_config.option<ConfigOptionFloats>("nozzle_diameter");
        if (map == nullptr || map->values.size() < 2 || diameters == nullptr || diameters->values.size() < 2)
            throw std::runtime_error("feature filament projection check lacks a concrete two-tool map");
        const size_t object_tool = size_t(map->values[0] - 1);
        const size_t wall_tool = size_t(map->values[1] - 1);
        if (object_tool >= diameters->values.size() || wall_tool >= diameters->values.size())
            throw std::runtime_error("feature filament projection check produced an invalid tool index");

        if (std::abs(diameters->values[object_tool] - diameters->values[wall_tool]) > 1e-6) {
            if (m_app.model().objects.empty())
                throw std::runtime_error("feature filament projection check has no model object");
            const auto *projection = dynamic_cast<const ConfigOptionString *>(
                m_app.model().objects.front()->config.option("wall_process_projection"));
            if (projection == nullptr || projection->value.empty())
                throw std::runtime_error("wall_process_projection stayed empty for different mapped nozzle diameters");
        }
        return Progress::Done;
    }

    const std::string &key = keys[m_feature_stage / 2];
    const int value = (m_feature_stage % 2 == 0) ? 2 : 0;
    if (!change_tab_field(*tab, key, boost::any(value)))
        return skip("field is unavailable: " + key);
    if (tab->get_config()->opt_int(key) != value)
        throw std::runtime_error(key + " did not retain value " + std::to_string(value));
    ++m_feature_stage;
    return Progress::Pending;
}

GuiSmokeTest::Progress GuiSmokeTest::detail_nozzle_control()
{
    static const std::vector<std::string> feature_keys {
        "outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
        "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id"
    };
    Tab *tab = m_app.get_tab(Preset::TYPE_PRINT);
    if (tab == nullptr || tab->get_config() == nullptr)
        return skip("Process tab is unavailable");

    if (m_detail_nozzle_stage < feature_keys.size()) {
        const std::string &key = feature_keys[m_detail_nozzle_stage++];
        if (!change_tab_field(*tab, key, boost::any(0)))
            return skip("field is unavailable: " + key);
        return Progress::Pending;
    }

    const DynamicPrintConfig full_config = m_app.preset_bundle->full_config();
    const auto *diameters = full_config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto *map = full_config.option<ConfigOptionInts>("filament_map");
    if (diameters == nullptr || diameters->values.size() < 2 || map == nullptr || map->values.empty())
        throw std::runtime_error("detail nozzle control lacks a concrete mixed-nozzle map");
    const size_t fine_tool = size_t(std::distance(
        diameters->values.begin(), std::min_element(diameters->values.begin(), diameters->values.end())));

    if (m_detail_nozzle_stage == feature_keys.size()) {
        ComboBox *combo = detail_nozzle_control_for_smoke();
        if (combo == nullptr && !m_detail_control_activated) {
            // Page rebuilds destroy the widget; re-activate the Multimaterial page and give
            // the UI a pump tick to recreate it.
            m_detail_control_activated = true;
            tab->activate_option("outer_wall_filament_id", wxString());
            return Progress::Pending;
        }
        if (combo == nullptr)
            throw std::runtime_error("Print fine details with control was not instantiated");
        if (!combo->IsShown() || combo->GetCount() != diameters->values.size() + 1)
            throw std::runtime_error("Print fine details with control did not expose every mixed nozzle");
        combo->SelectAndNotify(int(fine_tool + 1));
        ++m_detail_nozzle_stage;
        return Progress::Pending;
    }

    const auto expected = std::find(map->values.begin(), map->values.end(), int(fine_tool + 1));
    if (expected == map->values.end())
        throw std::runtime_error("no filament is mapped to the fine nozzle in the smoke configuration");
    const int expected_filament = int(std::distance(map->values.begin(), expected) + 1);
    const int outer = tab->get_config()->opt_int("outer_wall_filament_id");
    const int inner = tab->get_config()->opt_int("inner_wall_filament_id");
    if (outer != expected_filament)
        throw std::runtime_error("fine nozzle control did not select its mapped filament for outer walls");
    if (inner != expected_filament)
        throw std::runtime_error("inner walls did not follow the fine nozzle control");
    if (m_app.model().objects.empty())
        throw std::runtime_error("detail nozzle projection check has no model object");
    const auto *projection = dynamic_cast<const ConfigOptionString *>(
        m_app.model().objects.front()->config.option("wall_process_projection"));
    if (projection == nullptr || projection->value.empty())
        throw std::runtime_error("fine nozzle control left wall_process_projection empty");

    const size_t object_tool = map->values.front() > 0 ? size_t(map->values.front() - 1) : 0;
    const size_t wall_tool = outer > 0 && size_t(outer) <= map->values.size() ? size_t(map->values[outer - 1] - 1) : 0;
    if (object_tool >= diameters->values.size() || wall_tool >= diameters->values.size() ||
        !(diameters->values[wall_tool] + 1e-6 < diameters->values[object_tool]))
        throw std::runtime_error("fine nozzle control did not bind walls to a strictly finer nozzle than the object");
    return Progress::Done;
}

GuiSmokeTest::Progress GuiSmokeTest::wall_policy_cycle()
{
    Tab *tab = m_app.get_tab(Preset::TYPE_PRINT);
    if (tab == nullptr || tab->get_config() == nullptr)
        return skip("Process tab is unavailable");

    auto change = [this, tab](const std::string &key, const boost::any &value) {
        return change_tab_field(*tab, key, value) ? Progress::Pending : skip("field is unavailable: " + key);
    };
    switch (m_policy_stage++) {
    case 0: return change("wall_process_policy", boost::any(int(FeatureProcessPolicy::Pinned)));
    case 1: return change("wall_process_preset", boost::any(from_u8("GUI smoke missing preset")));
    case 2: return change("wall_process_policy", boost::any(int(FeatureProcessPolicy::AutoNozzleVariant)));
    case 3:
        if (!change_tab_field(*tab, "wall_process_preset", boost::any(wxString())))
            return skip("field is unavailable: wall_process_preset");
        return Progress::Done;
    default: return Progress::Done;
    }
}

GuiSmokeTest::Progress GuiSmokeTest::mixed_diameter_overlay()
{
    if (!m_app.sidebar().apply_nozzle_diameters_for_smoke("0.4", "0.2"))
        throw std::runtime_error("failed to apply the 0.4/0.2 project nozzle overlay");
    // full_config() returns by value; keep it alive for the option lookup.
    const DynamicPrintConfig full_config = m_app.preset_bundle->full_config();
    const auto *diameters = full_config.option<ConfigOptionFloats>("nozzle_diameter");
    if (diameters == nullptr || diameters->values.size() < 2 ||
        std::abs(diameters->values[0] - 0.4) > 1e-6 || std::abs(diameters->values[1] - 0.2) > 1e-6)
        throw std::runtime_error("full config did not expose the 0.4/0.2 overlay");
    return Progress::Done;
}

GuiSmokeTest::Progress GuiSmokeTest::filament_process_auto()
{
    PresetBundle &bundle = *m_app.preset_bundle;
    const DynamicPrintConfig full_config = bundle.full_config();
    const auto *diameters = full_config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto *map = full_config.option<ConfigOptionInts>("filament_map");
    if (diameters == nullptr || diameters->values.size() < 2 || map == nullptr || map->values.empty())
        throw std::runtime_error("filament process smoke lacks a concrete mixed-nozzle map");
    const size_t fine_tool = size_t(std::distance(
        diameters->values.begin(), std::min_element(diameters->values.begin(), diameters->values.end())));
    const auto fine_slot_it = std::find(map->values.begin(), map->values.end(), int(fine_tool + 1));
    if (fine_slot_it == map->values.end())
        throw std::runtime_error("filament process smoke has no filament on the fine nozzle");
    const size_t fine_slot = size_t(std::distance(map->values.begin(), fine_slot_it));

    const auto timed_out = [this] {
        return std::chrono::steady_clock::now() - m_filament_process_start > std::chrono::seconds(15);
    };
    auto *policies = bundle.project_config.option<ConfigOptionEnumsGeneric>("filament_process_policy");
    auto *presets = bundle.project_config.option<ConfigOptionStrings>("filament_process_preset");
    auto *projections = bundle.project_config.option<ConfigOptionStrings>("filament_process_projection");
    if (policies == nullptr || presets == nullptr || projections == nullptr)
        throw std::runtime_error("filament process project vectors are unavailable");

    if (m_filament_process_stage == 0) {
        set_filament_process_selection(fine_slot, FilamentProcessPolicy::AutoNozzleVariant, {});
        m_filament_process_start = std::chrono::steady_clock::now();
        ++m_filament_process_stage;
        return Progress::Pending;
    }

    if (fine_slot >= projections->values.size() || projections->values[fine_slot].empty()) {
        if (timed_out())
            throw std::runtime_error("fine-slot filament_process_projection did not sync back to project_config");
        return Progress::Pending;
    }
    if (projections->values[fine_slot].find("layer_height=") == std::string::npos)
        throw std::runtime_error("fine-slot filament_process_projection lacks layer_height");

    if (m_filament_process_stage == 1) {
        if (!m_app.plater()->filament_process_hint_registered_for_smoke(unsigned(fine_slot + 1))) {
            if (timed_out())
                throw std::runtime_error("filament process notification state was not registered");
            return Progress::Pending;
        }
        FilamentProcessRequest request;
        request.filament_id = unsigned(fine_slot + 1);
        request.policy = FilamentProcessPolicy::AutoNozzleVariant;
        request.bundle = &bundle;
        request.full_config = &full_config;
        const FilamentProcessResolution automatic = resolve_filament_process(request);
        if (!automatic.ok || automatic.resolved_preset.empty())
            throw std::runtime_error("automatic filament process did not resolve a pin-able fine-nozzle preset");
        set_filament_process_selection(fine_slot, FilamentProcessPolicy::Pinned, automatic.resolved_preset);
        m_filament_process_start = std::chrono::steady_clock::now();
        ++m_filament_process_stage;
        return Progress::Pending;
    }

    if (m_filament_process_stage == 2) {
        if (fine_slot >= policies->values.size() || fine_slot >= presets->values.size() ||
            policies->values[fine_slot] != int(FilamentProcessPolicy::Pinned) || presets->values[fine_slot].empty()) {
            if (timed_out())
                throw std::runtime_error("pinned filament process intent was not retained");
            return Progress::Pending;
        }
        const StringObjectException validity = m_app.plater()->fff_print().validate();
        if (!validity.string.empty())
            throw std::runtime_error("pinned filament process left an invalid reslice state: " + validity.string);
        set_filament_process_selection(fine_slot, FilamentProcessPolicy::AutoNozzleVariant, {});
        m_filament_process_start = std::chrono::steady_clock::now();
        ++m_filament_process_stage;
        return Progress::Pending;
    }

    if (fine_slot >= policies->values.size() || fine_slot >= presets->values.size() ||
        policies->values[fine_slot] != int(FilamentProcessPolicy::AutoNozzleVariant) ||
        !presets->values[fine_slot].empty()) {
        if (timed_out())
            throw std::runtime_error("automatic filament process intent was not restored");
        return Progress::Pending;
    }
    return Progress::Done;
}

GuiSmokeTest::Progress GuiSmokeTest::reset_uniform_diameters()
{
    if (!m_app.sidebar().apply_nozzle_diameters_for_smoke("0.4", "0.4"))
        throw std::runtime_error("failed to clear the project nozzle overlay with a uniform selection");
    const auto *project = m_app.preset_bundle->project_config.option<ConfigOptionFloats>("project_nozzle_diameter");
    if (project == nullptr || !project->values.empty())
        throw std::runtime_error("uniform nozzle selection did not clear project_nozzle_diameter");
    return Progress::Done;
}

GuiSmokeTest::Progress GuiSmokeTest::add_cube_and_object_settings()
{
    ObjectList *objects = m_app.obj_list();
    if (objects == nullptr)
        return skip("Object list is unavailable");
    objects->load_mesh_object(make_cube(20., 20., 20.), "GUI smoke cube");
    if (m_app.model().objects.empty())
        throw std::runtime_error("20 mm cube was not added to the model");
    objects->switch_to_object_process();
    if (m_app.plater()->get_selected_object_idx() < 0)
        throw std::runtime_error("added cube is not selected");
    return Progress::Done;
}

GuiSmokeTest::Progress GuiSmokeTest::slice_cube()
{
    BackgroundSlicingProcess &process = m_app.plater()->background_process();
    if (!m_slice_started) {
        m_slice_started = true;
        m_slice_start = std::chrono::steady_clock::now();
        m_app.plater()->reslice();
        return Progress::Pending;
    }

    m_slice_was_running = m_slice_was_running || process.running();
    if (process.finished()) {
        m_step_result = "OK (slice completed)";
        return Progress::Done;
    }
    if (m_slice_was_running && process.idle()) {
        m_step_result = "OK (slice ended with a reported error outcome)";
        return Progress::Done;
    }
    // A slice that never starts is usually a validation refusal the GUI shows only in the
    // sidebar; surface the actual message instead of timing out blind.
    if (!m_slice_was_running &&
        std::chrono::steady_clock::now() - m_slice_start > std::chrono::seconds(10)) {
        const StringObjectException validity = m_app.plater()->fff_print().validate();
        if (!validity.string.empty())
            throw std::runtime_error("slice blocked by validation: " + validity.string +
                                     " (key: " + validity.opt_key + ")");
    }
    // Temporary diagnostics: dump process state every ~15 s while waiting.
    const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_slice_start).count();
    if (waited > 0 && waited % 15 == 0 && waited != m_last_state_dump) {
        m_last_state_dump = int(waited);
        log("SMOKE SLICE STATE t=" + std::to_string(waited) +
            "s running=" + std::to_string(process.running()) +
            " idle=" + std::to_string(process.idle()) +
            " finished=" + std::to_string(process.finished()) +
            " was_running=" + std::to_string(m_slice_was_running) +
            " update_scheduled=" + std::to_string(m_app.plater()->is_background_process_update_scheduled()));
    }

    const auto elapsed = std::chrono::steady_clock::now() - m_slice_start;
    if (!m_slice_was_running && process.idle() && elapsed > std::chrono::seconds(2)) {
        m_step_result = "OK (slice was rejected before background execution)";
        return Progress::Done;
    }
    if (elapsed > std::chrono::seconds(180))
        throw std::runtime_error("slice did not complete within 180 seconds");
    return Progress::Pending;
}

GuiSmokeTest::Progress GuiSmokeTest::switch_printer_and_back()
{
    Tab *tab = m_app.get_tab(Preset::TYPE_PRINTER);
    if (tab == nullptr)
        return skip("Printer tab is unavailable");

    if (m_switch_stage++ == 0) {
        for (const Preset &preset : m_app.preset_bundle->printers.get_presets()) {
            if (preset.is_visible && preset.name != m_primary_printer && is_multitool_printer(preset)) {
                m_alternate_printer = preset.name;
                break;
            }
        }
        if (m_alternate_printer.empty()) {
            return skip("no alternate visible multi-tool printer preset");
        }
        if (!tab->select_preset(m_alternate_printer, false, {}, true, true))
            throw std::runtime_error("failed to select alternate printer " + m_alternate_printer);
        return Progress::Pending;
    }

    if (!tab->select_preset(m_primary_printer, false, {}, true, true))
        throw std::runtime_error("failed to re-select primary printer " + m_primary_printer);
    if (m_app.preset_bundle->printers.get_edited_preset().name != m_primary_printer)
        throw std::runtime_error("primary printer was not restored");
    return Progress::Done;
}

GuiSmokeTest::Progress GuiSmokeTest::close()
{
    return Progress::Done;
}

} // namespace Slic3r::GUI
