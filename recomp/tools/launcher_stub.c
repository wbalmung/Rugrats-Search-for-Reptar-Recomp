/* RugratsLauncher.exe: double-click start for the release bundle (no console window, unlike Launcher.bat).
   It only starts the bundled Python with the launcher window: python\pythonw.exe -I recomp\launcher.py [args],
   both next to this exe. Built by recomp/tools/make_bundle.py with the bundled clang (icon drawn there too); never
   committed as an exe. RUGRATS_STUB_WAIT=1 waits for the launcher and returns its exit code (bundle self-test). */
#include <windows.h>
#include <wchar.h>

static void fail(const wchar_t *msg) {
    MessageBoxW(NULL, msg, L"Rugrats: Search for Reptar", MB_OK | MB_ICONERROR);
}

static int exists(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR args, int show) {
    (void)inst; (void)prev; (void)show;
    static wchar_t dir[32768], py[32768], script[32768], cmd[32768];
    DWORD n = GetModuleFileNameW(NULL, dir, 32768);
    if (n == 0 || n >= 32768) { fail(L"Could not find the program's own folder."); return 1; }
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) { fail(L"Could not find the program's own folder."); return 1; }
    *slash = 0;
    swprintf(py, 32768, L"%ls\\python\\pythonw.exe", dir);
    swprintf(script, 32768, L"%ls\\recomp\\launcher.py", dir);
    if (!exists(py) || !exists(script)) {
        fail(L"The python and recomp folders were not found next to RugratsLauncher.exe.\n\n"
             L"Extract the whole zip into a folder first (right-click > Extract All), then start "
             L"RugratsLauncher.exe from there.");
        return 1;
    }
    swprintf(cmd, 32768, L"\"%ls\" -I \"%ls\"%ls%ls", py, script, args && *args ? L" " : L"", args ? args : L"");
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(py, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        fail(L"Could not start the bundled Python (python\\pythonw.exe).");
        return 1;
    }
    DWORD code = 0;
    wchar_t w[8];
    if (GetEnvironmentVariableW(L"RUGRATS_STUB_WAIT", w, 8) && w[0] == L'1') {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
