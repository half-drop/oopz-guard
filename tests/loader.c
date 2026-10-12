#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../src/profile.h"

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
    if (argc != 3) return 90;
    const wchar_t *input = argv[1];
    REQUIRE(GetFileAttributesW(input) != INVALID_FILE_ATTRIBUTES);
    BOOL expect_patch = wcscmp(argv[2], L"patched") == 0;
    BOOL expect_disabled = wcscmp(argv[2], L"disabled") == 0;
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
        for (size_t i=0; i<PROFILE_PATCH_COUNT; ++i) {
            char data[100]; DWORD n;
            SetFilePointer(f, profile_patches[i].offset, NULL, FILE_BEGIN);
            REQUIRE(ReadFile(f, data, (DWORD)strlen(profile_patches[i].after), &n, NULL));
            REQUIRE(!memcmp(data, profile_patches[i].after, n));
        }
        CloseHandle(f);
        FARPROC own = GetProcAddress(GetModuleHandleW(L"user32.dll"), "EnumWindows");
        REQUIRE(ResolveUser32("EnumWindows") != own);
        REQUIRE(ResolveUser32("MessageBoxW") == GetProcAddress(GetModuleHandleW(L"user32.dll"), "MessageBoxW"));
        Properties second = { L"assets", L"icu", L"different-unsupported-snapshot.so", NULL, 0, NULL };
        REQUIRE(FlutterDesktopEngineCreate(&second) == (void *)0x1234);
        REQUIRE(!wcscmp(ReceivedSnapshot(), second.aot));
        REQUIRE(second.aot != NULL);
    } else {
        REQUIRE((status() & 12) == (expect_disabled ? 12 : 4));
        REQUIRE(!wcscmp(input, ReceivedSnapshot()));
        REQUIRE(ResolveUser32("EnumWindows") == GetProcAddress(GetModuleHandleW(L"user32.dll"), "EnumWindows"));
    }
    printf("loader test (%s): %d failures, status=0x%lx\n", expect_patch ? "patched" : expect_disabled ? "disabled" : "original", failure, status());
    return failure ? 1 : 0;
}
