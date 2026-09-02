// FileLog.h
// Mirrors a debug line to DebugView *and* to a file, so logs can be read
// directly (no paste truncation). Append-per-call keeps it safe across the
// server + client processes.

#pragma once
#include <cstdio>
#include <Windows.h>

inline void WriteLog(const char* msg)
{
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    static const char* kLogPath = "C:\\Users\\hanne\\Desktop\\Boundary_Payload.log";
    FILE* f = nullptr;
    if (fopen_s(&f, kLogPath, "ab") == 0 && f)
    {
        fprintf(f, "%s\n", msg);
        fflush(f);
        fclose(f);
    }
}
