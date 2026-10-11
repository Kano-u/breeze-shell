#include "host_input.h"

#include <atomic>
#include <thread>

#include <Windows.h>
#include <spdlog/spdlog.h>

#include "hook_registry.h"

namespace mb_shell::host_input {
namespace {
std::atomic<ULONGLONG> right_button_down_tick{0};
std::atomic<int> long_press_ms{300};
std::atomic<int> pending_x{0};
std::atomic<int> pending_y{0};
std::atomic<bool> pending{false};

LRESULT CALLBACK right_button_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        switch (wparam) {
        case WM_RBUTTONDOWN:
            right_button_down_tick = GetTickCount64();
            break;
        case WM_RBUTTONUP: {
            auto down_tick = right_button_down_tick.exchange(0);
            if (down_tick != 0 &&
                GetTickCount64() - down_tick >=
                    static_cast<ULONGLONG>(long_press_ms.load())) {
                auto *info = reinterpret_cast<MSLLHOOKSTRUCT *>(lparam);
                pending_x = info->pt.x;
                pending_y = info->pt.y;
                pending = true;
                // 吞掉这次松开，系统原生菜单就不会弹出
                return 1;
            }
            break;
        }
        default:
            break;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
} // namespace

void install_right_button_long_press(int threshold_ms) {
    long_press_ms = threshold_ms;

    std::thread([]() {
        auto hook = SetWindowsHookExW(WH_MOUSE_LL, right_button_proc, nullptr,
                                    0);
        if (!hook) {
            spdlog::error("Failed to install right button long press hook: {}",
                          GetLastError());
            return;
        }
        spdlog::info("Right button long press hook installed ({}ms)",
                     long_press_ms.load());
        hook_registry::register_uninstaller(
            [hook]() { UnhookWindowsHookEx(hook); });

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }).detach();
}

std::optional<std::string> poll_long_press() {
    if (!pending.exchange(false))
        return std::nullopt;
    return std::to_string(pending_x.load()) + "," +
           std::to_string(pending_y.load());
}
} // namespace mb_shell::host_input
