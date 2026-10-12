#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "window_filter.h"
#include "snapshot_patch.h"
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

static HANDLE verified_cache(const wchar_t *path, DWORD length, const unsigned char expected[32]) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size; DWORD bytes=0; unsigned char actual[32];
    unsigned char *buffer=NULL; BOOL valid=FALSE;
    if(!GetFileSizeEx(file,&size)||size.QuadPart!=length) goto done;
    buffer=HeapAlloc(GetProcessHeap(),0,length);
    if(buffer && ReadFile(file,buffer,length,&bytes,NULL) && bytes==length &&
       sha256(buffer,length,actual) && !memcmp(actual,expected,32)) valid=TRUE;
done:
    if(buffer) HeapFree(GetProcessHeap(),0,buffer);
    if(!valid) { CloseHandle(file); return NULL; }
    return file;
}

static BOOL prepare_snapshot(const wchar_t *input, BOOL remove_ads, SnapshotReport *report) {
    wchar_t original[MAX_PATH],directory[MAX_PATH],temp[MAX_PATH],cache[MAX_PATH];
    unsigned char hash[32], *data=NULL;
    HANDLE source=INVALID_HANDLE_VALUE,dest=INVALID_HANDLE_VALUE;
    BOOL success=FALSE; LARGE_INTEGER size; DWORD bytes=0,length=0;
    memset(report,0,sizeof(*report)); temp[0]=0;
    if(!resolve_aot_path(input,original)) goto done;
    if(!remove_ads) { success=TRUE; goto done; }
    source=CreateFileW(original,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(source==INVALID_HANDLE_VALUE) goto done;
    /* This is an allocation bound, not a client-version or exact-size gate. */
    if(!GetFileSizeEx(source,&size)||size.QuadPart<64||size.QuadPart>512LL*1024*1024) goto done;
    length=(DWORD)size.QuadPart;
    data=HeapAlloc(GetProcessHeap(),0,length);
    if(!data||!ReadFile(source,data,length,&bytes,NULL)||bytes!=length) goto done;
    if(!snapshot_patch_image(data,length,report,1)) goto done;
    wcscpy(validated_original_path,original);
    if(!report->patched_objects) { success=TRUE;goto done; }
    /* Hashes identify and validate this dynamically generated cache. There is
     * no known-client SHA allowlist: any structurally recognized image works. */
    if(!sha256(data,length,hash)) goto done;
    DWORD n=GetTempPathW(MAX_PATH,directory);
    if(!n||n+96>=MAX_PATH) goto done;
    wcscat(directory,L"OopzGuard");
    if(!CreateDirectoryW(directory,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS) goto done;
    wchar_t hex[65];
    for(unsigned i=0;i<32;++i) swprintf(hex+2*i,3,L"%02x",hash[i]);
    swprintf(cache,MAX_PATH,L"%ls\\snapshot-p%u-%ls.so",directory,SNAPSHOT_POLICY_VERSION,hex);
    snapshot_lease=verified_cache(cache,length,hash);
    if(snapshot_lease) { wcscpy(prepared_path,cache);success=TRUE;goto done; }
    /* A damaged, unused cache can be rebuilt. Active read leases prevent its
     * replacement; an unsuccessful preparation keeps the original snapshot. */
    if(GetFileAttributesW(cache)!=INVALID_FILE_ATTRIBUTES && !DeleteFileW(cache)) goto done;
    if(!GetTempFileNameW(directory,L"ogd",0,temp)) goto done;
    dest=CreateFileW(temp,GENERIC_WRITE,0,NULL,TRUNCATE_EXISTING,FILE_ATTRIBUTE_TEMPORARY,NULL);
    if(dest==INVALID_HANDLE_VALUE) goto done;
    if(!WriteFile(dest,data,length,&bytes,NULL)||bytes!=length||!FlushFileBuffers(dest)) goto done;
    CloseHandle(dest);dest=INVALID_HANDLE_VALUE;
    if(MoveFileW(temp,cache)) temp[0]=0;
    else if(GetLastError()!=ERROR_ALREADY_EXISTS && GetLastError()!=ERROR_FILE_EXISTS) goto done;
    snapshot_lease=verified_cache(cache,length,hash);
    if(!snapshot_lease) goto done;
    wcscpy(prepared_path,cache);success=TRUE;
done:
    if(source!=INVALID_HANDLE_VALUE) CloseHandle(source);
    if(dest!=INVALID_HANDLE_VALUE) CloseHandle(dest);
    if(data) HeapFree(GetProcessHeap(),0,data);
    if(temp[0]) DeleteFileW(temp);
    return success;
}

static void *guard_engine_create(const EngineProperties *properties) {
    if (!original_engine_create || !properties) return NULL;
    AcquireSRWLockExclusive(&prepare_lock);
    if (!(hook_status & 4)) {
        BOOL ads = GetPrivateProfileIntW(L"Guard", L"BlockAds", 1, ini_path) != 0;
        BOOL privacy = GetPrivateProfileIntW(L"Guard", L"HideOBSWindows", 1, ini_path) != 0;
        SnapshotReport report;
        log_line("OopzGuard 0.2.0; adaptive Dart string discovery; no client-version whitelist.");
        /* OBS query filtering is independent of snapshot/ad compatibility. */
        InterlockedExchange(&guard_window_filter_active, privacy && (hook_status & 2));
        if (prepare_snapshot(properties->aot_library_path, ads, &report)) {
            InterlockedOr(&hook_status, 8);
            char message[192];
            snprintf(message,sizeof(message),"Adaptive snapshot: strings=%u layouts=%u class=%u api=%u fields=%u assets=%u patched=%u.",
                report.string_objects,report.layouts,report.class_id,report.api_paths,report.ad_fields,report.ad_assets,report.patched_objects);
            log_line(ads?message:"BlockAds=0; original snapshot retained.");
        } else {
            log_line("SNAPSHOT_UNRECOGNIZED_OR_IO_ERROR: original snapshot retained; OBS filtering remains independent.");
        }
        log_line(guard_window_filter_active ? "OBS/YY OBS window filtering enabled for Dart FFI queries."
                     : "OBS window filtering disabled or lookup hook unavailable.");
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
__declspec(dllexport) const char * WINAPI OopzGuardVersion(void) { return "0.2.0 / adaptive oopz x64"; }
