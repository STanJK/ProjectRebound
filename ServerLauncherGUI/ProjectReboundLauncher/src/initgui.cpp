// ======================================================
//  GUI MODE – SLINT FRONTEND FOR SERVER WRAPPER
// ======================================================
#include "app-window.h"
#include "headers/wrapper.h"
#include <thread>
#include <slint.h>
#include <deque>
#include <mutex>
#include <string>
#include <sstream>
#include <atomic>
#include <windows.h>

// ---------- UI log queue (thread‑safe) ----------
slint::ComponentWeakHandle<AppWindow> g_ui;

constexpr size_t MAX_LOG_LINES = 100;
std::deque<std::string> g_log_lines;
std::mutex g_log_mutex;
std::atomic<int> g_total_log_length{0};

void PushToUILog(const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_lines.push_back(text);
    if (g_log_lines.size() > MAX_LOG_LINES)
        g_log_lines.pop_front();

    int total = 0;
    for (const auto& line : g_log_lines)
        total += line.size();
    g_total_log_length.store(total);
}

std::string get_recent_logs()
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ostringstream oss;
    for (const auto& line : g_log_lines)
        oss << line;
    return oss.str();
}

int get_log_length()
{
    return g_total_log_length.load();
}

// callback that feeds wrapper logs into the UI queue
void WrapperLogCallback(const std::string& msg)
{
    PushToUILog(msg);
}

// ---------- GUI entry ----------
void InitGUI()
{
    // prevent dxgi proxy hijacking
    SetDllDirectoryW(L"");

    auto ui = AppWindow::create();
    g_ui = ui;

    ui->on_get_new_log([&]() -> slint::SharedString {
        return slint::SharedString(get_recent_logs());
    });
    ui->on_get_log_length([&]() -> int {
        return get_log_length();
    });
    ui->on_send_command([&](slint::SharedString cmd) {
        ExecuteConsoleCommand(std::string(cmd));
    });

    // button that starts the server
    ui->on_start_server([&]() {
        static bool started = false;
        if (started) return;
        started = true;

        SetExternalLogCallback(WrapperLogCallback);

        std::thread([]() {
            InitWrapperCore();    // load config, init commands, log file
            // note: we deliberately do NOT start console input thread
            RunWrapperLoop();     // launch server, wait for shutdown
        }).detach();
    });

    ui->run();

    // window closed → trigger graceful shutdown
    g_WrapperShuttingDown.store(true);
    std::this_thread::sleep_for(std::chrono::seconds(2));
}