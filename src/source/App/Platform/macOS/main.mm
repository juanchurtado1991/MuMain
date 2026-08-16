// macOS entry point (DarkMu / MuMain desktop port).
//
// Same contract as Linux (issue #442): Winmain.cpp exposes a plain WinMain off
// Windows; this forwards into it. HINSTANCE / command-line args are unused.
#include "Core/Platform/WinCompat.h"

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdLine, int nCmdShow);

int main(int /*argc*/, char* /*argv*/[])
{
    return WinMain(nullptr, nullptr, nullptr, SW_SHOW);
}
