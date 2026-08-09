#include "GuiSmokeTest.hpp"

#include "BackgroundSlicingProcess.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "MainFrame.hpp"
#include "ParamsPanel.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <wx/display.h>

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

void change_tab_field(Tab &tab, const std::string &key, const boost::any &value)
{
    tab.activate_option(key, {});
    Field *field = tab.get_field(key);
    if (field == nullptr)
        throw std::runtime_error("field is unavailable: " + key);
    field->set_value(value, false);
    field->field_changed();
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

    auto add = [this, &selected](const char *name, std::function<Progress()> fn) {
        if (selected.empty() || selected.erase(name) > 0)
            m_steps.push_back({name, std::move(fn)});
    };

    add("select_multitool_printer", [this] { return select_multitool_printer(); });
    add("two_filaments", [this] { return two_filaments(); });
    add("feature_filament_each", [this] { return feature_filament_each(); });
    add("wall_policy_cycle", [this] { return wall_policy_cycle(); });
    add("mixed_diameter_overlay", [this] { return mixed_diameter_overlay(); });
    add("add_cube_and_object_settings", [this] { return add_cube_and_object_settings(); });
    add("slice_cube", [this] { return slice_cube(); });
    add("switch_printer_and_back", [this] { return switch_printer_and_back(); });
    add("close", [this] { return close(); });

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
        m_steps.insert(m_steps.begin(), {"validate_step_filter", [message = message.str()]() -> Progress {
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
    log_phase("OnInit entered; 120 second watchdog armed");
    m_watchdog.StartOnce(120000);

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
        finish(0);
        return;
    }

    Step &step = m_steps[m_step_index];
    try {
        if (step.run() == Progress::Pending)
            return;
        const std::string result = m_step_result.empty() ? "OK" : m_step_result;
        log("SMOKE [" + std::to_string(m_step_index + 1) + "/" + std::to_string(m_steps.size()) + "] " +
            step.name + ": " + result);
        m_step_result.clear();
        ++m_step_index;
    } catch (const std::exception &ex) {
        fail_current(ex.what());
    } catch (...) {
        fail_current("unknown non-C++ exception");
    }
}

void GuiSmokeTest::on_watchdog(wxTimerEvent &)
{
    if (!m_finishing)
        log("SMOKE WATCHDOG: 120 second overall timeout");
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
        m_step_result = "SKIP: no visible printer preset with two or more extruders";
        return Progress::Done;
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
    if (m_primary_printer.empty()) {
        m_step_result = "SKIP: multi-tool printer unavailable";
        return Progress::Done;
    }
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
    if (m_primary_printer.empty()) {
        m_step_result = "SKIP: multi-tool printer unavailable";
        return Progress::Done;
    }

    Tab *tab = m_app.get_tab(Preset::TYPE_PRINT);
    if (tab == nullptr || tab->get_config() == nullptr)
        throw std::runtime_error("Process tab is unavailable");
    if (m_feature_stage >= keys.size() * 2)
        return Progress::Done;

    const std::string &key = keys[m_feature_stage / 2];
    const int value = (m_feature_stage % 2 == 0) ? 2 : 0;
    change_tab_field(*tab, key, boost::any(value));
    if (tab->get_config()->opt_int(key) != value)
        throw std::runtime_error(key + " did not retain value " + std::to_string(value));
    ++m_feature_stage;
    return m_feature_stage == keys.size() * 2 ? Progress::Done : Progress::Pending;
}

GuiSmokeTest::Progress GuiSmokeTest::wall_policy_cycle()
{
    Tab *tab = m_app.get_tab(Preset::TYPE_PRINT);
    if (tab == nullptr || tab->get_config() == nullptr)
        throw std::runtime_error("Process tab is unavailable");

    auto change = [tab](const std::string &key, const boost::any &value) {
        change_tab_field(*tab, key, value);
    };
    switch (m_policy_stage++) {
    case 0: change("wall_process_policy", boost::any(int(FeatureProcessPolicy::Pinned))); return Progress::Pending;
    case 1: change("wall_process_preset", boost::any(from_u8("GUI smoke missing preset"))); return Progress::Pending;
    case 2: change("wall_process_policy", boost::any(int(FeatureProcessPolicy::AutoNozzleVariant))); return Progress::Pending;
    case 3: change("wall_process_preset", boost::any(wxString())); return Progress::Done;
    default: return Progress::Done;
    }
}

GuiSmokeTest::Progress GuiSmokeTest::mixed_diameter_overlay()
{
    if (m_primary_printer.empty()) {
        m_step_result = "SKIP: multi-tool printer unavailable";
        return Progress::Done;
    }
    if (m_overlay_stage++ == 0) {
        if (!m_app.sidebar().apply_nozzle_diameters_for_smoke("0.4", "0.2"))
            throw std::runtime_error("failed to apply the 0.4/0.2 project nozzle overlay");
        const auto *diameters = m_app.preset_bundle->full_config().option<ConfigOptionFloats>("nozzle_diameter");
        if (diameters == nullptr || diameters->values.size() < 2 ||
            std::abs(diameters->values[0] - 0.4) > 1e-6 || std::abs(diameters->values[1] - 0.2) > 1e-6)
            throw std::runtime_error("full config did not expose the 0.4/0.2 overlay");
        return Progress::Pending;
    }

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
        throw std::runtime_error("Object list is unavailable");
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

    const auto elapsed = std::chrono::steady_clock::now() - m_slice_start;
    if (!m_slice_was_running && process.idle() && elapsed > std::chrono::seconds(2)) {
        m_step_result = "OK (slice was rejected before background execution)";
        return Progress::Done;
    }
    if (elapsed > std::chrono::seconds(60))
        throw std::runtime_error("slice did not complete within 60 seconds");
    return Progress::Pending;
}

GuiSmokeTest::Progress GuiSmokeTest::switch_printer_and_back()
{
    if (m_primary_printer.empty()) {
        m_step_result = "SKIP: multi-tool printer unavailable";
        return Progress::Done;
    }
    Tab *tab = m_app.get_tab(Preset::TYPE_PRINTER);
    if (tab == nullptr)
        throw std::runtime_error("Printer tab is unavailable");

    if (m_switch_stage++ == 0) {
        for (const Preset &preset : m_app.preset_bundle->printers.get_presets()) {
            if (preset.is_visible && preset.name != m_primary_printer && is_multitool_printer(preset)) {
                m_alternate_printer = preset.name;
                break;
            }
        }
        if (m_alternate_printer.empty()) {
            m_step_result = "SKIP: no alternate visible multi-tool printer preset";
            return Progress::Done;
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
