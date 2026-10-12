#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

typedef struct {
    const wchar_t *assets, *icu, *aot;
    const char *entrypoint;
    int argc;
    const char **argv;
} Properties;
__declspec(dllimport) void *FlutterDesktopEngineCreate(const Properties *p);
__declspec(dllimport) const wchar_t *ReceivedSnapshot(void);
__declspec(dllimport) FARPROC ResolveUser32(const char *name);

static int failure;
#define REQUIRE(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failure; } } while (0)

int wmain(int argc, wchar_t **argv) {
    if (argc < 3 || argc > 4) return 90;
    const wchar_t *input = argv[1];
    REQUIRE(GetFileAttributesW(input) != INVALID_FILE_ATTRIBUTES);
    BOOL expect_patch = wcscmp(argv[2], L"patched") == 0;
    BOOL expect_disabled = wcscmp(argv[2], L"disabled") == 0;
    BOOL expect_noads = wcscmp(argv[2], L"noads") == 0;
    BOOL expect_no_privacy = GetPrivateProfileIntW(L"Guard", L"HideOBSWindows", 1, L".\\oopz_guard.ini") == 0;
    HMODULE proxy = GetModuleHandleW(L"dwmapi.dll");
    REQUIRE(proxy != NULL);
    DWORD (WINAPI *status)(void) = (void *)GetProcAddress(proxy, "OopzGuardStatus");
    REQUIRE(status != NULL);
    if (!status) return 91;
    REQUIRE((status() & 3) == 3);
    BOOL composition = FALSE;
    HRESULT a = DwmIsCompositionEnabled(&composition);
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH); wcscat(system, L"\\dwmapi.dll");
    HMODULE actual = LoadLibraryExW(system, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HRESULT (WINAPI *direct)(BOOL *) = (void *)GetProcAddress(actual, "DwmIsCompositionEnabled");
    BOOL direct_composition = FALSE;
    REQUIRE(a == direct(&direct_composition));
    REQUIRE(composition == direct_composition);
    int attribute = 1;
    HRESULT (WINAPI *direct_attribute)(HWND, DWORD, LPCVOID, DWORD) = (void *)GetProcAddress(actual, "DwmSetWindowAttribute");
    REQUIRE(DwmSetWindowAttribute(NULL, 2, &attribute, sizeof(attribute)) == direct_attribute(NULL, 2, &attribute, sizeof(attribute)));
    REQUIRE(GetProcAddress(proxy, "DwmSetWindowAttribute") == GetProcAddress(proxy, MAKEINTRESOURCEA(193)));
    Properties props = { L"assets", L"icu", input, NULL, 0, NULL };
    REQUIRE(FlutterDesktopEngineCreate(&props) == (void *)0x1234);
    REQUIRE(props.aot == input);
    if (expect_patch) {
        REQUIRE((status() & 12) == 12);
        REQUIRE(wcscmp(input, ReceivedSnapshot()) != 0);
        HANDLE f = CreateFileW(ReceivedSnapshot(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        REQUIRE(f != INVALID_HANDLE_VALUE);
        REQUIRE(argc == 4);
        HANDLE expected = argc == 4 ? CreateFileW(argv[3], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
        REQUIRE(expected != INVALID_HANDLE_VALUE);
        if (f != INVALID_HANDLE_VALUE && expected != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size, expected_size;
            REQUIRE(GetFileSizeEx(f, &size)); REQUIRE(GetFileSizeEx(expected, &expected_size));
            REQUIRE(size.QuadPart == expected_size.QuadPart);
            char actual_bytes[8192], expected_bytes[8192]; DWORD n=0, m=0;
            do {
                REQUIRE(ReadFile(f, actual_bytes, sizeof(actual_bytes), &n, NULL));
                REQUIRE(ReadFile(expected, expected_bytes, sizeof(expected_bytes), &m, NULL));
                REQUIRE(n == m && !memcmp(actual_bytes, expected_bytes, n));
            } while (n && m);
        }
        if (expected != INVALID_HANDLE_VALUE) CloseHandle(expected);
        CloseHandle(f);
        REQUIRE(ResolveUser32("MessageBoxW") == GetProcAddress(GetModuleHandleW(L"user32.dll"), "MessageBoxW"));
        Properties second = { L"assets", L"icu", L"different-unsupported-snapshot.so", NULL, 0, NULL };
        REQUIRE(FlutterDesktopEngineCreate(&second) == (void *)0x1234);
        REQUIRE(!wcscmp(ReceivedSnapshot(), second.aot));
        REQUIRE(second.aot != NULL);
    } else {
        REQUIRE((status() & 12) == ((expect_disabled || expect_noads) ? 12 : 4));
        REQUIRE(!wcscmp(input, ReceivedSnapshot()));
    }
    FARPROC own = GetProcAddress(GetModuleHandleW(L"user32.dll"), "EnumWindows");
    REQUIRE((ResolveUser32("EnumWindows") == own) == expect_no_privacy);
    printf("loader test (%s): %d failures, status=0x%lx\n", expect_patch ? "patched" : expect_disabled ? "disabled" : expect_noads ? "noads" : "original", failure, status());
    return failure ? 1 : 0;
}
