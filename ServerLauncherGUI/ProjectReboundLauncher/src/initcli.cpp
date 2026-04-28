// ======================================================
//  CLI MODE – MATCH ORIGINAL WRAPPER BEHAVIOR
// ======================================================
#include "headers/wrapper.h"

void WrapperMain()
{
    InitWrapperCore();
    StartConsoleInput();
    RunWrapperLoop();
}

void InitCLI()
{
    WrapperMain();   // performs InitCore + ConsoleInput + RunLoop
}