#include "menu_render.h"
#include "hooks.h"
#include "shell/utils.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include "GLFW/glfw3native.h"
#include "Windows.h"
#include "menu_widget.h"

#include "breeze_ui/ui.h"
#include "shell/entry.h"
#include "shell/logger.h"
#include "shell/script/binding_types.hpp"
#include <exception>
#include <mutex>
#include <thread>

namespace mb_shell {
std::optional<menu_render *> menu_render::current{};

namespace {
int64_t ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
}
} // namespace

std::shared_ptr<ui::render_target> menu_render::render_target() {
    auto t0 = std::chrono::steady_clock::now();
    if (auto res = ui::render_target::init_global(); !res) {
        MessageBoxW(NULL, L"Failed to initialize global render target", L"Error",
                    MB_ICONERROR);
        return nullptr;
    }
    spdlog::info("[perf] menu_rt init_global {}ms", ms_since(t0));

    static auto rt = []() {
        static window_proc_hook glfw_proc_hook;
        auto rt = std::make_shared<ui::render_target>();
        rt->transparent = true;
        rt->no_activate = true;
        // 独占鼠标输入只在面板真正显示时才需要：预热阶段抓住用户的鼠标会出事
        rt->capture_all_input = false;
        rt->decorated = false;
        rt->topmost = true;
        rt->vsync = config::current->context_menu.vsync;
        rt->idle_poll_ms = config::current->host.menu_idle_poll_ms;

        if (config::current->avoid_resize_ui) {
            rt->width = 3840;
            rt->height = 2159;
        }

        auto t_init = std::chrono::steady_clock::now();
        if (auto res = rt->init(); !res) {
            MessageBoxW(NULL, L"Failed to initialize render target", L"Error",
                        MB_ICONERROR);
            return std::shared_ptr<ui::render_target>{nullptr};
        }
        spdlog::info("[perf] menu_rt init() {}ms", ms_since(t_init));

        glfw_proc_hook.install(rt->hwnd());
        glfw_proc_hook.hooks.push_back([](void *hwnd, void *original_proc,
                                          size_t msg, size_t wparam,
                                          size_t lparam) -> std::optional<int> {
            if (msg == WM_MOUSEACTIVATE) {
                return MA_NOACTIVATE;
            }

            return std::nullopt;
        });

        auto t_font = std::chrono::steady_clock::now();
        config::current->apply_fonts_to_nvg(rt->nvg);
        spdlog::info("[perf] menu_rt apply_fonts {}ms", ms_since(t_font));
        return rt;
    }();
    return rt;
}

void menu_render::prewarm() {
    auto t0 = std::chrono::steady_clock::now();
    auto rt = render_target();
    if (!rt) {
        spdlog::error("prewarm: failed to create menu render target");
        return;
    }
    // 预热线程退出前必须解绑 GL 上下文，否则渲染线程再也没法把它设为 current
    glfwMakeContextCurrent(nullptr);
    spdlog::info("[perf] menu_render::prewarm {}ms", ms_since(t0));
}

menu_render menu_render::create(int x, int y, menu menu, bool run_js) {
    auto t_target = std::chrono::steady_clock::now();
    auto rt = render_target();
    spdlog::info("[perf] menu_render::create render_target {}ms",
                 ms_since(t_target));
    if (!rt) {
        return {nullptr, std::nullopt};
    }
    auto render = menu_render(rt, std::nullopt);

    rt->parent = menu.parent_window;

    // get the monitor in which the menu is being shown
    auto monitor = MonitorFromPoint({x, y}, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEX monitor_info;
    monitor_info.cbSize = sizeof(MONITORINFOEX);
    GetMonitorInfo(monitor, &monitor_info);

    // set the position of the window to fullscreen in this monitor + padding

    spdlog::info("Monitor: {} {} {} {}", monitor_info.rcMonitor.left,
           monitor_info.rcMonitor.top, monitor_info.rcMonitor.right,
           monitor_info.rcMonitor.bottom);

    rt->set_position(monitor_info.rcMonitor.left + 1,
                     monitor_info.rcMonitor.top + 1);
    if (!config::current->avoid_resize_ui)
        rt->resize(
            monitor_info.rcMonitor.right - monitor_info.rcMonitor.left - 2,
            monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top - 2);

    glfwMakeContextCurrent(rt->window);
    glfwSwapInterval(config::current->context_menu.vsync ? 1 : 0);
    glfwSetWindowAttrib(rt->window, GLFW_MOUSE_PASSTHROUGH, GLFW_FALSE);

    rt->show();
    if (config::current->host.menu_capture_all_input) {
        // 面板显示期间独占鼠标输入：面板外的点击也会送回这里（用来关面板）
        rt->capture_all_input = true;
        SetCapture((HWND)rt->hwnd());
    }
    spdlog::info("[perf] menu_render::create show {}ms", ms_since(t_target));
    auto menu_wid = std::make_shared<mouse_menu_widget_main>(
        menu,
        // convert the x and y to the window coordinates
        x - monitor_info.rcMonitor.left, y - monitor_info.rcMonitor.top);
    context_menu_hooks::set_active_root_menu_widget(menu_wid->menu_wid);
    rt->root->owner_rt = rt.get();
    rt->root->add_child(menu_wid);
    auto current_js_context =
        entry::main_window_loop_hook
            .add_task([&]() {
                return std::make_shared<js::js_menu_context>(
                    js::js_menu_context::$from_window(menu.parent_window));
            })
            .get();
    spdlog::info("[perf] menu_render::create js_context {}ms",
                 ms_since(t_target));

    js::menu_info_basic_js menu_info{
        .menu = std::make_shared<js::menu_controller>(menu_wid->menu_wid),
        .context = current_js_context,
        .screenside_button =
            std::make_shared<js::screenside_button_controller>(menu_wid),
    };

    if (run_js) {
        spdlog::info("[perf] JS plugins start");
        auto before_js = rt->clock.now();
        for (auto &listener : menu_callbacks_js) {
            // Plugin callbacks can run against an already destroyed JS context
            // (scripts are reloaded asynchronously); a single misbehaving plugin
            // must not be able to abort the whole shell process.
            try {
                listener->operator()(menu_info);
            } catch (const std::exception &e) {
                spdlog::error("Error in JS menu callback: {}", e.what());
            } catch (...) {
                spdlog::error("Error in JS menu callback: unknown exception");
            }
        }
        spdlog::info("[perf] JS plugins costed {}ms",
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   rt->clock.now() - before_js)
                   .count());
    } else {
        spdlog::info("Skipped running JS");
    }

    spdlog::info("Current menu: {}", menu_render::current.has_value());
    return render;
}

menu_render::menu_render(std::shared_ptr<ui::render_target> rt,
                         std::optional<int> selected_menu)
    : rt(std::move(rt)), selected_menu(selected_menu) {
    current = this;
}
menu_render::~menu_render() {
    if (this->rt) {
        current = nullptr;
        // 面板关掉后必须交还鼠标独占，否则鼠标输入会一直留在这个（已隐藏的）窗口上
        auto hwnd = (HWND)this->rt->hwnd();
        if (GetCapture() == hwnd)
            ReleaseCapture();
    }
}
menu_render::menu_render(menu_render &&t) {
    current = this;

    rt = std::move(t.rt);
    selected_menu = std::move(t.selected_menu);
    took_keyboard_focus = t.took_keyboard_focus;
}
menu_render &menu_render::operator=(menu_render &&t) {
    current = this;
    rt = std::move(t.rt);
    selected_menu = std::move(t.selected_menu);
    took_keyboard_focus = t.took_keyboard_focus;
    return *this;
}
}; // namespace mb_shell
