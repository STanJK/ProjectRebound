// FileLog.h
// Mirrors a debug line to DebugView *and* to a file, so logs can be read
// directly (no paste truncation). Append-per-call keeps it safe across the
// server + client processes.
//
// The path is derived from %USERPROFILE% rather than hard-coded, and if the
// primary file cannot be opened (a viewer may hold it — see
// NEXT_SESSION_PROMPT.md §3) the line goes to %TEMP% instead of being lost.

#pragma once
#include <cstdio>
#include <cstdarg>
#include <Windows.h>

inline const char* PrimaryLogPath()
{
    static char path[MAX_PATH * 2] = {};
    if (!path[0])
    {
        char home[MAX_PATH] = {};
        if (GetEnvironmentVariableA("USERPROFILE", home, MAX_PATH) == 0)
            snprintf(home, sizeof(home), ".");
        snprintf(path, sizeof(path), "%s\\Desktop\\Boundary_Payload.log", home);
    }
    return path;
}

inline void WriteLog(const char* msg)
{
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    FILE* f = nullptr;
    if (fopen_s(&f, PrimaryLogPath(), "ab") != 0 || !f)
    {
        // Primary log unavailable — fall back to %TEMP% so a locked file can
        // never silently swallow the whole log again.
        char temp[MAX_PATH] = {};
        char alt[MAX_PATH * 2] = {};
        if (GetTempPathA(MAX_PATH, temp) == 0)
            return;
        snprintf(alt, sizeof(alt), "%sBoundary_Payload.log", temp);
        if (fopen_s(&f, alt, "ab") != 0 || !f)
            return;
    }
    fprintf(f, "%s\n", msg);
    fflush(f);
    fclose(f);
}

// printf-style convenience. Truncates on overflow instead of invoking the
// invalid-parameter handler the way sprintf_s does, so a long line can never
// terminate the host process.
inline void LogF(const char* fmt, ...)
{
    char buf[1024] = {};
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    WriteLog(buf);
}
