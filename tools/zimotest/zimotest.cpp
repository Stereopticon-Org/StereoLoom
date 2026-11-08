// zimotest: checks that the Zondision DLLs carried as resources unpack and load
// (no display needed; no session is started).
//   cmake --build build/x64 --config Release --target zimotest
#include <windows.h>
#include <cstdio>
#include "Zondision.h"
int main()
{
    const bool ok = srw::Zondision::Available();
    std::printf("Zondision adapter %s\n", ok ? "loaded" : "NOT loaded");
    return ok ? 0 : 1;
}
