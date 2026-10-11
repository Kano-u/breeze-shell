#include <chrono>
#include <thread>

#include <Windows.h>

// shell.dll 通过导入表在本进程启动时加载，DllMain 里已跑完 mb_shell::main()：
// 渲染器、QuickJS 运行时、插件加载都在那一刻起来。这里只需要把进程挂住不退出。
extern "C" __declspec(dllimport) void func();

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    func();
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
