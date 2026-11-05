// Entry point: single instance, crash-safe routing restore, then the App message loop.

#include "app/App.h"
#include "audio/Routing.h"
#include "util/Log.h"

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

namespace {

std::atomic<bool> g_emergencyRestoreDone{false};

// Last-chance undo of system/app redirection so the user's audio is never left pointing at the silent device.
// May run on any thread, so it brings its own COM apartment when the thread has none.
void emergencyRestore(const char* why) {
    if (g_emergencyRestoreDone.exchange(true)) return;
    logx::error(std::string(why) + ": restoring audio routing");
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    audio::RoutingJournal::instance().restoreAll();
    if (SUCCEEDED(co)) CoUninitialize();
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* info) {
    char why[96];
    std::snprintf(why, sizeof(why), "Unhandled exception 0x%08lX at %p",
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul,
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    emergencyRestore(why);
    return EXCEPTION_CONTINUE_SEARCH;
}

[[noreturn]] void onTerminate() {
    emergencyRestore("std::terminate");
    std::abort();
}

// The MSVC terminate handler is per thread; abort() from any thread still raises the process-wide SIGABRT.
void onAbortSignal(int) { emergencyRestore("abort"); }

BOOL WINAPI onConsoleCtrl(DWORD) {
    emergencyRestore("Console control event");
    return FALSE;
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int showCmd) {
    // Before logx::init(): a second instance must not truncate the running instance's log or touch routing.
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\Spatiall.SingleInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(app::kWindowClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, RegisterWindowMessageW(app::kActivateMessage), 0, 0);
        }
        CloseHandle(instanceMutex);
        return 0;
    }

    logx::init();
    logx::info("Spatiall " SPATIALL_VERSION " starting");

    SetUnhandledExceptionFilter(&onUnhandledException);
    std::set_terminate(&onTerminate);
    std::signal(SIGABRT, &onAbortSignal);
    SetConsoleCtrlHandler(&onConsoleCtrl, TRUE);

    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(co)) logx::error("CoInitializeEx failed: " + logx::hr(co));
    // The manifest already declares PerMonitorV2; this only matters if the manifest is missing (fails harmlessly).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Undo anything a crashed previous run left redirected, before any code can touch routing.
    audio::RoutingJournal::instance().recoverFromCrash();

    int rc = 1;
    {
        auto application = std::make_unique<app::App>();
        if (application->init(instance, showCmd)) rc = application->run();
    }
    logx::info("Spatiall exiting");

    if (SUCCEEDED(co)) CoUninitialize();
    if (instanceMutex) CloseHandle(instanceMutex);
    return rc;
}
