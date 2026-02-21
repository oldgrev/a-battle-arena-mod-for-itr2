#pragma once

#include <Windows.h>

namespace SDK { class UWorld; }

namespace Mod
{
    class ModMain
    {
    public:
        static DWORD Run(HMODULE moduleHandle);
        static void OnTick(SDK::UWorld* world);
    };
}
