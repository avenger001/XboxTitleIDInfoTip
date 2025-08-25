// XboxTitleIdInfoTip.cpp – Windows 11 (x64) InfoTip handler for folders named like 8-char uppercase/digit IDs.
// Shows tooltip from %SystemRoot%\System32\XboxTitleIDs.txt (UTF-8; lines: ID=Name).
// Build (x64 Dev Prompt):
//   cl /LD /EHsc /permissive- /std:c++17 /DUNICODE /D_UNICODE XboxTitleIdInfoTip.cpp ^
//      shlwapi.lib ole32.lib uuid.lib advapi32.lib shell32.lib user32.lib propsys.lib
//
// Logs to %TEMP%\XboxTip.log for troubleshooting.

#if __has_include("pch.h")
#include "pch.h"
#endif

#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Win10+
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10
#endif

#include <windows.h>
#include <objidl.h>    // IPersistFile
#include <shlwapi.h>
#include <strsafe.h>
#include <shellapi.h>
#include <shlobj.h>    // IShellExtInit, SHCreateItemFromParsingName
#include <shobjidl.h>  // IShellItem, IShellItem::BindToHandler
#include <propkey.h>   // For property keys
#include <propvarutil.h> // For property utilities
#include <propsys.h>   // For property system
#include <comdef.h>    // For COM definitions

#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>
#include <new>

// --- Fix for DllRegisterServer not found ---
// These pragmas instruct the linker to export the required functions.
#pragma comment(linker,"/EXPORT:DllCanUnloadNow,PRIVATE")
#pragma comment(linker,"/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker,"/EXPORT:DllRegisterServer,PRIVATE")
#pragma comment(linker,"/EXPORT:DllUnregisterServer,PRIVATE")
// --- End of fix ---


// {A7C2C6B9-1B52-4E1E-9D56-2D2A9AB7D0C4}
static const CLSID CLSID_XboxTitleIdInfoTip =
{ 0xa7c2c6b9, 0x1b52, 0x4e1e, { 0x9d, 0x56, 0x2d, 0x2a, 0x9a, 0xb7, 0xd0, 0xc4 } };
const wchar_t* CLSID_STR = L"{A7C2C6B9-1B52-4E1E-9D56-2D2A9AB7D0C4}";
const wchar_t* IQI_STR = L"{00021500-0000-0000-C000-000000000046}";

// Global ref count for the DLL
static LONG g_dllRefCount = 0;

// Global instance handle for DllMain
HINSTANCE g_hInstance = nullptr;

// ---------------- Logging ----------------
// Simple logging to %TEMP%\XboxTip.log for debugging purposes.
static void LogLine(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH]{}; DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n >= MAX_PATH) return;
    StringCchCatW(path, MAX_PATH, L"XboxTip.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    StringCchVPrintfW(buf, 1024, fmt, ap);
    va_end(ap);
    DWORD wrote = 0;
    WriteFile(h, buf, (DWORD)(lstrlenW(buf) * sizeof(wchar_t)), &wrote, nullptr);
    WriteFile(h, L"\r\n", 4, &wrote, nullptr);
    CloseHandle(h);
}

// ---------------- DllMain ----------------
// DllMain is the entry point for a DLL. We use it to save our instance handle.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        g_hInstance = hModule;
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

// -------------- TXT UTF-8: reading and caching --------------
// This namespace handles reading, parsing, and caching the ID -> Name mapping file.
namespace {
    std::unordered_map<std::wstring, std::wstring> g_cache; // ID -> Name
    FILETIME g_lastWrite = {};
    std::once_flag g_once;
    std::mutex g_mutex;

    // Get the path to System32
    std::wstring GetSystem32Path() {
        wchar_t sysDir[MAX_PATH] = {};
        UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return L"C:\\Windows\\System32";
        return std::wstring(sysDir);
    }

    // Get the full path to the mapping file
    std::wstring GetMappingPath() {
        std::wstring p = GetSystem32Path();
        if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
        p += L"XboxTitleIDs.txt";
        return p;
    }

    // Check if a string is a valid 8-character uppercase alphanumeric ID
    bool Is8UpperAlnum(std::wstring s) {
        if (s.size() != 8) return false;
        for (auto& ch : s) {
            if (ch >= L'a' && ch <= L'z') ch = (wchar_t)(ch - L'a' + L'A');
            if (!((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z'))) return false;
        }
        return true;
    }

    // Read all bytes from a file
    std::string ReadAllBytes(const std::wstring& path, FILETIME* outWriteTime) {
        std::string data;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return data;
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(h, &info) && outWriteTime) *outWriteTime = info.ftLastWriteTime;
        LARGE_INTEGER size{}; if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return data; }
        if (size.QuadPart <= 0 || size.QuadPart > 10LL * 1024 * 1024) { CloseHandle(h); return data; } // 10MB cap
        std::vector<char> buf(static_cast<size_t>(size.QuadPart));
        DWORD read = 0; BOOL ok = ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr);
        CloseHandle(h);
        if (!ok) return std::string();
        return std::string(buf.data(), buf.data() + read);
    }

    // Convert a UTF-8 string to a wide character string
    std::wstring Utf8ToWide(const std::string& s) {
        if (s.empty()) return L"";
        int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
        if (needed <= 0) return L"";
        std::wstring w; w.resize(needed);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &w[0], needed);
        return w;
    }

    // Parse the mapping file content and populate the cache
    void ParseMappingLocked(const std::wstring& text) {
        g_cache.clear();
        size_t i = 0, n = text.size();
        while (i < n) {
            size_t ls = i;
            while (i < n && text[i] != L'\n' && text[i] != L'\r') ++i;
            size_t le = i;
            if (i < n && text[i] == L'\r') ++i;
            if (i < n && text[i] == L'\n') ++i;

            if (le <= ls) continue;
            std::wstring line = text.substr(ls, le - ls);

            if (ls == 0 && !line.empty() && line[0] == 0xFEFF) line.erase(0, 1);

            auto lpos = line.find_first_not_of(L" \t");
            if (lpos == std::wstring::npos) continue;
            auto rpos = line.find_last_not_of(L" \t");
            std::wstring trimmed = line.substr(lpos, rpos - lpos + 1);
            if (trimmed.empty()) continue;
            if (trimmed[0] == L'#' || trimmed[0] == L';') continue;

            size_t eq = trimmed.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring id = trimmed.substr(0, eq);
            std::wstring name = trimmed.substr(eq + 1);

            auto idl = id.find_first_not_of(L" \t");
            auto idr = id.find_last_not_of(L" \t");
            if (idl == std::wstring::npos) continue;
            id = id.substr(idl, idr - idl + 1);

            for (auto& ch : id) if (ch >= L'a' && ch <= L'z') ch = (wchar_t)(ch - L'a' + L'A');

            if (!Is8UpperAlnum(id)) continue;
            g_cache[id] = name;
        }
        LogLine(L"[Parse] Loaded %u mappings", (unsigned)g_cache.size());
    }

    // Ensure the cache is loaded, and reload if the file has been modified.
    void EnsureCacheLoaded() {
        std::call_once(g_once, [] {
            FILETIME wt{}; auto bytes = ReadAllBytes(GetMappingPath(), &wt);
            if (!bytes.empty()) {
                std::wstring wide = Utf8ToWide(bytes);
                std::lock_guard<std::mutex> lk(g_mutex);
                g_lastWrite = wt;
                ParseMappingLocked(wide);
            } else {
                LogLine(L"[Init] Mapping file not found: %s", GetMappingPath().c_str());
            }
        });
        FILETIME wt{}; auto bytes = ReadAllBytes(GetMappingPath(), &wt);
        if (!bytes.empty()) {
            bool changed = CompareFileTime(&wt, &g_lastWrite) > 0;
            if (changed) {
                std::wstring wide = Utf8ToWide(bytes);
                std::lock_guard<std::mutex> lk(g_mutex);
                g_lastWrite = wt;
                ParseMappingLocked(wide);
                LogLine(L"[Reload] Mapping file reloaded");
            }
        }
    }

    // Lookup a name for a given ID.
    std::wstring LookupName(std::wstring id) {
        for (auto& ch : id) if (ch >= L'a' && ch <= L'z') ch = (wchar_t)(ch - L'a' + L'A');
        EnsureCacheLoaded();
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_cache.find(id);
        if (it == g_cache.end()) return L"";
        return it->second;
    }

    // Get default Windows tooltip for a file/folder
    std::wstring GetDefaultTooltip(const std::wstring& path) {
        std::wstring result;
        
        // Simple fallback using Win32 API only
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                result = L"File folder";
            } else {
                LARGE_INTEGER size;
                size.LowPart = fad.nFileSizeLow;
                size.HighPart = fad.nFileSizeHigh;
                
                wchar_t sizeStr[64];
                if (size.QuadPart == 0) {
                    StringCchCopyW(sizeStr, 64, L"0 bytes");
                } else if (size.QuadPart < 1024) {
                    StringCchPrintfW(sizeStr, 64, L"%lld bytes", size.QuadPart);
                } else if (size.QuadPart < 1024 * 1024) {
                    StringCchPrintfW(sizeStr, 64, L"%.1f KB", size.QuadPart / 1024.0);
                } else if (size.QuadPart < 1024LL * 1024 * 1024) {
                    StringCchPrintfW(sizeStr, 64, L"%.1f MB", size.QuadPart / (1024.0 * 1024.0));
                } else {
                    StringCchPrintfW(sizeStr, 64, L"%.1f GB", size.QuadPart / (1024.0 * 1024.0 * 1024.0));
                }
                result = sizeStr;
                
                // Get file extension and add it to tooltip
                size_t dotPos = path.find_last_of(L'.');
                if (dotPos != std::wstring::npos && dotPos > path.find_last_of(L"\\/")) {
                    std::wstring ext = path.substr(dotPos + 1);
                    if (!ext.empty()) {
                        // Convert to uppercase
                        for (auto& ch : ext) {
                            if (ch >= L'a' && ch <= L'z') ch = (wchar_t)(ch - L'a' + L'A');
                        }
                        result = ext + L" file\r\n" + result;
                    }
                }
            }
            
            // Add modified date
            SYSTEMTIME st;
            if (FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) {
                wchar_t dateStr[128];
                if (GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, dateStr, 128) > 0) {
                    wchar_t timeStr[64];
                    if (GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, timeStr, 64) > 0) {
                        result += L"\r\nDate modified: ";
                        result += dateStr;
                        result += L" ";
                        result += timeStr;
                    }
                }
            }
        }
        
        return result;
    }
}

// -------------- COM: IQueryInfo + IPersistFile + IShellExtInit --------------
class XboxTitleIdInfoTip : public IQueryInfo, public IPersistFile, public IShellExtInit {
    LONG m_ref = 1;
    std::wstring m_path;
    
public:
    XboxTitleIdInfoTip() {
        InterlockedIncrement(&g_dllRefCount);
    }
    ~XboxTitleIdInfoTip() {
        InterlockedDecrement(&g_dllRefCount);
    }
    
    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown)            *ppv = static_cast<IQueryInfo*>(this);
        else if (riid == IID_IQueryInfo)     *ppv = static_cast<IQueryInfo*>(this);
        else if (riid == IID_IPersistFile)   *ppv = static_cast<IPersistFile*>(this);
        else if (riid == IID_IShellExtInit)  *ppv = static_cast<IShellExtInit*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return r; }

    // IPersist
    IFACEMETHODIMP GetClassID(CLSID* pClassID) override {
        if (!pClassID) return E_POINTER;
        *pClassID = CLSID_XboxTitleIdInfoTip;
        return S_OK;
    }

    // IPersistFile
    IFACEMETHODIMP IsDirty() override { return S_FALSE; }
    IFACEMETHODIMP Load(LPCOLESTR pszFileName, DWORD) override {
        m_path = pszFileName;
        LogLine(L"[Init] (IPersistFile) Path = %s", m_path.c_str());
        return S_OK;
    }
    IFACEMETHODIMP Save(LPCOLESTR, BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP SaveCompleted(LPCOLESTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetCurFile(LPOLESTR*) override { return E_NOTIMPL; }

    // IShellExtInit
    IFACEMETHODIMP Initialize(LPCITEMIDLIST pidlFolder, IDataObject* pDataObj, HKEY hKeyProgID) override {
        // Alternative initialization method - try to get path from data object
        if (pDataObj) {
            FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM stg = {};
            if (SUCCEEDED(pDataObj->GetData(&fmt, &stg))) {
                HDROP hDrop = (HDROP)stg.hGlobal;
                wchar_t szPath[MAX_PATH];
                if (DragQueryFileW(hDrop, 0, szPath, MAX_PATH)) {
                    m_path = szPath;
                    LogLine(L"[Init] (IShellExtInit) Path = %s", m_path.c_str());
                }
                ReleaseStgMedium(&stg);
            }
        }
        return S_OK;
    }

    // IQueryInfo
    IFACEMETHODIMP GetInfoFlags(DWORD* pdwFlags) override {
        if (pdwFlags) {
            *pdwFlags = 0;
        }
        return S_OK;
    }

    IFACEMETHODIMP GetInfoTip(DWORD, LPWSTR* ppszTip) override {
        LogLine(L"[Query] GetInfoTip called.");
        
        *ppszTip = nullptr;
        
        if (m_path.empty()) {
            LogLine(L"[Query] Path is empty, returning E_FAIL.");
            return E_FAIL;
        }
        
        // Check if this is a directory first
        DWORD attrs = GetFileAttributesW(m_path.c_str());
        bool isDirectory = (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY);
        
        if (isDirectory) {
            // Find the folder name
            size_t pos = m_path.find_last_of(L"\\/");
            std::wstring name;
            if (pos != std::wstring::npos) {
                name = m_path.substr(pos + 1);
            } else {
                name = m_path;
            }

            LogLine(L"[Query] Directory Name: %s", name.c_str());

            // Check if it's a valid Xbox title ID
            if (name.size() == 8) {
                bool is_id = true;
                for (wchar_t ch : name) {
                    if (!((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z'))) {
                        is_id = false;
                        break;
                    }
                }

                if (is_id) {
                    std::wstring lookupName = LookupName(name);
                    if (!lookupName.empty()) {
                        LogLine(L"[Query] Found Xbox title lookup: %s", lookupName.c_str());
                        *ppszTip = (LPWSTR)CoTaskMemAlloc((lookupName.size() + 1) * sizeof(wchar_t));
                        if (*ppszTip) {
                            StringCchCopyW(*ppszTip, lookupName.size() + 1, lookupName.c_str());
                            LogLine(L"[Query] Returning S_OK with Xbox title tooltip.");
                            return S_OK;
                        }
                    }
                }
            }
        }
        
        // If we get here, it's either not a directory, not an Xbox title ID, or no mapping found
        // Try to get the default Windows tooltip
        LogLine(L"[Query] Not an Xbox title, trying to get default tooltip.");
        std::wstring defaultTip = GetDefaultTooltip(m_path);
        
        if (!defaultTip.empty()) {
            LogLine(L"[Query] Found default tooltip: %s", defaultTip.c_str());
            *ppszTip = (LPWSTR)CoTaskMemAlloc((defaultTip.size() + 1) * sizeof(wchar_t));
            if (*ppszTip) {
                StringCchCopyW(*ppszTip, defaultTip.size() + 1, defaultTip.c_str());
                LogLine(L"[Query] Returning S_OK with default tooltip.");
                return S_OK;
            }
        }
        
        LogLine(L"[Query] No tooltip available, returning S_FALSE.");
        return S_FALSE;
    }
};

// -------------- COM: Class Factory --------------
class XboxTitleIdInfoTipFactory : public IClassFactory {
    LONG m_ref = 1;
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) *ppv = static_cast<IClassFactory*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return r; }
    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        auto tip = new (std::nothrow) XboxTitleIdInfoTip();
        if (!tip) return E_OUTOFMEMORY;
        HRESULT hr = tip->QueryInterface(riid, ppv);
        tip->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL fLock) override { 
        if (fLock) InterlockedIncrement(&g_dllRefCount);
        else InterlockedDecrement(&g_dllRefCount);
        return S_OK; 
    }
};

// COM exports
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (rclsid != CLSID_XboxTitleIdInfoTip) return CLASS_E_CLASSNOTAVAILABLE;
    auto fact = new (std::nothrow) XboxTitleIdInfoTipFactory();
    if (!fact) return E_OUTOFMEMORY;
    HRESULT hr = fact->QueryInterface(riid, ppv);
    fact->Release();
    return hr;
}
STDAPI DllCanUnloadNow() { return (g_dllRefCount > 0) ? S_FALSE : S_OK; }

// DllRegisterServer - The registration logic.
STDAPI DllRegisterServer() {
    LogLine(L"[Register] DllRegisterServer called.");
    
    // Register the CLSID for the shell extension
    HKEY hCLSID;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, L"CLSID\\{A7C2C6B9-1B52-4E1E-9D56-2D2A9AB7D0C4}", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hCLSID, nullptr) != ERROR_SUCCESS) {
        return E_FAIL;
    }
    RegSetValueExW(hCLSID, nullptr, 0, REG_SZ, (const BYTE*)L"XboxTitleIdInfoTip Class", 50);
    HKEY hInProc;
    if (RegCreateKeyExW(hCLSID, L"InProcServer32", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hInProc, nullptr) == ERROR_SUCCESS) {
        wchar_t modulePath[MAX_PATH];
        GetModuleFileNameW(g_hInstance, modulePath, MAX_PATH);
        RegSetValueExW(hInProc, nullptr, 0, REG_SZ, (const BYTE*)modulePath, (lstrlenW(modulePath) + 1) * sizeof(wchar_t));
        RegSetValueExW(hInProc, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Apartment", 20);
        RegCloseKey(hInProc);
        LogLine(L"[Register] InProcServer32 path set to: %s", modulePath);
    }
    RegCloseKey(hCLSID);
    
    // Add to approved list for security
    HKEY hApproved;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", 0, KEY_SET_VALUE, &hApproved) == ERROR_SUCCESS) {
        RegSetValueExW(hApproved, CLSID_STR, 0, REG_SZ, (const BYTE*)L"XboxTitleIdInfoTip", 38);
        RegCloseKey(hApproved);
    }

    // Register for both files and directories - we'll handle the logic internally
    HKEY hExt;
    wchar_t keyPath[256];
    
    // Register for all files
    StringCchPrintfW(keyPath, 256, L"*\\shellex\\%s", IQI_STR);
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hExt, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hExt, nullptr, 0, REG_SZ, (const BYTE*)CLSID_STR, (lstrlenW(CLSID_STR) + 1) * sizeof(wchar_t));
        RegCloseKey(hExt);
        LogLine(L"[Register] Registered for all files");
    }
    
    // Register for directories
    StringCchPrintfW(keyPath, 256, L"Directory\\shellex\\%s", IQI_STR);
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hExt, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hExt, nullptr, 0, REG_SZ, (const BYTE*)CLSID_STR, (lstrlenW(CLSID_STR) + 1) * sizeof(wchar_t));
        RegCloseKey(hExt);
        LogLine(L"[Register] Registered for directories");
    }
    
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

// DllUnregisterServer - The unregistration logic.
STDAPI DllUnregisterServer() {
    LogLine(L"[Register] DllUnregisterServer called.");
    
    // Unregister the CLSID
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{A7C2C6B9-1B52-4E1E-9D56-2D2A9AB7D0C4}");
    
    // Remove from approved list
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, CLSID_STR); RegCloseKey(h);
    }
    
    // Unregister from all locations
    wchar_t keyPath[256];
    
    StringCchPrintfW(keyPath, 256, L"*\\shellex\\%s", IQI_STR);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, keyPath);

    StringCchPrintfW(keyPath, 256, L"Directory\\shellex\\%s", IQI_STR);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, keyPath);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}