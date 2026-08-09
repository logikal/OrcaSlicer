#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/timer.h>

namespace Slic3r::GUI {

class GUI_App;

class GuiSmokeTest final : public wxEvtHandler
{
public:
    GuiSmokeTest(GUI_App &app, const std::string &step_filter);
    ~GuiSmokeTest() override;

    bool arm_watchdog();
    void start_step_pump();
    void log_phase(const std::string &phase) const;
    void handle_unhandled_exception();

    static void handle_early_unhandled_exception(GUI_App &app);
    static int exit_code();

private:
    enum class Progress { Pending, Done };

    struct Step {
        std::string name;
        std::function<Progress()> run;
    };

    void build_steps(const std::string &step_filter);
    void on_pump(wxTimerEvent &);
    void on_watchdog(wxTimerEvent &);
    void finish(int code);
    void fail_current(const std::string &message);
    void log(const std::string &line) const;

    Progress select_multitool_printer();
    Progress two_filaments();
    Progress feature_filament_each();
    Progress wall_policy_cycle();
    Progress mixed_diameter_overlay();
    Progress add_cube_and_object_settings();
    Progress slice_cube();
    Progress switch_printer_and_back();
    Progress close();

    GUI_App &m_app;
    wxTimer m_pump;
    wxTimer m_watchdog;
    std::vector<Step> m_steps;
    size_t m_step_index { 0 };
    std::string m_step_result;
    std::string m_primary_printer;
    std::string m_alternate_printer;
    size_t m_feature_stage { 0 };
    size_t m_policy_stage { 0 };
    size_t m_overlay_stage { 0 };
    size_t m_switch_stage { 0 };
    bool m_slice_started { false };
    bool m_slice_was_running { false };
    bool m_finishing { false };
    bool m_watchdog_armed { false };
    bool m_pump_started { false };
    bool m_pump_start_logged { false };
    std::chrono::steady_clock::time_point m_slice_start;

    static std::atomic<int> s_exit_code;
};

} // namespace Slic3r::GUI
