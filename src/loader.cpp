// BBCenterLoader.exe — 挂起启动中控并注入 bbcenter_ext.dll
// 用法：  BBCenterLoader.exe                启动同目录 BBCenter.exe 并注入
//         BBCenterLoader.exe --attach 1234  注入到已在跑的中控
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>
#include <string>

static std::wstring g_dir;
static std::wstring g_dll;

static void say(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}

static bool inject(HANDLE proc) {
    const size_t bytes = (g_dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { say("  [!] VirtualAllocEx failed (%lu)", GetLastError()); return false; }
    if (!WriteProcessMemory(proc, remote, g_dll.c_str(), bytes, nullptr)) {
        say("  [!] WriteProcessMemory failed (%lu)", GetLastError()); return false;
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    if (!loadLib) { say("  [!] LoadLibraryW not found"); return false; }
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th) { say("  [!] CreateRemoteThread failed (%lu)", GetLastError()); return false; }
    WaitForSingleObject(th, 15000);
    DWORD code = 0; GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    say("  [ok] LoadLibraryW -> hModule=0x%llx", (unsigned long long)code);
    return code != 0;
}

static DWORD pidByName(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) { found = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

int wmain(int argc, wchar_t** argv) {
    wchar_t buf[MAX_PATH]{}; GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_dir = buf;
    const size_t p = g_dir.find_last_of(L"\\/");
    if (p != std::wstring::npos) g_dir = g_dir.substr(0, p);
    g_dll = g_dir + L"\\bbcenter_ext.dll";
    const std::wstring target = g_dir + L"\\BBCenter.exe";

    say("BBCenterLoader 0.1 (%s)", BBEXT_VER);
    say("  dir    = %ls", g_dir.c_str());
    say("  dll    = %ls", g_dll.c_str());

    if (GetFileAttributesW(g_dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        say("  [!] bbcenter_ext.dll not found next to the loader"); return 2;
    }

    if (argc >= 3 && _wcsicmp(argv[1], L"--attach") == 0) {
        const DWORD pid = (DWORD)_wtoi(argv[2]);
        say("  attach pid=%lu", pid);
        HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!h) { say("  [!] OpenProcess failed (%lu)", GetLastError()); return 3; }
        const bool ok = inject(h);
        CloseHandle(h);
        say(ok ? "  done (attach)" : "  failed (attach)");
        return ok ? 0 : 4;
    }

    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        say("  [!] BBCenter.exe not found: %ls", target.c_str()); return 2;
    }
    if (DWORD existing = pidByName(L"BBCenter.exe")) {
        say("  [i] BBCenter.exe already running (pid=%lu) -> attaching", existing);
        HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, existing);
        if (!h) { say("  [!] OpenProcess failed (%lu)", GetLastError()); return 3; }
        const bool ok = inject(h);
        CloseHandle(h);
        say(ok ? "  done (attach)" : "  failed (attach)");
        return ok ? 0 : 4;
    }

    std::wstring cmd = L"\"" + target + L"\"";
    std::wstring workdir = g_dir;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    say("  spawning (suspended): %ls", target.c_str());
    if (!CreateProcessW(target.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, workdir.c_str(), &si, &pi)) {
        say("  [!] CreateProcess failed (%lu)", GetLastError()); return 5;
    }
    say("  [ok] pid=%lu tid=%lu", pi.dwProcessId, pi.dwThreadId);
    const bool ok = inject(pi.hProcess);
    if (ok) {
        ResumeThread(pi.hThread);
        say("  [ok] resumed — 中控已启动，看 bbcenter_ext.log");
    } else {
        say("  [!] injection failed, resuming without ext");
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return ok ? 0 : 6;
}
