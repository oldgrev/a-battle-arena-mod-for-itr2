#include "pch.h"
#include "Mod/ModMain.hpp"


DWORD MainThread(HMODULE module)
{
	return Mod::ModMain::Run(module);
}


extern "C" __declspec(dllexport) const char* GetModSDKVersion()
{
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
		DisableThreadLibraryCalls(hModule);
		// Create a new thread to run the main logic of the DLL
		CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, nullptr);
		break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

