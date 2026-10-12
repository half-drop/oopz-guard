#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include "window_filter.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

volatile LONG guard_window_filter_active = 1;
static unsigned failures = 0;
#define CHECK(condition, label) do { if (!(condition)) { printf("FAIL: %s (error %lu)\n", label, GetLastError()); ++failures; } } while (0)

static LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

static BOOL register_class(void) {
    WNDCLASSW definition = {0};
    definition.lpfnWndProc = test_window_proc;
    definition.hInstance = GetModuleHandleW(NULL);
    definition.lpszClassName = L"OopzWindowFilterTestClass";
    return RegisterClassW(&definition) != 0;
}

static int helper_mode(const char *token) {
    WCHAR caption[160], event_name[160];
    swprintf(caption, 160, L"OopzWindowFilterHidden_%hs", token);
    swprintf(event_name, 160, L"Local\\OopzWindowFilterReady_%hs", token);
    if (!register_class()) return 20;
    HWND top = CreateWindowExW(0, L"OopzWindowFilterTestClass", caption, WS_OVERLAPPEDWINDOW,
                              0, 0, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    HWND child = CreateWindowExW(0, L"STATIC", L"Filter hidden child", WS_CHILD,
                                0, 0, 10, 10, top, NULL, GetModuleHandleW(NULL), NULL);
    if (!top || !child) return 21;
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);
    if (!ready) return 22;
    SetEvent(ready);
    CloseHandle(ready);
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

typedef struct CountContext { HWND hidden; HWND keep; unsigned hidden_count; unsigned keep_count; unsigned total; } CountContext;
static BOOL CALLBACK count_windows(HWND window, LPARAM parameter) {
    CountContext *context = (CountContext *)parameter;
    ++context->total;
    if (window == context->hidden) ++context->hidden_count;
    if (window == context->keep) ++context->keep_count;
    return TRUE;
}
static BOOL CALLBACK stop_windows(HWND window, LPARAM parameter) {
    (void)window;
    ++*(unsigned *)parameter;
    SetLastError(0x3456);
    return FALSE;
}

#define RESOLVE(type, name) ((type)(void *)window_filter_resolve(user32, #name, GetProcAddress(user32, #name)))
typedef HWND (WINAPI *FindA)(LPCSTR, LPCSTR);
typedef HWND (WINAPI *FindW)(LPCWSTR, LPCWSTR);
typedef HWND (WINAPI *FindExA)(HWND, HWND, LPCSTR, LPCSTR);
typedef HWND (WINAPI *FindExW)(HWND, HWND, LPCWSTR, LPCWSTR);
typedef BOOL (WINAPI *EnumTop)(WNDENUMPROC, LPARAM);
typedef BOOL (WINAPI *EnumChild)(HWND, WNDENUMPROC, LPARAM);
typedef int (WINAPI *TextA)(HWND, LPSTR, int);
typedef int (WINAPI *TextW)(HWND, LPWSTR, int);
typedef int (WINAPI *TextLength)(HWND);

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--helper") == 0) return helper_mode(argv[2]);
    WCHAR current[32768], helper[32768], command[32768], token[32], event_name[160], hidden_title[160], keep_title[160];
    char hidden_title_a[160], keep_title_a[160];
    DWORD pid = GetCurrentProcessId();
    swprintf(token, 32, L"%lu", pid);
    swprintf(event_name, 160, L"Local\\OopzWindowFilterReady_%ls", token);
    swprintf(hidden_title, 160, L"OopzWindowFilterHidden_%ls", token);
    swprintf(keep_title, 160, L"OopzWindowFilterKeep_%ls", token);
    snprintf(hidden_title_a, 160, "OopzWindowFilterHidden_%lu", pid);
    snprintf(keep_title_a, 160, "OopzWindowFilterKeep_%lu", pid);
    CHECK(GetModuleFileNameW(NULL, current, 32768) > 0, "find test executable");
    wcscpy(helper, current);
    WCHAR *slash = wcsrchr(helper, L'\\');
    if (!slash) return 2;
    wcscpy(slash + 1, L"obs64.exe");
    CHECK(CopyFileW(current, helper, FALSE), "copy helper under OBS basename");
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, event_name);
    CHECK(ready != NULL, "create helper readiness event");
    swprintf(command, 32768, L"\"%ls\" --helper %ls", helper, token);
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(helper, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) {
        CHECK(FALSE, "start hidden helper");
        CloseHandle(ready);
        return 3;
    }
    CHECK(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0, "helper readiness");
    CloseHandle(ready);
    CHECK(register_class(), "register ordinary test window");
    HWND keep = CreateWindowExW(0, L"OopzWindowFilterTestClass", keep_title, WS_OVERLAPPEDWINDOW,
                               0, 0, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    HWND keep_child = CreateWindowExW(0, L"STATIC", L"Filter keep child", WS_CHILD,
                                     0, 0, 10, 10, keep, NULL, GetModuleHandleW(NULL), NULL);
    HWND hidden = FindWindowW(NULL, hidden_title);
    HWND hidden_child = FindWindowExW(hidden, NULL, L"STATIC", NULL);
    CHECK(keep && keep_child && hidden && hidden_child, "create hidden and ordinary windows");
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    FARPROC original_find = GetProcAddress(user32, "FindWindowW");
    SetLastError(0x1234);
    CHECK(window_filter_resolve(user32, (LPCSTR)1, original_find) == original_find, "ordinal pass through");
    CHECK(GetLastError() == 0x1234, "resolver keeps LastError");
    CHECK(window_filter_resolve(GetModuleHandleW(L"kernel32.dll"), "FindWindowW", original_find) == original_find, "other module pass through");
    CHECK(window_filter_resolve(user32, "FindWindowW", NULL) == NULL, "missing symbol pass through");
    FindA find_a = RESOLVE(FindA, FindWindowA);
    FindW find_w = RESOLVE(FindW, FindWindowW);
    FindExA find_ex_a = RESOLVE(FindExA, FindWindowExA);
    FindExW find_ex_w = RESOLVE(FindExW, FindWindowExW);
    CHECK(find_a(NULL, hidden_title_a) == NULL, "FindWindowA hides OBS");
    CHECK(find_w(NULL, hidden_title) == NULL, "FindWindowW hides OBS");
    CHECK(find_ex_a(NULL, NULL, NULL, hidden_title_a) == NULL, "FindWindowExA hides OBS");
    CHECK(find_ex_w(NULL, NULL, NULL, hidden_title) == NULL, "FindWindowExW hides OBS");
    CHECK(find_a(NULL, keep_title_a) == keep && find_w(NULL, keep_title) == keep, "ordinary title remains discoverable");
    CHECK(find_ex_w(keep, NULL, L"STATIC", NULL) == keep_child, "ordinary child remains discoverable");
    CHECK(SetWindowTextW(hidden, keep_title), "set matching helper title");
    SetWindowPos(hidden, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    CHECK(find_a(NULL, keep_title_a) == keep && find_w(NULL, keep_title) == keep, "FindWindow skips hidden match to ordinary match");
    CHECK(find_ex_a(NULL, NULL, NULL, keep_title_a) == keep && find_ex_w(NULL, NULL, NULL, keep_title) == keep, "FindWindowEx skips hidden match to ordinary match");
    CHECK(SetWindowTextW(hidden, hidden_title), "restore helper title");
    TextA text_a = RESOLVE(TextA, GetWindowTextA);
    TextW text_w = RESOLVE(TextW, GetWindowTextW);
    TextA class_a = RESOLVE(TextA, GetClassNameA);
    TextW class_w = RESOLVE(TextW, GetClassNameW);
    TextLength len_a = RESOLVE(TextLength, GetWindowTextLengthA);
    TextLength len_w = RESOLVE(TextLength, GetWindowTextLengthW);
    char buffer_a[160] = "x";
    WCHAR buffer_w[160] = L"x";
    SetLastError(0x1234);
    CHECK(text_a(hidden, buffer_a, 160) == 0 && buffer_a[0] == 0, "hidden ANSI title empty");
    CHECK(GetLastError() == 0x1234, "hidden query preserves LastError");
    CHECK(text_w(hidden, buffer_w, 160) == 0 && buffer_w[0] == 0, "hidden Unicode title empty");
    CHECK(class_a(hidden, buffer_a, 160) == 0 && buffer_a[0] == 0, "hidden ANSI class empty");
    CHECK(class_w(hidden, buffer_w, 160) == 0 && buffer_w[0] == 0, "hidden Unicode class empty");
    CHECK(len_a(hidden) == 0 && len_w(hidden) == 0, "hidden text length zero");
    SetLastError(0x1234);
    int expected_length = GetWindowTextW(keep, buffer_w, 160);
    DWORD expected_error = GetLastError();
    SetLastError(0x1234);
    CHECK(text_w(keep, buffer_w, 160) == expected_length, "ordinary title value preserved");
    CHECK(GetLastError() == expected_error, "ordinary title LastError preserved");
    HWND invalid = (HWND)(UINT_PTR)0x123456;
    SetLastError(0x1234);
    int invalid_length = GetWindowTextW(invalid, buffer_w, 160);
    DWORD invalid_error = GetLastError();
    SetLastError(0x1234);
    CHECK(text_w(invalid, buffer_w, 160) == invalid_length, "invalid HWND preserves original result");
    CHECK(GetLastError() == invalid_error, "invalid HWND preserves original error");
    CHECK(class_a(keep, buffer_a, 160) > 0 && class_w(keep, buffer_w, 160) > 0, "ordinary class preserved");
    EnumTop enum_top = RESOLVE(EnumTop, EnumWindows);
    EnumChild enum_child = RESOLVE(EnumChild, EnumChildWindows);
    CountContext count = {hidden, keep, 0, 0, 0};
    CHECK(enum_top(count_windows, (LPARAM)&count), "enumeration completes");
    CHECK(count.hidden_count == 0 && count.keep_count == 1, "enumeration excludes only OBS test window");
    CountContext child_count = {hidden_child, keep_child, 0, 0, 0};
    enum_child(hidden, count_windows, (LPARAM)&child_count);
    CHECK(child_count.total == 0, "OBS children hidden");
    enum_child(keep, count_windows, (LPARAM)&child_count);
    CHECK(child_count.keep_count == 1, "ordinary children preserved");
    unsigned stopped = 0;
    CHECK(!enum_top(stop_windows, (LPARAM)&stopped), "EnumWindows FALSE early return");
    CHECK(stopped == 1 && GetLastError() == 0x3456, "EnumWindows callback error retained");
    stopped = 0;
    enum_child(keep, stop_windows, (LPARAM)&stopped);
    CHECK(stopped == 1 && GetLastError() == 0x3456, "EnumChildWindows early stop retained");
    InterlockedExchange(&guard_window_filter_active, 0);
    CHECK(window_filter_resolve(user32, "FindWindowW", original_find) == original_find, "disabled resolver pass through");
    CHECK(find_w(NULL, hidden_title) == hidden, "previous wrapper honors disabled setting");
    CHECK(text_w(hidden, buffer_w, 160) > 0, "disabled setting restores OBS title");
    PostMessageW(hidden, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 4);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    DestroyWindow(keep_child);
    DestroyWindow(keep);
    printf("window_filter tests: %u failures\n", failures);
    return failures ? 1 : 0;
}
