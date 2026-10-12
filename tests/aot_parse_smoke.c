#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <objbase.h>
#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>

typedef struct {
    const wchar_t *assets, *icu, *aot;
    const char *entrypoint;
    int argc;
    const char **argv;
} Properties;

/* Intentionally request an entrypoint that does not exist. Flutter must load
 * and parse the AOT image to report that error; Oopz's main is never invoked.
 * The native Run result is not an AOT verdict: inspect the missing-entrypoint
 * diagnostic, which is reached only after snapshot/isolate loading. */
int wmain(int argc, wchar_t **argv) {
    if (argc != 3) return 2;
    wchar_t path[MAX_PATH], assets[MAX_PATH], icu[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\flutter_windows.dll", argv[1]);
    swprintf(assets, MAX_PATH, L"%ls\\data\\flutter_assets", argv[1]);
    swprintf(icu, MAX_PATH, L"%ls\\data\\icudtl.dat", argv[1]);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_SEARCH_USER_DIRS)) return 3;
    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(argv[1]);
    if (!cookie) return 4;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 5;
    HMODULE module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!module) { fprintf(stderr, "load error=%lu\n", GetLastError()); return 6; }
    void *(*create)(const Properties *) = (void *)GetProcAddress(module, "FlutterDesktopEngineCreate");
    bool (*run)(void *, const char *) = (void *)GetProcAddress(module, "FlutterDesktopEngineRun");
    bool (*destroy)(void *) = (void *)GetProcAddress(module, "FlutterDesktopEngineDestroy");
    if (!create || !run || !destroy) return 7;
    Properties p = { assets, icu, argv[2], "oopz_guard_missing_entrypoint_749182", 0, NULL };
    void *engine = create(&p);
    if (!engine) return 8;
    bool started = run(engine, NULL);
    printf("Native Run result=%d; verify missing-entrypoint diagnostic in captured stderr.\n", started);
    bool closed = destroy(engine);
    FreeLibrary(module);
    CoUninitialize();
    RemoveDllDirectory(cookie);
    return closed ? 0 : 9;
}
