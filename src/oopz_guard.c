#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "window_filter.h"
#include "profile.h"
#include "dwmapi_names.h"

/* Only these first three fields are ever changed. The known Flutter ABI has
 * six fields and is checked by the real-engine smoke test before delivery. */
typedef struct {
    const wchar_t *assets_path, *icu_data_path, *aot_library_path;
    const char *dart_entrypoint;
    int dart_entrypoint_argc;
    const char **dart_entrypoint_argv;
} EngineProperties;
typedef void *(*EngineCreate)(const EngineProperties *);

static HMODULE self_module;
static EngineCreate original_engine_create;
static FARPROC (WINAPI *original_lookup)(HMODULE, LPCSTR);
static volatile LONG hook_status;
volatile LONG guard_window_filter_active;
static INIT_ONCE dwm_once = INIT_ONCE_STATIC_INIT;
static HMODULE system_dwm;
static FARPROC dwm_targets[256];
static wchar_t prepared_path[MAX_PATH];
static wchar_t validated_original_path[MAX_PATH];
static wchar_t ini_path[MAX_PATH];
static wchar_t log_path[MAX_PATH];
static HANDLE snapshot_lease;
static SRWLOCK prepare_lock = SRWLOCK_INIT;

static const wchar_t *basename_w(const wchar_t *path) {
    const wchar_t *base = path;
    for (; *path; ++path) if (*path == L'\\' || *path == L'/') base = path+1;
    return base;
}

static BOOL target_host(void) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    return n && n < MAX_PATH && !_wcsicmp(basename_w(path), L"oopz.exe");
}

static void log_line(const char *message) {
    if (!log_path[0]) {
        wchar_t temp[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, temp);
        if (!n || n+64 >= MAX_PATH) return;
        wcscat(temp, L"OopzGuard");
        if (!CreateDirectoryW(temp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return;
        swprintf(log_path, MAX_PATH, L"%ls\\oopz-%lu.log", temp, GetCurrentProcessId());
    }
    HANDLE f = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
                         NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(f, message, (DWORD)strlen(message), &written, NULL);
    WriteFile(f, "\r\n", 2, &written, NULL);
    CloseHandle(f);
}

static BOOL sha256(const unsigned char *data, DWORD size, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    DWORD needed = 0, object_size = 0;
    unsigned char *object = NULL;
    BOOL success = FALSE;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                          sizeof(object_size), &needed, 0) < 0) goto done;
    object = HeapAlloc(GetProcessHeap(), 0, object_size);
    if (!object) goto done;
    if (BCryptCreateHash(alg, &hash, object, object_size, NULL, 0, 0) < 0) goto done;
    if (BCryptHashData(hash, (PUCHAR)data, size, 0) < 0) goto done;
    success = BCryptFinishHash(hash, out, 32, 0) >= 0;
done:
    if (hash) BCryptDestroyHash(hash);
    if (object) HeapFree(GetProcessHeap(), 0, object);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return success;
}

static uint32_t dart_hash(const unsigned char *s, size_t length) {
    uint32_t h = 0;
    for (size_t i=0; i<length; ++i) {
        h += s[i]; h += h << 10; h ^= h >> 6;
    }
    h += h << 3; h ^= h >> 11; h += h << 15;
    h &= 0x3fffffffu;
    return h ? h : 1;
}

/* No generic binary search/replacement: the entire AOT image, object length,
 * class ID and stored hash must all match this exact investigated release. */
static BOOL patch_image(unsigned char *data, DWORD length) {
    for (size_t i=0; i<PROFILE_PATCH_COUNT; ++i) {
        const ProfilePatch *p = &profile_patches[i];
        size_t n = strlen(p->before);
        uint64_t tags, smi_length;
        if (p->offset < 16 || p->offset+n > length || strlen(p->after) != n) return FALSE;
        memcpy(&tags, data+p->offset-16, 8);
        memcpy(&smi_length, data+p->offset-8, 8);
        if (((tags >> 12) & 0xfffffu) != 94 || smi_length != n*2 ||
            (uint32_t)(tags >> 32) != dart_hash((const unsigned char *)p->before, n) ||
            memcmp(data+p->offset, p->before, n)) return FALSE;
    }
    for (size_t i=0; i<PROFILE_PATCH_COUNT; ++i) {
        const ProfilePatch *p = &profile_patches[i];
        size_t n = strlen(p->before);
        uint32_t hash = dart_hash((const unsigned char *)p->after, n);
        memcpy(data+p->offset, p->after, n);
        memcpy(data+p->offset-12, &hash, 4);
    }
    return TRUE;
}

static BOOL resolve_aot_path(const wchar_t *input, wchar_t full[MAX_PATH]) {
    if (!input || !*input) return FALSE;
    if ((input[0] && input[1] == L':') || input[0] == L'\\') {
        size_t n = wcslen(input);
        if (n >= MAX_PATH) return FALSE;
        wcscpy(full, input);
        return TRUE;
    }
    DWORD n = GetModuleFileNameW(NULL, full, MAX_PATH);
    if (!n || n >= MAX_PATH) return FALSE;
    wchar_t *base = (wchar_t *)basename_w(full);
    if ((size_t)(base-full)+wcslen(input) >= MAX_PATH) return FALSE;
    wcscpy(base, input);
    return TRUE;
}

static BOOL prepare_snapshot(const wchar_t *input, BOOL remove_ads) {
    wchar_t original[MAX_PATH], directory[MAX_PATH], temp[MAX_PATH], cache[MAX_PATH];
    unsigned char hash[32];
    unsigned char *data = NULL;
    HANDLE source = INVALID_HANDLE_VALUE, dest = INVALID_HANDLE_VALUE;
    BOOL success = FALSE;
    LARGE_INTEGER size;
    DWORD bytes = 0;
    temp[0] = 0;
    if (!resolve_aot_path(input, original)) goto done;
    source = CreateFileW(original, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (source == INVALID_HANDLE_VALUE) goto done;
    if (!GetFileSizeEx(source, &size) || size.QuadPart != PROFILE_SIZE) goto done;
    data = HeapAlloc(GetProcessHeap(), 0, PROFILE_SIZE);
    if (!data || !ReadFile(source, data, PROFILE_SIZE, &bytes, NULL) || bytes != PROFILE_SIZE) goto done;
    if (!sha256(data, PROFILE_SIZE, hash) || memcmp(hash, profile_sha256, 32)) goto done;
    wcscpy(validated_original_path, original);
    if (!remove_ads) { success = TRUE; goto done; }
    if (!patch_image(data, PROFILE_SIZE)) goto done;
    DWORD n = GetTempPathW(MAX_PATH, directory);
    if (!n || n+48 >= MAX_PATH) goto done;
    wcscat(directory, L"OopzGuard");
    if (!CreateDirectoryW(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) goto done;
    swprintf(cache, MAX_PATH, L"%ls\\%ls", directory, PROFILE_CACHE_FILENAME);
    HANDLE existing = CreateFileW(cache, GENERIC_READ, FILE_SHARE_READ, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (existing != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(existing, &size) && size.QuadPart == PROFILE_SIZE &&
            ReadFile(existing, data, PROFILE_SIZE, &bytes, NULL) && bytes == PROFILE_SIZE &&
            sha256(data, PROFILE_SIZE, hash) && !memcmp(hash, profile_patched_sha256, 32)) {
            snapshot_lease = existing;
            wcscpy(prepared_path, cache);
            success = TRUE;
        } else CloseHandle(existing);
        goto done;
    }
    if (GetLastError() != ERROR_FILE_NOT_FOUND) goto done;
    if (!GetTempFileNameW(directory, L"ogd", 0, temp)) goto done;
    dest = CreateFileW(temp, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       TRUNCATE_EXISTING, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (dest == INVALID_HANDLE_VALUE) goto done;
    if (!WriteFile(dest, data, PROFILE_SIZE, &bytes, NULL) || bytes != PROFILE_SIZE ||
        !FlushFileBuffers(dest)) goto done;
    /* Keep a read lease so another process cannot alter the mapped snapshot. */
    CloseHandle(dest); dest = INVALID_HANDLE_VALUE;
    if (!MoveFileW(temp, cache)) goto done;
    temp[0] = 0;
    snapshot_lease = CreateFileW(cache, GENERIC_READ, FILE_SHARE_READ, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (snapshot_lease == INVALID_HANDLE_VALUE) { snapshot_lease = NULL; goto done; }
    /* Verify again through the lease after reopening; a changed file is never
     * supplied to Flutter even if it was altered between write and read open. */
    if (!GetFileSizeEx(snapshot_lease, &size) || size.QuadPart != PROFILE_SIZE ||
        !ReadFile(snapshot_lease, data, PROFILE_SIZE, &bytes, NULL) || bytes != PROFILE_SIZE ||
        !sha256(data, PROFILE_SIZE, hash) || memcmp(hash, profile_patched_sha256, 32)) {
        CloseHandle(snapshot_lease); snapshot_lease = NULL; goto done;
    }
    wcscpy(prepared_path, cache);
    success = TRUE;
done:
    if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
    if (dest != INVALID_HANDLE_VALUE) CloseHandle(dest);
    if (data) HeapFree(GetProcessHeap(), 0, data);
    if (!success && temp[0]) DeleteFileW(temp);
    return success;
}

static void *guard_engine_create(const EngineProperties *properties) {
    if (!original_engine_create || !properties) return NULL;
    AcquireSRWLockExclusive(&prepare_lock);
    if (!(hook_status & 4)) {
        BOOL ads = GetPrivateProfileIntW(L"Guard", L"BlockAds", 1, ini_path) != 0;
        BOOL privacy = GetPrivateProfileIntW(L"Guard", L"HideOBSWindows", 1, ini_path) != 0;
        log_line("OopzGuard 0.1.2; profile=0.88.41; validating AOT SHA256");
        if (prepare_snapshot(properties->aot_library_path, ads)) {
            InterlockedExchange(&guard_window_filter_active, privacy && (hook_status & 2));
            InterlockedOr(&hook_status, 8);
            log_line(ads ? "Exact AOT matched; two advertisement API paths and three discovery ad keys disabled in temporary snapshot."
                         : "Exact AOT matched; BlockAds=0.");
            log_line(guard_window_filter_active ? "OBS/YY OBS window filtering enabled for Dart FFI queries only."
                             : "HideOBSWindows=0.");
        } else {
            log_line("UNSUPPORTED_OR_IO_ERROR: retaining original snapshot and window queries; no patch applied.");
        }
        InterlockedOr(&hook_status, 4);
    }
    wchar_t current_original[MAX_PATH];
    BOOL redirect = prepared_path[0] && resolve_aot_path(properties->aot_library_path, current_original) &&
                    !_wcsicmp(current_original, validated_original_path);
    EngineProperties copy;
    if (redirect) { copy = *properties; copy.aot_library_path = prepared_path; }
    ReleaseSRWLockExclusive(&prepare_lock);
    return original_engine_create(redirect ? &copy : properties);
}

static FARPROC WINAPI guard_lookup(HMODULE module, LPCSTR name) {
    FARPROC original = original_lookup(module, name);
    if (!original) return original;
    return window_filter_resolve(module, name, original);
}

static BOOL patch_import(HMODULE module, const char *name, void *replacement, void **saved) {
    if (!module) return FALSE;
    unsigned char *base = (unsigned char *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base+dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return FALSE;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return FALSE;
    IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base+rva);
    for (; desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(base+desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(base+desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *entry = (IMAGE_IMPORT_BY_NAME *)(base+names->u1.AddressOfData);
            if (strcmp((const char *)entry->Name, name)) continue;
            DWORD old, ignored;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return FALSE;
            *saved = (void *)(uintptr_t)iat->u1.Function;
            InterlockedExchangePointer((void *volatile *)&iat->u1.Function, replacement);
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &ignored);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    self_module = instance;
    DisableThreadLibraryCalls(instance);
    if (!target_host()) return TRUE;
    DWORD n = GetModuleFileNameW(instance, ini_path, MAX_PATH);
    if (!n || n >= MAX_PATH) return TRUE;
    wchar_t *base = (wchar_t *)basename_w(ini_path);
    if ((size_t)(base-ini_path)+wcslen(L"oopz_guard.ini") >= MAX_PATH) return TRUE;
    wcscpy(base, L"oopz_guard.ini");
    if (patch_import(GetModuleHandleW(NULL), "FlutterDesktopEngineCreate", guard_engine_create,
                     (void **)&original_engine_create)) InterlockedOr(&hook_status, 1);
    if (patch_import(GetModuleHandleW(L"flutter_windows.dll"), "GetProcAddress", guard_lookup,
                     (void **)&original_lookup)) InterlockedOr(&hook_status, 2);
    return TRUE;
}

static BOOL CALLBACK load_system_dwm(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once; (void)parameter; (void)context;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n+12 >= MAX_PATH) return FALSE;
    wcscat(path, L"\\dwmapi.dll");
    system_dwm = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return system_dwm != NULL && system_dwm != self_module;
}

static HRESULT WINAPI unavailable_dwm(void) { return E_NOTIMPL; }
static FARPROC unavailable_dwm_target = (FARPROC)unavailable_dwm;

/* Assembly trampolines preserve all x64 argument registers and tail-call the
 * system export. Nothing is loaded from inside DllMain. */
FARPROC *ResolveDwmExport(unsigned int ordinal) {
    if (ordinal < DWM_EXPORT_FIRST_ORDINAL || ordinal > DWM_EXPORT_LAST_ORDINAL)
        return &unavailable_dwm_target;
    FARPROC p = (FARPROC)unavailable_dwm;
    if (InitOnceExecuteOnce(&dwm_once, load_system_dwm, NULL, NULL)) {
        /* Named exports move between ordinals on different Windows versions.
         * Never fall back to an old ordinal when a named API is unavailable. */
        const char *name = dwm_export_names[ordinal - DWM_EXPORT_FIRST_ORDINAL];
        FARPROC resolved = GetProcAddress(system_dwm, name ? name : MAKEINTRESOURCEA(ordinal));
        if (resolved) p = resolved;
    }
    InterlockedCompareExchangePointer((void *volatile *)&dwm_targets[ordinal], (void *)p, NULL);
    return &dwm_targets[ordinal];
}

__declspec(dllexport) DWORD WINAPI OopzGuardStatus(void) { return (DWORD)hook_status; }
__declspec(dllexport) const char * WINAPI OopzGuardVersion(void) { return "0.1.2 / oopz 0.88.41 x64"; }
