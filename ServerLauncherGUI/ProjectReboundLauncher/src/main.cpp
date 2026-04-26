#include <windows.h>
#include <string>
#include <cstdio>

void InitCLI();
void InitGUI();

int main(int argc, char* argv[])
{
    std::string cmdline = GetCommandLineA();
    if (cmdline.find("-cli") != std::string::npos)
    {
        // CLI 模式：创建控制台并重定向标准 I/O
        AllocConsole();
        FILE* dummy;
        freopen_s(&dummy, "CONIN$", "r", stdin);
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        InitCLI();
    }
    else
    {
        // GUI 模式：没有控制台，直接启动
        InitGUI();
    }
    return 0;
}