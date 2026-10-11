#pragma once
#include "breeze_ui/ui.h"
#include "contextmenu.h"
#include "shell/utils.h"
#include <memory>
#include <optional>

namespace mb_shell {
struct menu_render {
    std::shared_ptr<ui::render_target> rt;
    std::optional<int32_t> selected_menu;
    bool took_keyboard_focus = false;
    bool light_color = is_light_mode();
    static std::optional<menu_render *> current;

    menu_render() = delete;
    menu_render(std::shared_ptr<ui::render_target> rt,
                std::optional<int> selected_menu);

    ~menu_render();
    const menu_render &operator=(const menu_render &) = delete;
    menu_render(menu_render &&t);
    menu_render &operator=(menu_render &&t);
    static menu_render create(int x, int y, menu menu, bool run_js = true);

    // 进程内唯一的菜单渲染目标（窗口 + GL 上下文 + 字体）。第一次调用会付初始化开销。
    static std::shared_ptr<ui::render_target> render_target();
    // 提前把渲染目标建好，让第一次弹面板不用等几秒
    static void prewarm();
};
} // namespace mb_shell