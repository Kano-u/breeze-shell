#pragma once

#include <optional>
#include <string>

namespace mb_shell::host_input {
// 独立宿主 (breeze-ui.exe) 的全局鼠标钩子：右键按住超过 threshold_ms 再松开时吞掉
// 这一次松开（系统原生菜单不会弹出），并把这次长按记下来等插件取走；短按原样放行，
// 系统原生菜单零延迟。
void install_right_button_long_press(int threshold_ms);

// 取走一次待处理的长按，返回 "x,y"（屏幕坐标）；没有待处理的长按时返回空。
std::optional<std::string> poll_long_press();
} // namespace mb_shell::host_input
