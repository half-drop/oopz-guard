#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include "window_filter.h"
#include <stdint.h>
#include <string.h>

static BOOL filter_active(void) {
    return InterlockedCompareExchange(&guard_window_filter_active, 0, 0) == 1;
}

static WCHAR lower_ascii(WCHAR ch) {
    return ch >= L'A' && ch <= L'Z' ? (WCHAR)(ch + (L'a' - L'A')) : ch;
}

static BOOL equal_ascii_insensitive(const WCHAR *wide, const char *ascii) {
    while (*wide && *ascii && lower_ascii(*wide) == (WCHAR)(unsigned char)*ascii) {
        ++wide;
        ++ascii;
    }
    return *wide == 0 && *ascii == 0;
}

static BOOL contains_obs_directory(const WCHAR *path) {
    const char *part = "\\obs-studio\\";
    for (; *path; ++path) {
        const WCHAR *candidate = path;
        const char *match = part;
        while (*candidate && *match &&
               lower_ascii(*candidate) == (WCHAR)(unsigned char)*match) {
            ++candidate;
            ++match;
        }
        if (*match == 0) return TRUE;
    }
    return FALSE;
}

/* Fail open and restore LastError even when HWND lookup/process access fails.
   There is no process scan: only the owner of the supplied HWND is queried. */
static BOOL hidden_window(HWND window) {
    DWORD previous_error = GetLastError();
    DWORD pid = 0;
    BOOL hidden = FALSE;
    HANDLE process = NULL;
    WCHAR *image = NULL;
    if (!filter_active() || !window) goto done;
    if (!GetWindowThreadProcessId(window, &pid) || !pid) goto done;
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) goto done;
    image = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, 32768 * sizeof(WCHAR));
    if (image) {
        DWORD count = 32768;
        if (QueryFullProcessImageNameW(process, 0, image, &count) && count < 32768) {
            const WCHAR *basename = image;
            image[count] = 0;
            for (const WCHAR *at = image; *at; ++at) {
                if (*at == L'\\' || *at == L'/') basename = at + 1;
            }
            hidden = equal_ascii_insensitive(basename, "obs.exe") ||
                     equal_ascii_insensitive(basename, "obs32.exe") ||
                     equal_ascii_insensitive(basename, "obs64.exe") ||
                     equal_ascii_insensitive(basename, "yyobs.exe") ||
                     contains_obs_directory(image);
        }
    }
done:
    if (image) HeapFree(GetProcessHeap(), 0, image);
    if (process) CloseHandle(process);
    SetLastError(previous_error);
    return hidden;
}

static HWND WINAPI filtered_FindWindowA(LPCSTR class_name, LPCSTR title) {
    HWND result = FindWindowA(class_name, title);
    DWORD error = GetLastError();
    while (result && hidden_window(result)) {
        result = FindWindowExA(NULL, result, class_name, title);
    }
    SetLastError(error);
    return result;
}

static HWND WINAPI filtered_FindWindowW(LPCWSTR class_name, LPCWSTR title) {
    HWND result = FindWindowW(class_name, title);
    DWORD error = GetLastError();
    while (result && hidden_window(result)) {
        result = FindWindowExW(NULL, result, class_name, title);
    }
    SetLastError(error);
    return result;
}

static HWND WINAPI filtered_FindWindowExA(HWND parent, HWND after, LPCSTR class_name, LPCSTR title) {
    HWND result = FindWindowExA(parent, after, class_name, title);
    DWORD error = GetLastError();
    while (result && hidden_window(result)) {
        result = FindWindowExA(parent, result, class_name, title);
    }
    SetLastError(error);
    return result;
}

static HWND WINAPI filtered_FindWindowExW(HWND parent, HWND after, LPCWSTR class_name, LPCWSTR title) {
    HWND result = FindWindowExW(parent, after, class_name, title);
    DWORD error = GetLastError();
    while (result && hidden_window(result)) {
        result = FindWindowExW(parent, result, class_name, title);
    }
    SetLastError(error);
    return result;
}

typedef struct FilterCallback {
    WNDENUMPROC callback;
    LPARAM parameter;
} FilterCallback;

static BOOL CALLBACK filtered_callback(HWND window, LPARAM parameter) {
    FilterCallback *context = (FilterCallback *)parameter;
    if (hidden_window(window)) return TRUE;
    /* Propagate FALSE immediately; retain LastError set by the user's callback. */
    return context->callback(window, context->parameter);
}

static BOOL WINAPI filtered_EnumWindows(WNDENUMPROC callback, LPARAM parameter) {
    if (!callback || !filter_active()) return EnumWindows(callback, parameter);
    FilterCallback context = {callback, parameter};
    return EnumWindows(filtered_callback, (LPARAM)&context);
}

static BOOL WINAPI filtered_EnumChildWindows(HWND parent, WNDENUMPROC callback, LPARAM parameter) {
    if (!callback || !filter_active()) return EnumChildWindows(parent, callback, parameter);
    FilterCallback context = {callback, parameter};
    return EnumChildWindows(parent, filtered_callback, (LPARAM)&context);
}

static int WINAPI filtered_GetWindowTextA(HWND window, LPSTR buffer, int capacity) {
    if (!hidden_window(window)) return GetWindowTextA(window, buffer, capacity);
    if (buffer && capacity > 0) buffer[0] = 0;
    return 0;
}

static int WINAPI filtered_GetWindowTextW(HWND window, LPWSTR buffer, int capacity) {
    if (!hidden_window(window)) return GetWindowTextW(window, buffer, capacity);
    if (buffer && capacity > 0) buffer[0] = 0;
    return 0;
}

static int WINAPI filtered_GetWindowTextLengthA(HWND window) {
    return hidden_window(window) ? 0 : GetWindowTextLengthA(window);
}

static int WINAPI filtered_GetWindowTextLengthW(HWND window) {
    return hidden_window(window) ? 0 : GetWindowTextLengthW(window);
}

static int WINAPI filtered_GetClassNameA(HWND window, LPSTR buffer, int capacity) {
    if (!hidden_window(window)) return GetClassNameA(window, buffer, capacity);
    if (buffer && capacity > 0) buffer[0] = 0;
    return 0;
}

static int WINAPI filtered_GetClassNameW(HWND window, LPWSTR buffer, int capacity) {
    if (!hidden_window(window)) return GetClassNameW(window, buffer, capacity);
    if (buffer && capacity > 0) buffer[0] = 0;
    return 0;
}

FARPROC window_filter_resolve(HMODULE module, LPCSTR name, FARPROC original) {
    DWORD error = GetLastError();
    FARPROC result = original;
    if (!original || !name || (uintptr_t)name <= 0xffff || !filter_active()) goto done;
    if (module != GetModuleHandleW(L"user32.dll")) goto done;
#define MATCH(api) if (strcmp(name, #api) == 0) { result = (FARPROC)(void *)filtered_##api; goto done; }
    MATCH(FindWindowA)
    MATCH(FindWindowW)
    MATCH(FindWindowExA)
    MATCH(FindWindowExW)
    MATCH(EnumWindows)
    MATCH(EnumChildWindows)
    MATCH(GetWindowTextA)
    MATCH(GetWindowTextW)
    MATCH(GetWindowTextLengthA)
    MATCH(GetWindowTextLengthW)
    MATCH(GetClassNameA)
    MATCH(GetClassNameW)
#undef MATCH
done:
    SetLastError(error);
    return result;
}
