#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const wchar_t *assets, *icu, *aot;
    const char *entrypoint;
    int argc;
    const char **argv;
} Properties;
static wchar_t received[MAX_PATH];

__declspec(dllexport) void *FlutterDesktopEngineCreate(const Properties *p) {
    DWORD n = (DWORD)wcslen(p->aot);
    if (n >= MAX_PATH) return NULL;
    wcscpy(received, p->aot);
    return (void *)(uintptr_t)0x1234;
}

__declspec(dllexport) const wchar_t *ReceivedSnapshot(void) { return received; }
__declspec(dllexport) FARPROC ResolveUser32(const char *name) {
    return GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
}
