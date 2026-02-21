// AILEARNINGS:
// - NEVER call MessageBoxA (or any UI/window function) from within DllMain callbacks or functions
//   called by the modloader while it holds the loader lock (DLL_THREAD_ATTACH, DLL_THREAD_DETACH,
//   DLL_PROCESS_DETACH, GetModSDKVersion). MessageBoxA runs a message pump that can deadlock.
// - ALWAYS call DisableThreadLibraryCalls(hModule) in DLL_PROCESS_ATTACH. Without it, DllMain is
//   called for every thread the game creates, causing chaos with any non-trivial callback body.
#include "pch.h"
#include "Mod/ModMain.hpp"


DWORD MainThread(HMODULE module)
{
	return Mod::ModMain::Run(module);
}


extern "C" __declspec(dllexport) const char* GetModSDKVersion()
{
    // NOTE: This is called by the modloader from DLL_PROCESS_ATTACH context.
    // Do NOT call MessageBoxA, LoadLibrary, or anything that acquires the loader lock here.
    return "1.0.0";
}


BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
		// Suppress DLL_THREAD_ATTACH / DLL_THREAD_DETACH notifications.
		// Without this, DllMain fires for every thread the game creates, which is
		// catastrophic if the callback body does anything non-trivial.
		DisableThreadLibraryCalls(hModule);
		// Create a new thread to run the main logic of the DLL.
		CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, nullptr);
		break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

