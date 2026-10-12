#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <objbase.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

/* ABI from Flutter's public flutter_windows.h. This program deliberately
 * resolves and invokes only Create/Destroy. It never runs Dart code or creates
 * a Flutter view. AOT file parsing happens later in EngineRun, so this smoke
 * test validates the properties ABI and native engine lifecycle only. */
typedef struct {
    const wchar_t *assets_path;
    const wchar_t *icu_data_path;
    const wchar_t *aot_library_path;
    const char *dart_entrypoint;
    int dart_entrypoint_argc;
    const char **dart_entrypoint_argv;
} EngineProperties;
typedef void *(*EngineCreate)(const EngineProperties *);
typedef bool (*EngineDestroy)(void *);

_Static_assert(sizeof(EngineProperties) == 48, "Expected Flutter x64 ABI");
_Static_assert(offsetof(EngineProperties, dart_entrypoint_argv) == 40,
               "Expected Flutter x64 field alignment");

static bool join_path(wchar_t *out, size_t capacity,
                      const wchar_t *root, const wchar_t *relative) {
    int n = swprintf(out, capacity, L"%ls\\%ls", root, relative);
    return n >= 0 && (size_t)n < capacity;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 2 || argc > 3) {
        fwprintf(stderr, L"Usage: real_engine_smoke.exe <Oopz install root> [AOT path]\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    wchar_t engine_path[32768], assets_path[32768], icu_path[32768];
    wchar_t default_aot_path[32768];
    if (!join_path(engine_path, 32768, argv[1], L"flutter_windows.dll") ||
        !join_path(assets_path, 32768, argv[1], L"data\\flutter_assets") ||
        !join_path(icu_path, 32768, argv[1], L"data\\icudtl.dat") ||
        !join_path(default_aot_path, 32768, argv[1], L"data\\app.so")) {
        fputs("Path is too long.\n", stderr);
        return 3;
    }
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                  LOAD_LIBRARY_SEARCH_USER_DIRS)) {
        fprintf(stderr, "SetDefaultDllDirectories failed: %lu\n", GetLastError());
        return 4;
    }
    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(argv[1]);
    if (!cookie) {
        fprintf(stderr, "AddDllDirectory failed: %lu\n", GetLastError());
        return 4;
    }
    HRESULT com_result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
        fprintf(stderr, "CoInitializeEx failed: 0x%08lx\n", (unsigned long)com_result);
        RemoveDllDirectory(cookie);
        return 5;
    }
    HMODULE module = LoadLibraryExW(engine_path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 |
        LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!module) {
        fprintf(stderr, "LoadLibraryExW(flutter_windows.dll) failed: %lu\n", GetLastError());
        CoUninitialize();
        RemoveDllDirectory(cookie);
        return 6;
    }
    EngineCreate create_engine = (EngineCreate)GetProcAddress(module, "FlutterDesktopEngineCreate");
    EngineDestroy destroy_engine = (EngineDestroy)GetProcAddress(module, "FlutterDesktopEngineDestroy");
    int result = 7;
    if (!create_engine || !destroy_engine) {
        fputs("Required Flutter Create/Destroy exports missing.\n", stderr);
        goto done;
    }
    EngineProperties properties = {0};
    properties.assets_path = assets_path;
    properties.icu_data_path = icu_path;
    properties.aot_library_path = argc == 3 ? argv[2] : default_aot_path;
    void *engine = create_engine(&properties);
    if (!engine) {
        fputs("FlutterDesktopEngineCreate returned NULL.\n", stderr);
        result = 8;
        goto done;
    }
    if (!destroy_engine(engine)) {
        fputs("FlutterDesktopEngineDestroy returned false.\n", stderr);
        result = 9;
        goto done;
    }
    puts("PASS: real Flutter engine created/destroyed; six-field x64 ABI accepted.");
    puts("Dart was not run. This does not verify AOT parsing or application behavior.");
    result = 0;
done:
    FreeLibrary(module);
    CoUninitialize();
    RemoveDllDirectory(cookie);
    return result;
}
