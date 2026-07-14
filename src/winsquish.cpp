/* Copyright (C) 2026  Paige Julianne Sullivan
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* ============================================================================
 * winsquish.cpp — Windows GUI front end for libsquish
 *
 * A WinZip/PeaZip-style archive manager built on one large, resizable window:
 * a toolbar (Open, Compress, Extract, Up), an address bar, a central file list,
 * and a status bar. Open a .sq or self-extracting archive to browse its folders
 * in the list and extract just the files or folders you select; Compress packs
 * a file or folder into a new .sq (or a .exe SFX) with live progress, then opens
 * the result. Files, folders, and archives can also be dropped onto the window.
 * Installs optional per-user Explorer context-menu entries:
 *
 *   any file  ->  "Compress to .sq (WinSquish)"
 *   any file  ->  "Compress to self-extracting .exe (WinSquish)"
 *   .sq file  ->  "Extract with WinSquish" (+ double-click opens the GUI)
 *
 * Self-extracting archives (SFX)
 *   Ticking "Create self-extracting archive" produces a Windows .exe instead
 *   of a .sq: WinSquish itself is used as the stub, with the compressed
 *   payload, the original file name, and a 32-byte trailer appended. Running
 *   that .exe (e.g. double-clicking it) re-launches WinSquish, which detects
 *   the trailer in its own image and extracts the payload beside itself.
 *
 *   The trailer format ("SQSFX01") is shared with the squish CLI, so an SFX
 *   built on any platform (a Linux ELF or macOS Mach-O stub included) can be
 *   extracted here: extraction only reads the trailer + payload and never
 *   needs to run the foreign stub. Only self-extraction-by-running is
 *   platform-specific; opening a foreign SFX in WinSquish always works.
 *
 * Command line:
 *   winsquish.exe [file]                 open GUI (browse an archive, else queue
 *                                        the file for compression)
 *   winsquish.exe --compress <file>      open GUI and start compressing (.sq)
 *   winsquish.exe --compress-sfx <file>  open GUI and build a .exe SFX
 *   winsquish.exe --decompress <file>    open GUI and extract to disk (.sq/SFX)
 *   winsquish.exe --view <file>          open GUI and browse the archive
 *   winsquish.exe --register             install context-menu entries (HKCU)
 *   winsquish.exe --unregister           remove them
 *   winsquish.exe --register --quiet     as above, but no confirmation dialog
 *                                        (used by the installer/uninstaller)
 *
 * Registration is per-user (HKCU\Software\Classes): no admin rights needed.
 * ==========================================================================*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <string.h>
#include <vector>
#include <algorithm>
#include <set>

#include "../squish/squish.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

/* --- constants ------------------------------------------------------------ */
static const wchar_t *APP_NAME   = L"WinSquish";
static const wchar_t *WND_CLASS  = L"WinSquishMainWindow";
static const wchar_t *PROGID     = L"WinSquish.sq";
static const wchar_t *SQ_EXT     = L".sq";

#define WM_APP_PROGRESS (WM_APP + 1)   /* wParam = percent 0..100            */
#define WM_APP_DONE     (WM_APP + 2)   /* wParam = squish_status              */

/* main-window control IDs */
#define IDC_CPU_COMBO    1004
#define IDC_PROGRESS     1007
#define IDC_SFX_CHECK    1009
#define IDC_TOOLBAR      1020
#define IDC_STATUSBAR    1021
#define IDC_HINT         1022
#define IDC_LOC_LABEL    1023
#define IDC_CPU_LABEL    1024

/* file-list / address-bar control IDs */
#define IDC_LV           1101
#define IDC_PATH_STATIC  1105

/* command IDs — menu items and toolbar buttons share the WM_COMMAND space */
#define IDM_OPEN            2001   /* open an archive to browse            */
#define IDM_EXIT            2002
#define IDM_REGISTER        2003
#define IDM_UNREGISTER      2004
#define IDM_ABOUT           2005
#define IDM_COMPRESS        2006   /* toolbar dropdown anchor              */
#define IDM_COMPRESS_FILE   2007
#define IDM_COMPRESS_FOLDER 2008
#define IDM_EXTRACT         2009   /* extract selection from open archive  */
#define IDM_EXTRACT_ALL     2010
#define IDM_UP              2011   /* go up one folder within the archive  */
#define IDM_CLOSE_ARCHIVE   2012

/* --- globals ---------------------------------------------------------------*/
static HWND      g_hwnd;                 /* the single top-level browser window */
static HWND      g_toolbar = nullptr;
static HWND      g_status  = nullptr;    /* status bar at the bottom            */
static HWND      g_list    = nullptr;    /* the central file ListView           */
static HWND      g_hint    = nullptr;    /* empty-state hint shown when idle     */
static HWND      g_progress = nullptr;   /* progress bar, embedded in the status bar */
static HIMAGELIST g_tbImages = nullptr;  /* custom toolbar glyphs (owned by us)  */
static ULONG_PTR g_gdipToken = 0;        /* GDI+ lifetime token                  */
static HFONT     g_font;
static bool      g_busy = false;
static ULONGLONG g_t0;

struct Browser;                          /* the archive currently on screen      */
static Browser     *g_browser = nullptr; /* null when no archive is open         */
static std::wstring g_source;            /* file/folder queued for compression   */

struct Job {
    HWND         hwnd;
    std::wstring src, dst;
    bool         compress;   /* true = compress, false = extract              */
    bool         sfx;        /* compress: build .exe SFX; extract: src is SFX */
    bool         srcIsDir;   /* compress: source is a folder (SQAR archive)   */
    bool         tree;       /* extract: output is a directory tree (archive) */
    bool         listing;    /* decompress the stream into `blob` for browsing*/
    std::string  blob;       /* listing: decompressed bytes (legacy / file)   */
    int          threads;    /* worker count; 1 = single-block (best ratio)   */
    volatile LONG lastPct;
};
static Job *g_job = nullptr;

/* Default worker count when nothing else is chosen. */
#define DEFAULT_THREADS 4

/* --- small helpers ---------------------------------------------------------*/
static std::string ToUtf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring ExePath(void) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

static bool HasSqExt(const std::wstring &path) {
    size_t n = path.size(), e = wcslen(SQ_EXT);
    return n > e && _wcsicmp(path.c_str() + n - e, SQ_EXT) == 0;
}

static long long FileSize(const std::wstring &path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
        return -1;
    return ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

static std::wstring PrettySize(long long n) {
    wchar_t buf[64];
    if (n < 0)                    return L"?";
    if (n < 1024)                 swprintf(buf, 64, L"%lld bytes", n);
    else if (n < 1024 * 1024)     swprintf(buf, 64, L"%.1f KB", n / 1024.0);
    else if (n < 1024LL * 1024 * 1024)
                                  swprintf(buf, 64, L"%.1f MB", n / 1048576.0);
    else                          swprintf(buf, 64, L"%.2f GB", n / 1073741824.0);
    return buf;
}

/* --- self-extracting archive (SFX) container -------------------------------
 * Byte-compatible with the squish CLI's "SQSFX01" format:
 *
 *     [ stub executable ][ payload ][ name ][ 32-byte trailer ]
 *
 * payload is a plain SQUISH stream (exactly what squish_compress emits); name
 * is the raw UTF-8 basename of the original file (NOT null-terminated); the
 * trailer is little-endian:
 *
 *     off  size  field
 *       0     8  magic "SQSFX01\n"
 *       8     8  payload_off  (u64)  = stub length / offset of payload
 *      16     8  payload_len  (u64)
 *      24     4  name_len     (u32)
 *      28     4  flags        (u32)  = 0
 *
 * The trailer sits at end-of-file, so probing is platform-agnostic: a stub
 * built as a PE, ELF, or Mach-O binary is irrelevant to extraction. -------- */
static const unsigned char SFX_MAGIC[8] =
    { 'S','Q','S','F','X','0','1','\n' };
#define SFX_MAGIC_LEN   8u
#define SFX_TRAILER_LEN 32u
#define SFX_MAX_NAME    4096u

static void PutU32LE(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void PutU64LE(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t GetU32LE(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t GetU64LE(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

struct SfxInfo {
    uint64_t  payloadOff;
    uint64_t  payloadLen;
    uint32_t  nameLen;
    long long fileSize;
};

/* Open a file for shared reading — tolerant enough to read our own running
 * image (which the loader keeps open). */
static HANDLE OpenShared(const std::wstring &path) {
    return CreateFileW(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
}

/* Validate and read the SFX trailer of `path`. Returns true for a well-formed
 * archive (any platform's stub). Mirrors the CLI's sfx_probe checks. */
static bool ProbeSfx(const std::wstring &path, SfxInfo *info) {
    HANDLE h = OpenShared(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) { CloseHandle(h); return false; }
    long long sz = li.QuadPart;
    if (sz < (long long)SFX_TRAILER_LEN) { CloseHandle(h); return false; }
    LARGE_INTEGER pos; pos.QuadPart = sz - (long long)SFX_TRAILER_LEN;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); return false; }
    unsigned char t[SFX_TRAILER_LEN];
    DWORD got = 0;
    BOOL ok = ReadFile(h, t, SFX_TRAILER_LEN, &got, nullptr);
    CloseHandle(h);
    if (!ok || got != SFX_TRAILER_LEN) return false;
    if (memcmp(t, SFX_MAGIC, SFX_MAGIC_LEN) != 0) return false;
    uint64_t off  = GetU64LE(t + 8);
    uint64_t plen = GetU64LE(t + 16);
    uint32_t nlen = GetU32LE(t + 24);
    if (off == 0 || plen == 0 || nlen > SFX_MAX_NAME) return false;
    uint64_t tail = plen + (uint64_t)nlen + SFX_TRAILER_LEN;
    if (tail > (uint64_t)sz || off != (uint64_t)sz - tail) return false;
    info->payloadOff = off; info->payloadLen = plen;
    info->nameLen = nlen;   info->fileSize = sz;
    return true;
}

/* Read the stored (UTF-8) original name out of an SFX into a wide string. */
static bool ReadSfxName(const std::wstring &path, const SfxInfo &info,
                        std::wstring *outName) {
    outName->clear();
    if (info.nameLen == 0) return true;
    HANDLE h = OpenShared(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos;
    pos.QuadPart = (long long)(info.payloadOff + info.payloadLen);
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); return false; }
    std::string raw(info.nameLen, '\0');
    DWORD got = 0;
    BOOL ok = ReadFile(h, &raw[0], info.nameLen, &got, nullptr);
    CloseHandle(h);
    if (!ok || got != info.nameLen) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &w[0], n);
    *outName = w;
    return true;
}

/* Path helpers. */
static std::wstring Basename(const std::wstring &path) {
    size_t s = path.find_last_of(L"\\/:");
    return (s == std::wstring::npos) ? path : path.substr(s + 1);
}
static std::wstring DirWithSlash(const std::wstring &path) {
    size_t s = path.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring() : path.substr(0, s + 1);
}

/* Reduce a stored name to a safe basename in the output directory (blocks any
 * path traversal); matches the CLI's sfx_basename fallback. */
static std::wstring SafeStoredName(const std::wstring &stored) {
    std::wstring b = Basename(stored);
    if (b.empty() || b == L"." || b == L"..") return L"extracted.out";
    return b;
}

/* Default output path when building an SFX: swap the source's final extension
 * for ".exe", keeping its directory. Never collides with the source. */
static std::wstring SfxOutputPath(const std::wstring &src) {
    std::wstring dir = DirWithSlash(src), base = Basename(src);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) base = base.substr(0, dot);
    std::wstring out = dir + base + L".exe";
    if (_wcsicmp(out.c_str(), src.c_str()) == 0) out = src + L".exe";
    return out;
}

/* Length of our own executable's stub — i.e. everything before any SFX we may
 * already carry, so building an SFX from a self-extracting build re-uses the
 * clean stub instead of nesting payloads. */
static bool OwnStubLength(uint64_t *stubLen) {
    std::wstring self = ExePath();
    long long sz = FileSize(self);
    if (sz < 0) return false;
    SfxInfo info;
    *stubLen = ProbeSfx(self, &info) ? info.payloadOff : (uint64_t)sz;
    return true;
}

/* A scratch path in %TEMP% for the intermediate .sq payload. Empty on error. */
static std::wstring MakeTempPath(void) {
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return std::wstring();
    wchar_t path[MAX_PATH];
    if (!GetTempFileNameW(dir, L"wsq", 0, path)) return std::wstring();
    return path;
}

/* Copy `len` bytes from `in` (starting at `start`) to the current position of
 * `out`. Used to lay down the stub and payload without loading them whole. */
static bool CopyRange(HANDLE in, HANDLE out, uint64_t start, uint64_t len) {
    LARGE_INTEGER pos; pos.QuadPart = (long long)start;
    if (!SetFilePointerEx(in, pos, nullptr, FILE_BEGIN)) return false;
    const DWORD CH = 1u << 20;
    std::string buf(CH, '\0');
    uint64_t left = len;
    while (left) {
        DWORD want = (DWORD)(left < CH ? left : CH), got = 0;
        if (!ReadFile(in, &buf[0], want, &got, nullptr) || got == 0) return false;
        for (DWORD off = 0; off < got; ) {
            DWORD wr = 0;
            if (!WriteFile(out, buf.data() + off, got - off, &wr, nullptr)) return false;
            off += wr;
        }
        left -= got;
    }
    return true;
}

/* Assemble outPath = [stub][payload][name][trailer]. On any failure the
 * partial output is deleted. */
static int WriteSfx(uint64_t stubLen, const std::wstring &payloadPath,
                    uint64_t payloadLen, const std::string &nameUtf8,
                    const std::wstring &outPath) {
    HANDLE in = OpenShared(ExePath());
    if (in == INVALID_HANDLE_VALUE) return SQUISH_E_IO;
    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return SQUISH_E_IO; }

    int rc = SQUISH_OK;
    if (!CopyRange(in, out, 0, stubLen)) rc = SQUISH_E_IO;
    CloseHandle(in);

    if (rc == SQUISH_OK) {
        HANDLE pf = OpenShared(payloadPath);
        if (pf == INVALID_HANDLE_VALUE) rc = SQUISH_E_IO;
        else { if (!CopyRange(pf, out, 0, payloadLen)) rc = SQUISH_E_IO;
               CloseHandle(pf); }
    }
    if (rc == SQUISH_OK && !nameUtf8.empty()) {
        DWORD wr = 0;
        if (!WriteFile(out, nameUtf8.data(), (DWORD)nameUtf8.size(), &wr, nullptr) ||
            wr != nameUtf8.size()) rc = SQUISH_E_IO;
    }
    if (rc == SQUISH_OK) {
        unsigned char tr[SFX_TRAILER_LEN] = { 0 };
        memcpy(tr, SFX_MAGIC, SFX_MAGIC_LEN);
        PutU64LE(tr + 8,  stubLen);
        PutU64LE(tr + 16, payloadLen);
        PutU32LE(tr + 24, (uint32_t)nameUtf8.size());
        PutU32LE(tr + 28, 0);
        DWORD wr = 0;
        if (!WriteFile(out, tr, SFX_TRAILER_LEN, &wr, nullptr) ||
            wr != SFX_TRAILER_LEN) rc = SQUISH_E_IO;
    }
    CloseHandle(out);
    if (rc != SQUISH_OK) DeleteFileW(outPath.c_str());
    return rc;
}

/* --- directory archives ("SQAR02", docs FORMAT.md §12) ---------------------
 * A directory is packed into a seekable SQAR archive entirely by libsquish
 * (squish_archive_create): each file becomes its own compressed stream behind
 * a compact index, so a reader can list members and inflate one file or subtree
 * by seeking straight to it — never touching the rest. WinSquish no longer
 * serializes the tree itself; it just calls the archive API for creation,
 * listing (the browser), and extraction. The small helpers below (UTF-8
 * conversion, whole-file/range I/O, mkdir -p) remain because the SFX container
 * and the browser's own file writes still need them. */

/* UTF-8 (n bytes) -> wide. */
static std::wstring FromUtf8(const char *s, int n) {
    if (n <= 0) return std::wstring();
    int w = MultiByteToWideChar(CP_UTF8, 0, s, n, nullptr, 0);
    if (w <= 0) return std::wstring();
    std::wstring r(w, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, n, &r[0], w);
    return r;
}

static bool IsDirectory(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* Read an entire file into `out`. false on open/read failure or if it does not
 * fit in memory (size_t). */
static bool ReadWholeFile(const std::wstring &path, std::string *out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0 ||
        (unsigned long long)li.QuadPart > (size_t)-1) { CloseHandle(h); return false; }
    out->resize((size_t)li.QuadPart);
    size_t left = out->size(), off = 0;
    bool ok = true;
    while (left) {
        DWORD want = (DWORD)(left < (1u << 20) ? left : (1u << 20)), got = 0;
        if (!ReadFile(h, &(*out)[off], want, &got, nullptr) || got == 0) { ok = false; break; }
        off += got; left -= got;
    }
    CloseHandle(h);
    return ok;
}

/* Read [off, off+len) of `path` into `out`. */
static bool ReadRange(const std::wstring &path, uint64_t off, uint64_t len,
                      std::string *out) {
    if (len > (size_t)-1) return false;
    HANDLE h = OpenShared(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos; pos.QuadPart = (long long)off;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); return false; }
    out->resize((size_t)len);
    size_t leftb = (size_t)len, o = 0;
    bool ok = true;
    while (leftb) {
        DWORD want = (DWORD)(leftb < (1u << 20) ? leftb : (1u << 20)), got = 0;
        if (!ReadFile(h, &(*out)[o], want, &got, nullptr) || got == 0) { ok = false; break; }
        o += got; leftb -= got;
    }
    CloseHandle(h);
    return ok;
}

/* Write `len` bytes to `path` (overwriting). 0 on success, -1 on failure (the
 * partial file is removed). */
static int WriteWholeFile(const std::wstring &path, const void *data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    const unsigned char *p = (const unsigned char *)data;
    size_t left = len;
    bool ok = true;
    while (left) {
        DWORD want = (DWORD)(left < (1u << 20) ? left : (1u << 20)), wr = 0;
        if (!WriteFile(h, p, want, &wr, nullptr) || wr == 0) { ok = false; break; }
        p += wr; left -= wr;
    }
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok ? 0 : -1;
}

/* Create `path` and every missing parent directory. Absolute paths only.
 * Returns true once `path` exists as a directory. */
static bool MakeDirTree(const std::wstring &path) {
    if (path.empty()) return false;
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == L'\\' || path[i] == L'/') {
            std::wstring sub = path.substr(0, i);
            CreateDirectoryW(sub.c_str(), nullptr);   /* ignore "already exists" */
        }
    CreateDirectoryW(path.c_str(), nullptr);
    return IsDirectory(path);
}


/* --- context-menu registration (Software\Classes) --------------------------
 * Registration is written under one of two roots:
 *   HKEY_CURRENT_USER  — a per-user install: no admin rights, this user only.
 *   HKEY_LOCAL_MACHINE — a system-wide install: all users, needs elevation.
 * The key layout below is identical either way; only the root differs. The
 * installer picks the root (all-users vs. per-user) and passes --allusers to
 * winsquish --register/--unregister; the GUI's Tools menu always uses HKCU. */
static bool SetRegValue(HKEY root, const std::wstring &key, const wchar_t *name,
                        const std::wstring &val) {
    HKEY hk;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(hk, name, 0, REG_SZ, (const BYTE *)val.c_str(),
                             (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
}

static bool RegisterShell(HKEY root) {
    std::wstring exe = ExePath();
    std::wstring quoted = L"\"" + exe + L"\"";
    bool ok = true;

    /* "Compress to .sq" on every file type */
    std::wstring k = L"Software\\Classes\\*\\shell\\WinSquish.Compress";
    ok &= SetRegValue(root,k, nullptr, L"Compress to .sq (WinSquish)");
    ok &= SetRegValue(root,k, L"Icon", quoted);
    ok &= SetRegValue(root,k + L"\\command", nullptr, quoted + L" --compress \"%1\"");

    /* "Compress to self-extracting .exe" on every file type */
    std::wstring ksfx = L"Software\\Classes\\*\\shell\\WinSquish.CompressSfx";
    ok &= SetRegValue(root,ksfx, nullptr,
                      L"Compress to self-extracting .exe (WinSquish)");
    ok &= SetRegValue(root,ksfx, L"Icon", quoted);
    ok &= SetRegValue(root,ksfx + L"\\command", nullptr,
                      quoted + L" --compress-sfx \"%1\"");

    /* The same two verbs on folders (right-click a directory): the whole tree
     * is packed into a single SQAR archive stream. */
    std::wstring kd = L"Software\\Classes\\Directory\\shell\\WinSquish.Compress";
    ok &= SetRegValue(root,kd, nullptr, L"Compress to .sq (WinSquish)");
    ok &= SetRegValue(root,kd, L"Icon", quoted);
    ok &= SetRegValue(root,kd + L"\\command", nullptr, quoted + L" --compress \"%1\"");

    std::wstring kdsfx =
        L"Software\\Classes\\Directory\\shell\\WinSquish.CompressSfx";
    ok &= SetRegValue(root,kdsfx, nullptr,
                      L"Compress to self-extracting .exe (WinSquish)");
    ok &= SetRegValue(root,kdsfx, L"Icon", quoted);
    ok &= SetRegValue(root,kdsfx + L"\\command", nullptr,
                      quoted + L" --compress-sfx \"%1\"");

    /* .sq extension -> ProgID */
    ok &= SetRegValue(root,L"Software\\Classes\\" + std::wstring(SQ_EXT),
                      nullptr, PROGID);

    /* ProgID: icon, double-click opens GUI, "View files" + "Extract" verbs */
    std::wstring p = L"Software\\Classes\\" + std::wstring(PROGID);
    ok &= SetRegValue(root,p, nullptr, L"SQUISH Compressed File");
    ok &= SetRegValue(root,p + L"\\DefaultIcon", nullptr, quoted + L",0");
    ok &= SetRegValue(root,p + L"\\shell\\open\\command", nullptr,
                      quoted + L" \"%1\"");
    ok &= SetRegValue(root,p + L"\\shell\\view", nullptr,
                      L"View files with WinSquish");
    ok &= SetRegValue(root,p + L"\\shell\\view", L"Icon", quoted);
    ok &= SetRegValue(root,p + L"\\shell\\view\\command", nullptr,
                      quoted + L" --view \"%1\"");
    ok &= SetRegValue(root,p + L"\\shell\\extract", nullptr,
                      L"Extract with WinSquish");
    ok &= SetRegValue(root,p + L"\\shell\\extract", L"Icon", quoted);
    ok &= SetRegValue(root,p + L"\\shell\\extract\\command", nullptr,
                      quoted + L" --decompress \"%1\"");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

static void UnregisterShell(HKEY root) {
    RegDeleteTreeW(root,
                   L"Software\\Classes\\*\\shell\\WinSquish.Compress");
    RegDeleteTreeW(root,
                   L"Software\\Classes\\*\\shell\\WinSquish.CompressSfx");
    RegDeleteTreeW(root,
                   L"Software\\Classes\\Directory\\shell\\WinSquish.Compress");
    RegDeleteTreeW(root,
                   L"Software\\Classes\\Directory\\shell\\WinSquish.CompressSfx");
    RegDeleteTreeW(root,
                   (L"Software\\Classes\\" + std::wstring(PROGID)).c_str());
    /* remove the extension mapping only if it still points at us */
    HKEY hk;
    std::wstring extKey = L"Software\\Classes\\" + std::wstring(SQ_EXT);
    if (RegOpenKeyExW(root, extKey.c_str(), 0,
                      KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        wchar_t val[64] = L"";
        DWORD cb = sizeof val, type = 0;
        RegQueryValueExW(hk, nullptr, nullptr, &type, (BYTE *)val, &cb);
        RegCloseKey(hk);
        if (type == REG_SZ && _wcsicmp(val, PROGID) == 0)
            RegDeleteTreeW(root, extKey.c_str());
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

/* --- worker thread ----------------------------------------------------------*/
static void SquishProgress(uint64_t done, uint64_t total, void *user) {
    Job *job = (Job *)user;
    LONG pct = total ? (LONG)(100.0 * (double)done / (double)total) : 100;
    if (InterlockedExchange(&job->lastPct, pct) != pct)
        PostMessageW(job->hwnd, WM_APP_PROGRESS, (WPARAM)pct, 0);
}

/* Classify the payload beginning at `off` in `path`: 'a' = SQAR archive,
 * 's' = plain §1 stream, 0 = neither. Reads only the first bytes. */
static char ProbePayloadKind(const std::wstring &path, uint64_t off) {
    HANDLE h = OpenShared(path);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER pos; pos.QuadPart = (long long)off;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); return 0; }
    unsigned char hd[16]; DWORD got = 0;
    BOOL ok = ReadFile(h, hd, sizeof hd, &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 12) return 0;
    if (squish_archive_probe(hd, (size_t)got)) return 'a';
    uint64_t sz;
    if (squish_decompressed_size(hd, (size_t)got, &sz) == SQUISH_OK) return 's';
    return 0;
}

/* One archive member, as listed by the browser. `index` is a SQAR02 member
 * index (for squish_archive_extract); `dataOff`/`size` locate a legacy member's
 * bytes inside a decompressed blob. Only one applies per archive kind. */
struct ArcEntry {
    std::wstring path;     /* relative, '/'-separated                         */
    bool         isDir;
    uint64_t     size;     /* uncompressed size (0 for a directory)           */
    uint64_t     index;    /* SQAR02 member index                             */
    uint64_t     dataOff;  /* legacy: offset of bytes within the blob         */
};

/* --- legacy "SQAR01" archives (pre-SQAR02 winsquish) ------------------------
 * Older winsquish builds serialized a whole directory tree into one "SQAR01"
 * blob and compressed it as a single stream, so there is no random access:
 * libsquish's SQAR02 reader cannot read them. We keep just enough read-only
 * code to browse and extract those older .sq files — decompress the whole
 * stream, then parse the blob in memory. New archives use SQAR02 and never take
 * this path. Layout: magic[8]="SQAR01\n\x1a" | version u32(1) | flags u32 |
 * count u64, then per entry: type u8 | mode u32 | plen u32 | dlen u64 | path |
 * data. */
static const unsigned char SQAR1_MAGIC[8] =
    { 'S','Q','A','R','0','1','\n','\x1a' };
#define SQAR1_HDR     24u
#define SQAR1_ENT     17u
#define SQAR1_MAXPATH 65535u

static bool LegacyIs(const void *b, size_t n) {
    return n >= SQAR1_HDR && memcmp(b, SQAR1_MAGIC, 8) == 0 &&
           GetU32LE((const unsigned char *)b + 8) == 1;
}

/* A stored path is safe iff relative with no empty, ".", "..", '\\' or ':'
 * component — so a legacy archive can never write outside the extraction root. */
static bool LegacyPathSafe(const char *p, size_t n) {
    if (n == 0 || p[0] == '/') return false;
    for (size_t i = 0; i < n; ) {
        size_t j = i;
        while (j < n && p[j] != '/') j++;
        size_t clen = j - i;
        if (clen == 0) return false;
        if (clen == 1 && p[i] == '.') return false;
        if (clen == 2 && p[i] == '.' && p[i + 1] == '.') return false;
        for (size_t k = i; k < j; k++) {
            unsigned char c = (unsigned char)p[k];
            if (c == '\\' || c == ':' || c == '\0') return false;
        }
        i = (j < n) ? j + 1 : j;
    }
    return true;
}

/* Parse a decompressed SQAR01 blob into `out` (paths kept '/'-separated;
 * dataOff points into `b`). SQUISH_OK or SQUISH_E_FORMAT. */
static int LegacyList(const unsigned char *b, size_t len,
                      std::vector<ArcEntry> *out) {
    if (!LegacyIs(b, len)) return SQUISH_E_FORMAT;
    uint64_t count = GetU64LE(b + 16);
    size_t off = SQAR1_HDR;
    out->clear();
    for (uint64_t i = 0; i < count; i++) {
        if (len - off < SQAR1_ENT) return SQUISH_E_FORMAT;
        unsigned char type = b[off];
        uint32_t plen = GetU32LE(b + off + 5);
        uint64_t dlen = GetU64LE(b + off + 9);
        off += SQAR1_ENT;
        if (plen == 0 || plen > SQAR1_MAXPATH || plen > len - off)
            return SQUISH_E_FORMAT;
        const char *path = (const char *)(b + off);
        if (!LegacyPathSafe(path, plen)) return SQUISH_E_FORMAT;
        ArcEntry e;
        e.path = FromUtf8(path, (int)plen);
        e.index = 0;
        off += plen;
        if (type == 1) {                                   /* directory */
            if (dlen != 0) return SQUISH_E_FORMAT;
            e.isDir = true; e.size = 0; e.dataOff = 0;
        } else if (type == 0) {                            /* file */
            if (dlen > len - off) return SQUISH_E_FORMAT;
            e.isDir = false; e.size = dlen; e.dataOff = off;
            off += (size_t)dlen;
        } else {
            return SQUISH_E_FORMAT;
        }
        out->push_back(std::move(e));
    }
    if (off != len) return SQUISH_E_FORMAT;
    return SQUISH_OK;
}

/* Unpack a decompressed legacy SQAR01 blob into the directory `outRoot`. */
static int LegacyUnpack(const unsigned char *b, size_t len,
                        const std::wstring &outRoot) {
    std::vector<ArcEntry> ents;
    int rc = LegacyList(b, len, &ents);
    if (rc != SQUISH_OK) return rc;
    if (!MakeDirTree(outRoot)) return SQUISH_E_IO;
    for (const ArcEntry &e : ents) {
        std::wstring rel = e.path;
        for (wchar_t &c : rel) if (c == L'/') c = L'\\';
        std::wstring full = outRoot + L"\\" + rel;
        if (e.isDir) {
            if (!MakeDirTree(full)) return SQUISH_E_IO;
        } else {
            size_t s = full.find_last_of(L'\\');
            if (s != std::wstring::npos && !MakeDirTree(full.substr(0, s)))
                return SQUISH_E_IO;
            if (WriteWholeFile(full, b + e.dataOff, (size_t)e.size) != 0)
                return SQUISH_E_IO;
        }
    }
    return SQUISH_OK;
}

/* Open the archive browser for a seekable SQAR02 (or single stream) source.
 * Defined near the browser window code below. */
static void OpenArchiveBrowser(const std::wstring &archivePath, int threads);
/* Open the browser from an already-decompressed blob (a legacy SQAR01 tree, or
 * a single file's contents). Takes ownership of `blob`. */
static void OpenBlobBrowser(const std::wstring &archivePath, std::string blob,
                            int threads);

/* Compress job->src to `outPath`. A directory becomes a seekable SQAR archive
 * (each file its own stream behind an index); a single file becomes a plain
 * SQUISH stream. libsquish does all the archive work. */
static int CompressToStream(Job *job, const std::wstring &outPath) {
    std::string src = ToUtf8(job->src), out = ToUtf8(outPath);
    if (IsDirectory(job->src))
        return squish_archive_create(src.c_str(), out.c_str(),
                                     job->threads, 0, SquishProgress, job);
    return job->threads > 1
         ? squish_compress_file_mt(src.c_str(), out.c_str(), job->threads, 0,
                                   SquishProgress, job)
         : squish_compress_file2(src.c_str(), out.c_str(), SquishProgress, job);
}

/* Extract a whole SQAR archive (every member) into the directory job->dst.
 * For an SFX the archive image is in memory (`data`/`len`, the payload region);
 * for a plain .sq we open the file by path so libsquish seeks per member. */
static int ExtractArchive(Job *job, const void *data, size_t len) {
    squish_archive *a = nullptr;
    int rc;
    if (job->sfx) {
        rc = squish_archive_open_memory(data, len, &a);
    } else {
        std::string src = ToUtf8(job->src);
        rc = squish_archive_open(src.c_str(), &a);
    }
    if (rc != SQUISH_OK) return rc;
    if (!MakeDirTree(job->dst)) { squish_archive_close(a); return SQUISH_E_IO; }
    std::string dst = ToUtf8(job->dst);
    rc = squish_archive_extract_subtree(a, nullptr, dst.c_str(),
                                        SquishProgress, job);
    squish_archive_close(a);
    return rc;
}

/* Extract a §1 stream. The decompressed bytes are usually a single file written
 * to job->dst — but an older winsquish archive decodes to a legacy SQAR01 tree,
 * which we detect and unpack into job->dst as a directory instead. For an SFX
 * the compressed bytes are in memory (`data`/`len`); for a plain .sq we read the
 * file. Decompressing to a buffer (not straight to disk) lets us sniff SQAR01. */
static int ExtractSingle(Job *job, const void *data, size_t len) {
    std::string comp;
    if (!job->sfx) {
        if (!ReadWholeFile(job->src, &comp)) return SQUISH_E_IO;
        data = comp.data(); len = comp.size();
    }
    void *raw = nullptr; size_t rn = 0;
    int rc = squish_decompress_alloc_mt(data, len, &raw, &rn,
                                        job->threads, SquishProgress, job);
    if (rc != SQUISH_OK) return rc;
    if (LegacyIs(raw, rn))
        rc = LegacyUnpack((const unsigned char *)raw, rn, job->dst);
    else
        rc = WriteWholeFile(job->dst, raw, rn) == 0 ? SQUISH_OK : SQUISH_E_IO;
    squish_free(raw);
    return rc;
}

/* Build job->dst as a self-extracting .exe: compress the source (file or
 * directory) to a scratch payload (a §1 stream for a file, a whole SQAR archive
 * for a directory — the slow part, over which progress is reported), then
 * splice [stub][payload][name][trailer] into the output. */
static int SfxCompress(Job *job) {
    std::wstring tmp = MakeTempPath();
    if (tmp.empty()) return SQUISH_E_IO;
    int rc = CompressToStream(job, tmp);
    if (rc != SQUISH_OK) { DeleteFileW(tmp.c_str()); return rc; }

    uint64_t stubLen = 0;
    long long plen = FileSize(tmp);
    std::string name = ToUtf8(SafeStoredName(job->src));
    if (name.size() > SFX_MAX_NAME) name.resize(SFX_MAX_NAME);
    if (!OwnStubLength(&stubLen) || plen < 0) {
        DeleteFileW(tmp.c_str());
        return SQUISH_E_IO;
    }
    rc = WriteSfx(stubLen, tmp, (uint64_t)plen, name, job->dst);
    DeleteFileW(tmp.c_str());
    return rc;
}

/* Run an extraction job: read the SFX payload region into memory if needed,
 * then dispatch on job->tree (archive tree vs. single file). */
static int DoExtract(Job *job) {
    std::string payload;
    const void *data = nullptr; size_t len = 0;
    if (job->sfx) {
        SfxInfo info;
        if (!ProbeSfx(job->src, &info)) return SQUISH_E_FORMAT;
        if (!ReadRange(job->src, info.payloadOff, info.payloadLen, &payload))
            return SQUISH_E_IO;
        data = payload.data(); len = payload.size();
    }
    return job->tree ? ExtractArchive(job, data, len)
                     : ExtractSingle(job, data, len);
}

/* Listing job for the browser: decompress a §1 stream (a legacy archive or a
 * single file) wholly into job->blob, so the browser can list/extract from it.
 * Only used for non-seekable sources; SQAR02 archives open without this. */
static int ListStream(Job *job) {
    std::string comp;
    const void *data; size_t len;
    if (job->sfx) {
        SfxInfo info;
        if (!ProbeSfx(job->src, &info)) return SQUISH_E_FORMAT;
        if (!ReadRange(job->src, info.payloadOff, info.payloadLen, &comp))
            return SQUISH_E_IO;
    } else if (!ReadWholeFile(job->src, &comp)) {
        return SQUISH_E_IO;
    }
    data = comp.data(); len = comp.size();
    void *raw = nullptr; size_t rn = 0;
    int rc = squish_decompress_alloc_mt(data, len, &raw, &rn,
                                        job->threads, SquishProgress, job);
    if (rc != SQUISH_OK) return rc;
    job->blob.assign((const char *)raw, rn);
    squish_free(raw);
    return SQUISH_OK;
}

static DWORD WINAPI WorkerProc(LPVOID param) {
    Job *job = (Job *)param;
    int rc = job->listing ? ListStream(job)
           : job->compress
           ? (job->sfx ? SfxCompress(job) : CompressToStream(job, job->dst))
           : DoExtract(job);
    PostMessageW(job->hwnd, WM_APP_DONE, (WPARAM)rc, 0);
    return 0;
}

/* --- UI --------------------------------------------------------------------*/
/* The status bar carries three parts: [ status text | progress | counts ]. */
static void SetStatus(const std::wstring &s) {
    if (g_status) SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)s.c_str());
}
static void SetCounts(const std::wstring &s) {
    if (g_status) SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)s.c_str());
}

/* Enable/disable the input chrome while a background job runs. */
static void SetBusy(bool busy) {
    g_busy = busy;
    EnableWindow(g_toolbar, !busy);
    EnableWindow(GetDlgItem(g_hwnd, IDC_CPU_COMBO), !busy);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SFX_CHECK), !busy);
    ShowWindow(g_progress, busy ? SW_SHOW : SW_HIDE);
    if (!busy)
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
}

/* Show, in the status bar, what the queued compression source is. */
static void RefreshSourceStatus(void) {
    if (g_source.empty()) return;
    bool isDir = IsDirectory(g_source);
    bool sfxMode = IsDlgButtonChecked(g_hwnd, IDC_SFX_CHECK) == BST_CHECKED;
    std::wstring out = sfxMode ? SfxOutputPath(g_source) : g_source + SQ_EXT;
    std::wstring info = (isDir ? L"Folder \"" : L"File \"") +
            Basename(g_source) + L"\" ready — Compress writes \"" +
            Basename(out) + L"\"";
    SetStatus(info);
}

/* Worker count chosen in the CPU-cores combo (items are "1".."ncpu", so the
 * value is the selection index + 1). Falls back to the default if unset. */
static int SelectedThreads(void) {
    LRESULT sel = SendDlgItemMessageW(g_hwnd, IDC_CPU_COMBO, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        int ncpu = squish_threads();
        return ncpu < DEFAULT_THREADS ? ncpu : DEFAULT_THREADS;
    }
    return (int)sel + 1;
}

/* Derive the output path for a job. Compress: append .sq.
 * Extract: strip .sq, or append .out if the name has no .sq suffix. */
static std::wstring OutputPath(const std::wstring &src, bool compress) {
    if (compress) return src + SQ_EXT;
    if (HasSqExt(src)) return src.substr(0, src.size() - wcslen(SQ_EXT));
    return src + L".out";
}

static void StartJob(bool compress, const std::wstring &src) {
    if (g_busy) return;
    if (src.empty()) {
        MessageBoxW(g_hwnd, L"Choose a file or folder first.", APP_NAME,
                    MB_ICONINFORMATION);
        return;
    }
    bool srcIsDir = IsDirectory(src);
    if (!srcIsDir && FileSize(src) < 0) {
        MessageBoxW(g_hwnd, (L"File not found:\n" + src).c_str(), APP_NAME,
                    MB_ICONERROR);
        return;
    }
    if (srcIsDir && !compress) {
        MessageBoxW(g_hwnd, L"A folder can be compressed, but not extracted.\n"
                            L"Choose a .sq stream or self-extracting archive to "
                            L"extract.", APP_NAME, MB_ICONINFORMATION);
        return;
    }

    /* Decide direction, whether SFX is involved, whether the output is a
     * directory tree (an archive) or a single file, and the output path. */
    bool sfx = false, tree = false;
    std::wstring dst;
    if (compress) {
        sfx = IsDlgButtonChecked(g_hwnd, IDC_SFX_CHECK) == BST_CHECKED;
        tree = srcIsDir;
        dst = sfx ? SfxOutputPath(src) : src + SQ_EXT;
    } else {
        SfxInfo si;
        if (ProbeSfx(src, &si)) {
            sfx = true;
            tree = ProbePayloadKind(src, si.payloadOff) == 'a';
            std::wstring stored;
            ReadSfxName(src, si, &stored);
            dst = DirWithSlash(src) + SafeStoredName(stored);
        } else {
            char kind = ProbePayloadKind(src, 0);
            if (kind == 0) {
                MessageBoxW(g_hwnd,
                    L"This file is not a SQUISH stream, archive, or "
                    L"self-extracting archive (bad or missing header).",
                    APP_NAME, MB_ICONERROR);
                return;
            }
            sfx = false;
            tree = (kind == 'a');
            dst = OutputPath(src, false);
        }
    }

    if (_wcsicmp(dst.c_str(), src.c_str()) == 0) {
        MessageBoxW(g_hwnd,
            L"The output would overwrite the source file. Rename the source, "
            L"or choose a different file.", APP_NAME, MB_ICONERROR);
        return;
    }
    if (FileSize(dst) >= 0) {
        std::wstring q = dst + L"\nalready exists. Overwrite?";
        if (MessageBoxW(g_hwnd, q.c_str(), APP_NAME,
                        MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
    }

    g_job = new Job{ g_hwnd, src, dst, compress, sfx, srcIsDir, tree,
                     false, std::string(), SelectedThreads(), -1 };
    g_t0 = GetTickCount64();
    SetBusy(true);
    SetStatus(compress ? (sfx ? L"Building self-extracting archive…"
                              : L"Compressing…")
                       : L"Extracting…");
    HANDLE th = CreateThread(nullptr, 0, WorkerProc, g_job, 0, nullptr);
    if (!th) {
        SetBusy(false);
        delete g_job; g_job = nullptr;
        SetStatus(L"Failed to start worker thread.");
        return;
    }
    CloseHandle(th);
}

/* Open an archive in the browser pane of the main window.
 *
 * A seekable SQAR02 archive opens instantly (only its header + index are read),
 * straight on the UI thread. A plain §1 stream — a single file, or an archive
 * from an older winsquish (a solid SQAR01 tree) — has no random access, so it
 * must be decompressed whole first: that runs on the worker thread with a
 * progress bar, and OnJobDone opens the browser from the resulting blob. */
static void BrowsePath(const std::wstring &src) {
    if (g_busy) return;
    if (src.empty()) {
        MessageBoxW(g_hwnd, L"Choose a .sq or self-extracting archive first.",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (IsDirectory(src) || FileSize(src) < 0) {
        MessageBoxW(g_hwnd,
            L"Choose an existing .sq stream or self-extracting archive to view.",
            APP_NAME, MB_ICONINFORMATION);
        return;
    }

    SfxInfo si;
    bool sfx = ProbeSfx(src, &si);
    char kind = ProbePayloadKind(src, sfx ? si.payloadOff : 0);
    if (kind == 0) {
        MessageBoxW(g_hwnd,
            L"This file is not a SQUISH stream, archive, or self-extracting "
            L"archive, so its contents cannot be listed.",
            APP_NAME, MB_ICONERROR);
        return;
    }
    if (kind == 'a') {                        /* seekable — open immediately */
        OpenArchiveBrowser(src, SelectedThreads());
        return;
    }
    /* A stream: decompress it whole on the worker, then browse the blob. */
    g_job = new Job{ g_hwnd, src, std::wstring(), false, sfx, false, false,
                     true, std::string(), SelectedThreads(), -1 };
    g_t0 = GetTickCount64();
    SetBusy(true);
    SetStatus(L"Reading archive…");
    HANDLE th = CreateThread(nullptr, 0, WorkerProc, g_job, 0, nullptr);
    if (!th) {
        SetBusy(false);
        delete g_job; g_job = nullptr;
        SetStatus(L"Failed to start worker thread.");
        return;
    }
    CloseHandle(th);
}

static void OnJobDone(int rc) {
    Job *job = g_job; g_job = nullptr;
    double secs = (GetTickCount64() - g_t0) / 1000.0;
    std::wstring browseNext;   /* archive to open in the browser once done */
    SetBusy(false);
    if (!job) return;

    /* Listing job: hand the decompressed blob to the browser (legacy archive
     * tree or single file — OpenBlobBrowser decides). */
    if (job->listing) {
        if (rc == SQUISH_OK) {
            SendMessageW(g_progress, PBM_SETPOS, 100, 0);
            SetStatus(L"Archive opened for browsing.");
            OpenBlobBrowser(job->src, std::move(job->blob), job->threads);
        } else {
            wchar_t err[128];
            MultiByteToWideChar(CP_UTF8, 0, squish_strerror(rc), -1, err, 128);
            SetStatus(std::wstring(L"Failed to read archive: ") + err);
            MessageBoxW(g_hwnd,
                (std::wstring(L"Could not read this archive:\n") + err).c_str(),
                APP_NAME, MB_ICONERROR);
        }
        delete job;
        return;
    }

    if (rc != SQUISH_OK) {
        wchar_t err[128];
        MultiByteToWideChar(CP_UTF8, 0, squish_strerror(rc), -1, err, 128);
        SetStatus(std::wstring(L"Failed: ") + err);
        DeleteFileW(job->dst.c_str());   /* don't leave partial output */
        MessageBoxW(g_hwnd, (std::wstring(L"Operation failed:\n") + err).c_str(),
                    APP_NAME, MB_ICONERROR);
    } else {
        long long in = FileSize(job->src), out = FileSize(job->dst);
        bool outIsDir = IsDirectory(job->dst);
        wchar_t msg[512];
        if (job->compress && job->sfx) {
            if (job->srcIsDir)
                swprintf(msg, 512,
                         L"Done in %.1f s — folder → self-extracting archive %s",
                         secs, PrettySize(out).c_str());
            else
                swprintf(msg, 512,
                         L"Done in %.1f s — self-extracting archive %s → %s",
                         secs, PrettySize(in).c_str(), PrettySize(out).c_str());
        } else if (job->compress) {
            if (job->srcIsDir)
                swprintf(msg, 512,
                         L"Done in %.1f s — folder packed + compressed to %s (%s)",
                         secs, Basename(job->dst).c_str(), PrettySize(out).c_str());
            else {
                double ratio = (in > 0 && out > 0) ? 100.0 * out / in : 0;
                swprintf(msg, 512,
                         L"Done in %.1f s — %s → %s (%.1f%% of original)",
                         secs, PrettySize(in).c_str(), PrettySize(out).c_str(),
                         ratio);
            }
        } else if (outIsDir) {
            swprintf(msg, 512,
                     L"Done in %.1f s — extracted folder \"%s\" (checksum OK)",
                     secs, Basename(job->dst).c_str());
        } else {
            swprintf(msg, 512, L"Done in %.1f s — extracted %s (checksum OK)",
                     secs, PrettySize(out).c_str());
        }
        SetStatus(msg);
        /* If we just packed a folder into a browsable archive, open it so the
         * user lands inside the new archive's contents (WinZip-style). */
        if (job->compress && !job->sfx &&
            ProbePayloadKind(job->dst, 0) == 'a')
            browseNext = job->dst;
    }
    delete job;
    if (!browseNext.empty()) BrowsePath(browseNext);
}

/* "Open": pick an existing .sq / self-extracting archive and browse it. */
static void OpenArchiveDialog(void) {
    if (g_busy) return;
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof ofn };
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L"SQUISH archives (*.sq;*.exe)\0*.sq;*.exe\0"
                      L"All files\0*.*\0";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        BrowsePath(buf);
}

/* "Compress ▸ File…": pick a file and compress it straight away. */
static void CompressFileDialog(void) {
    if (g_busy) return;
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof ofn };
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L"All files\0*.*\0";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        g_source = buf;
        StartJob(true, g_source);
    }
}

/* "Compress ▸ Folder…": pick a folder (IFileOpenDialog, pick-folders) and pack
 * it into a single SQAR archive. */
static void CompressFolderDialog(void) {
    if (g_busy) return;
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                g_source = path;
                CoTaskMemFree(path);
                StartJob(true, g_source);
            }
            item->Release();
        }
    }
    dlg->Release();
}

static HRESULT CALLBACK AboutCallback(HWND, UINT notification, WPARAM,
                                      LPARAM lp, LONG_PTR) {
    if (notification == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", (const wchar_t *)lp,
                      nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

static void ShowAbout(void) {
    wchar_t ver[64];
    MultiByteToWideChar(CP_UTF8, 0, squish_version(), -1, ver, 64);
    std::wstring msg =
        L"libsquish version " + std::wstring(ver) + L"\n\n"
        L"SQUISH predicts every bit with ten statistical models, fuses "
        L"them with a logistic mixer, and arithmetic-codes the result. "
        L"Excellent ratios; symmetric speed of roughly 0.5–0.7 MB/s "
        L"per core, so large files take a while.\n\n"
        L"WinSquish: <a href=\"https://github.com/paigejulianne/winsquish\">"
        L"github.com/paigejulianne/winsquish</a>\n"
        L"SQUISH library: <a href=\"https://github.com/paigejulianne/squish\">"
        L"github.com/paigejulianne/squish</a>\n\n"
        L"Licensed under the GNU GPL v3.";
    TASKDIALOGCONFIG tdc = { sizeof tdc };
    tdc.hwndParent         = g_hwnd;
    tdc.hInstance          = GetModuleHandleW(nullptr);
    tdc.dwFlags            = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tdc.dwCommonButtons    = TDCBF_OK_BUTTON;
    tdc.pszWindowTitle     = L"About WinSquish";
    tdc.pszMainIcon        = MAKEINTRESOURCEW(IDI_APP);
    tdc.pszMainInstruction = L"WinSquish — GUI for the SQUISH context-mixing "
                             L"compressor";
    tdc.pszContent         = msg.c_str();
    tdc.pfCallback         = AboutCallback;
    TaskDialogIndirect(&tdc, nullptr, nullptr, nullptr);
}

/* Scale a 96-dpi design coordinate to the window's DPI. */
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ============================================================================
 * Archive browser — the central file list of the main window
 *
 * The main window doubles as a WinZip/PeaZip-style archive browser: its central
 * ListView shows the contents of whatever archive is open. A seekable SQAR
 * archive opens instantly — only its header and index are read — and each file
 * is inflated on demand, by seeking straight to its own stream, when the user
 * extracts it (libsquish's squish_archive_* API). A plain single-file .sq (or
 * SFX payload) shows as one member and is decompressed whole on extraction.
 * Navigate folders by double-clicking (Up / Backspace to ascend), sort by
 * column, and extract the current selection or everything.
 * ==========================================================================*/

/* One row of the current folder view (a child of the browser's cwd). */
struct ViewRow {
    std::wstring name;      /* leaf name shown in the Name column            */
    std::wstring fullPath;  /* archive-relative '/'-path, no trailing slash  */
    bool         isDir;
    bool         isUp;      /* the synthetic ".." row                        */
    uint64_t     size;
    int          entryIndex;/* index into Browser::entries (files); else -1  */
};

/* The browser holds an archive one of two ways:
 *   arc != NULL  — a seekable SQAR02 handle; members inflate on demand via
 *                  squish_archive_extract (entries carry the member index).
 *   arc == NULL  — `blob` holds the decompressed content (a legacy SQAR01 tree
 *                  or a single file); members are byte ranges of `blob`
 *                  (entries carry dataOff/size). */
struct Browser {
    std::wstring          archivePath;  /* source .sq / .exe                  */
    squish_archive       *arc;          /* seekable handle, or NULL           */
    std::string           payload;      /* SFX: compressed bytes backing arc  */
    std::string           blob;         /* decompressed bytes (arc == NULL)   */
    std::vector<ArcEntry> entries;      /* every member                       */
    std::wstring          cwd;          /* "" (root) or "a/b/" with slash     */
    int                   threads;
    int                   sortCol;      /* 0 = name, 1 = size                 */
    bool                  sortAsc;
    std::vector<ViewRow>  view;         /* rows currently shown               */
    HWND                  hwnd;
    HWND                  hlist;
};

static bool StartsWith(const std::wstring &s, const std::wstring &pfx) {
    return s.size() >= pfx.size() &&
           wcsncmp(s.c_str(), pfx.c_str(), pfx.size()) == 0;
}

/* The system image list (small icons), so files show their real shell icon. */
static HIMAGELIST SysImageListSmall(void) {
    SHFILEINFOW sfi = { 0 };
    return (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof sfi,
                                      SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
}

/* System-image-list index for a name of the given kind, resolved purely from
 * the (fake) attributes + extension — the item need not exist on disk. */
static int IconIndex(const std::wstring &name, bool isDir) {
    SHFILEINFOW sfi = { 0 };
    DWORD attr = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    SHGetFileInfoW(name.c_str(), attr, &sfi, sizeof sfi,
                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    return sfi.iIcon;
}

/* Text for the Type column ("File folder", "TXT file", or plain "File"). */
static std::wstring TypeText(const std::wstring &name, bool isDir) {
    if (isDir) return L"File folder";
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return L"File";
    std::wstring ext = name.substr(dot + 1);
    CharUpperBuffW(&ext[0], (DWORD)ext.size());
    return ext + L" file";
}

/* Rebuild Browser::view: the immediate children (folders then files) of cwd,
 * sorted per the active column. Directories are synthesized from deeper paths
 * as well as explicit directory entries, so intermediate folders always show. */
static void BuildView(Browser *b) {
    b->view.clear();
    const std::wstring &cwd = b->cwd;
    if (!cwd.empty()) {
        ViewRow up = { L"..", std::wstring(), true, true, 0, -1 };
        b->view.push_back(up);
    }
    std::set<std::wstring> dirSet;
    std::vector<ViewRow> dirs, files;
    for (size_t i = 0; i < b->entries.size(); i++) {
        const ArcEntry &e = b->entries[i];
        const std::wstring &p = e.path;
        if (p.size() <= cwd.size() || !StartsWith(p, cwd)) continue;
        std::wstring rem = p.substr(cwd.size());
        size_t slash = rem.find(L'/');
        if (slash != std::wstring::npos) {
            dirSet.insert(rem.substr(0, slash));
        } else if (e.isDir) {
            dirSet.insert(rem);
        } else {
            ViewRow r = { rem, p, false, false, e.size, (int)i };
            files.push_back(std::move(r));
        }
    }
    for (const std::wstring &d : dirSet) {
        ViewRow r = { d, cwd + d, true, false, 0, -1 };
        dirs.push_back(std::move(r));
    }
    auto cmp = [b](const ViewRow &x, const ViewRow &y) -> bool {
        if (b->sortCol == 1 && !x.isDir && !y.isDir && x.size != y.size)
            return b->sortAsc ? x.size < y.size : x.size > y.size;
        int c = _wcsicmp(x.name.c_str(), y.name.c_str());
        return b->sortAsc ? c < 0 : c > 0;
    };
    std::sort(dirs.begin(), dirs.end(), cmp);
    std::sort(files.begin(), files.end(), cmp);
    for (ViewRow &r : dirs)  b->view.push_back(std::move(r));
    for (ViewRow &r : files) b->view.push_back(std::move(r));
}

/* Push Browser::view into the ListView. */
static void FillList(Browser *b) {
    HWND lv = b->hlist;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < b->view.size(); i++) {
        const ViewRow &r = b->view[i];
        LVITEMW it = { 0 };
        it.mask     = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        it.iItem    = (int)i;
        it.pszText  = const_cast<wchar_t *>(r.name.c_str());
        it.lParam   = (LPARAM)i;
        it.iImage   = r.isUp ? IconIndex(L"folder", true)
                             : IconIndex(r.name, r.isDir);
        int idx = ListView_InsertItem(lv, &it);
        if (idx < 0) continue;
        if (!r.isDir) {
            std::wstring sz = PrettySize((long long)r.size);
            ListView_SetItemText(lv, idx, 1, const_cast<wchar_t *>(sz.c_str()));
        }
        if (!r.isUp) {
            std::wstring tp = TypeText(r.name, r.isDir);
            ListView_SetItemText(lv, idx, 2, const_cast<wchar_t *>(tp.c_str()));
        }
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

/* Parent of a "a/b/" cwd ("a/"); "" if already at the root. */
static std::wstring ParentCwd(const std::wstring &cwd) {
    if (cwd.empty()) return cwd;
    std::wstring t = cwd.substr(0, cwd.size() - 1);   /* drop trailing '/' */
    size_t s = t.find_last_of(L'/');
    return s == std::wstring::npos ? std::wstring() : t.substr(0, s + 1);
}

static void ExtractSelection(Browser *b, bool all);   /* fwd */

/* Sync the toolbar button states and the status-bar item counts to the archive
 * that is currently open (g_browser), or to the empty state when none is. */
static void UpdateChrome(void) {
    bool open = g_browser != nullptr;
    SendMessageW(g_toolbar, TB_ENABLEBUTTON, IDM_EXTRACT,     MAKELONG(open, 0));
    SendMessageW(g_toolbar, TB_ENABLEBUTTON, IDM_EXTRACT_ALL, MAKELONG(open, 0));
    bool canUp = open && !g_browser->cwd.empty();
    SendMessageW(g_toolbar, TB_ENABLEBUTTON, IDM_UP, MAKELONG(canUp, 0));
    ShowWindow(g_hint, open ? SW_HIDE : SW_SHOW);
    ShowWindow(g_list, open ? SW_SHOW : SW_HIDE);
    if (!open) { SetCounts(L""); return; }
    int total = ListView_GetItemCount(g_list);
    if (!g_browser->cwd.empty() && total > 0) total--;   /* discount ".." row */
    int sel = ListView_GetSelectedCount(g_list);
    wchar_t s[64];
    if (sel > 0) swprintf(s, 64, L"%d of %d selected", sel, total);
    else         swprintf(s, 64, L"%d item%s", total, total == 1 ? L"" : L"s");
    SetCounts(s);
}

/* Move to a folder and refresh the list, address bar and toolbar. */
static void NavigateTo(Browser *b, const std::wstring &cwd) {
    b->cwd = cwd;
    BuildView(b);
    FillList(b);
    std::wstring disp = Basename(b->archivePath) + L"\\" + b->cwd;
    for (wchar_t &c : disp) if (c == L'/') c = L'\\';
    if (disp.size() > 1 && disp.back() == L'\\') disp.pop_back();
    SetWindowTextW(GetDlgItem(b->hwnd, IDC_PATH_STATIC), disp.c_str());
    UpdateChrome();
}

/* Double-click / Enter on a row: descend into folders, ascend on "..", and for
 * a file, offer to extract it (Save As). */
static void ActivateRow(Browser *b, int item) {
    LVITEMW it = { 0 };
    it.mask = LVIF_PARAM; it.iItem = item;
    if (!ListView_GetItem(b->hlist, &it)) return;
    size_t vi = (size_t)it.lParam;
    if (vi >= b->view.size()) return;
    const ViewRow &r = b->view[vi];
    if (r.isUp)       NavigateTo(b, ParentCwd(b->cwd));
    else if (r.isDir) NavigateTo(b, r.fullPath + L"/");
    else              ExtractSelection(b, false);
}

/* Pick a destination folder (IFileOpenDialog, pick-folders mode). */
static bool PickFolder(HWND owner, const std::wstring &initial,
                       std::wstring *out) {
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                    FOS_PATHMUSTEXIST);
    if (!initial.empty()) {
        IShellItem *si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                  IID_PPV_ARGS(&si)))) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                *out = p; CoTaskMemFree(p); ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

/* Ask for a destination path + filename (IFileSaveDialog, "Save As"), used when
 * extracting a single file so the user can rename it. The dialog itself prompts
 * before overwriting an existing file (FOS_OVERWRITEPROMPT). */
static bool PickSaveFile(HWND owner, const std::wstring &suggestedName,
                         const std::wstring &initial, std::wstring *out) {
    IFileSaveDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM |
                    FOS_NOREADONLYRETURN);
    if (!suggestedName.empty()) dlg->SetFileName(suggestedName.c_str());
    if (!initial.empty()) {
        IShellItem *si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                  IID_PPV_ARGS(&si)))) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                *out = p; CoTaskMemFree(p); ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

/* The on-disk path an entry extracts to under destRoot (its archive path with
 * '/'-separators turned into '\\'). */
static std::wstring EntryDest(const ArcEntry &e, const std::wstring &destRoot) {
    std::wstring rel = e.path;
    for (wchar_t &c : rel) if (c == L'/') c = L'\\';
    return destRoot + L"\\" + rel;
}

static bool PathExists(const std::wstring &p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/* Write file member `e` to `full`, creating parent directories. A seekable
 * archive inflates only that member's stream (squish_archive_extract); a
 * blob-backed archive copies the member's bytes straight out of `blob`.
 * Returns true on success. */
static bool ExtractMember(Browser *b, const ArcEntry &e,
                          const std::wstring &full) {
    size_t s = full.find_last_of(L'\\');
    if (s != std::wstring::npos && !MakeDirTree(full.substr(0, s))) return false;
    if (b->arc) {
        void *raw = nullptr; size_t rn = 0;
        if (squish_archive_extract(b->arc, e.index, &raw, &rn) != SQUISH_OK)
            return false;
        bool ok = WriteWholeFile(full, raw, rn) == 0;
        squish_free(raw);
        return ok;
    }
    return WriteWholeFile(full, b->blob.data() + e.dataOff,
                          (size_t)e.size) == 0;
}

/* The user's answer to a "target already exists" prompt. */
enum OwResult { OW_YES, OW_NO, OW_CANCEL };

/* Ask whether to overwrite an existing target. `*applyAll` is set from the
 * dialog's "do this for all" checkbox so the caller can stop asking. */
static OwResult AskOverwrite(HWND owner, const std::wstring &path,
                             bool *applyAll) {
    std::wstring content = path + L"\n\nOverwrite the existing file?";
    TASKDIALOGCONFIG tdc = { sizeof tdc };
    tdc.hwndParent         = owner;
    tdc.hInstance          = GetModuleHandleW(nullptr);
    tdc.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION |
                             TDF_POSITION_RELATIVE_TO_WINDOW;
    tdc.dwCommonButtons    = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON |
                             TDCBF_CANCEL_BUTTON;
    tdc.pszWindowTitle     = APP_NAME;
    tdc.pszMainIcon        = TD_WARNING_ICON;
    tdc.pszMainInstruction = L"A file already exists at the destination.";
    tdc.pszContent         = content.c_str();
    tdc.pszVerificationText = L"Do this for all remaining conflicts";
    int  button = 0;
    BOOL verify = FALSE;
    if (FAILED(TaskDialogIndirect(&tdc, &button, nullptr, &verify)))
        return OW_CANCEL;
    if (applyAll) *applyAll = (verify == TRUE);
    if (button == IDYES) return OW_YES;
    if (button == IDNO)  return OW_NO;
    return OW_CANCEL;
}

/* Extract either the current selection (all == false) or the whole archive.
 * A single selected file goes through a "Save As" dialog (choose folder AND
 * filename); anything else (folders or multiple files) goes to a destination
 * folder, each member landing at dest\<archive path> (WinRAR's default), with
 * a prompt before overwriting existing files. */
static void ExtractSelection(Browser *b, bool all) {
    std::vector<char> pick(b->entries.size(), 0);
    std::vector<std::wstring> emptyDirs;   /* selected dirs with no entries  */
    bool any = false;
    if (all) {
        for (size_t i = 0; i < b->entries.size(); i++) { pick[i] = 1; any = true; }
    } else {
        int i = -1;
        while ((i = ListView_GetNextItem(b->hlist, i, LVNI_SELECTED)) != -1) {
            LVITEMW it = { 0 };
            it.mask = LVIF_PARAM; it.iItem = i;
            if (!ListView_GetItem(b->hlist, &it)) continue;
            size_t vi = (size_t)it.lParam;
            if (vi >= b->view.size()) continue;
            const ViewRow &r = b->view[vi];
            if (r.isUp) continue;
            if (r.isDir) {
                std::wstring pfx = r.fullPath + L"/";
                bool hit = false;
                for (size_t k = 0; k < b->entries.size(); k++) {
                    const std::wstring &p = b->entries[k].path;
                    if (p == r.fullPath || StartsWith(p, pfx)) { pick[k] = 1; hit = true; }
                }
                if (!hit) emptyDirs.push_back(r.fullPath);
                any = true;
            } else if (r.entryIndex >= 0) {
                pick[(size_t)r.entryIndex] = 1; any = true;
            }
        }
    }
    if (!any) {
        MessageBoxW(b->hwnd, L"Select one or more files or folders to extract.",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }

    /* If the selection is exactly one file (no folders), offer a "Save As"
     * dialog so the destination folder AND filename can be chosen/renamed. */
    size_t nSelFiles = 0, sole = 0;
    for (size_t k = 0; k < b->entries.size(); k++)
        if (pick[k] && !b->entries[k].isDir) { nSelFiles++; sole = k; }
    bool oneFile = nSelFiles == 1 && emptyDirs.empty();
    for (size_t k = 0; oneFile && k < b->entries.size(); k++)
        if (pick[k] && b->entries[k].isDir) oneFile = false;   /* a dir came too */

    if (oneFile) {
        const ArcEntry &e = b->entries[sole];
        std::wstring leaf = e.path;
        size_t sl = leaf.find_last_of(L'/');
        if (sl != std::wstring::npos) leaf = leaf.substr(sl + 1);
        std::wstring dest;
        if (!PickSaveFile(b->hwnd, leaf, DirWithSlash(b->archivePath), &dest))
            return;
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        bool ok = ExtractMember(b, e, dest);
        SetCursor(old);
        wchar_t msg[700];
        if (ok)
            swprintf(msg, 700, L"Extracted \"%s\" (%s) to:\n%s", leaf.c_str(),
                     PrettySize((long long)e.size).c_str(), dest.c_str());
        else
            swprintf(msg, 700, L"Failed to extract \"%s\".", leaf.c_str());
        MessageBoxW(b->hwnd, msg, APP_NAME,
                    ok ? MB_ICONINFORMATION : MB_ICONWARNING);
        return;
    }

    /* Otherwise pick a destination folder; members land at dest\<archive path>. */
    std::wstring dest;
    if (!PickFolder(b->hwnd, DirWithSlash(b->archivePath), &dest)) return;

    uint64_t nfiles = 0, nbytes = 0, nskip = 0;
    bool okAll = true, cancelled = false;
    enum { ASK, ALL_YES, ALL_NO } overwrite = ASK;   /* existing-file policy */

    /* Selected-but-empty directories just get created (merging into any that
     * already exist is harmless, so those never prompt). */
    for (const std::wstring &d : emptyDirs) {
        std::wstring rel = d;
        for (wchar_t &c : rel) if (c == L'/') c = L'\\';
        if (!MakeDirTree(dest + L"\\" + rel)) okAll = false;
    }

    HCURSOR wait = LoadCursorW(nullptr, IDC_WAIT);
    for (size_t k = 0; k < b->entries.size() && !cancelled; k++) {
        if (!pick[k]) continue;
        const ArcEntry &e = b->entries[k];
        std::wstring full = EntryDest(e, dest);

        /* Only files are ever overwritten; existing directories merge. */
        if (!e.isDir && PathExists(full)) {
            if (overwrite == ALL_NO) { nskip++; continue; }
            if (overwrite == ASK) {
                bool applyAll = false;
                OwResult r = AskOverwrite(b->hwnd, full, &applyAll);
                if (r == OW_CANCEL) { cancelled = true; break; }
                if (r == OW_NO)  { if (applyAll) overwrite = ALL_NO; nskip++; continue; }
                if (applyAll)    overwrite = ALL_YES;   /* r == OW_YES */
            }
        }
        HCURSOR old = SetCursor(wait);
        bool ok = e.isDir ? MakeDirTree(full) : ExtractMember(b, e, full);
        SetCursor(old);
        if (ok) { if (!e.isDir) { nfiles++; nbytes += e.size; } }
        else    okAll = false;
    }

    std::wstring tail = L" to:\n" + dest;
    if (nskip) {
        wchar_t s[64];
        swprintf(s, 64, L"\n(%llu existing file%s skipped)",
                 (unsigned long long)nskip, nskip == 1 ? L"" : L"s");
        tail += s;
    }
    wchar_t msg[700];
    const wchar_t *plural = nfiles == 1 ? L"" : L"s";
    if (cancelled)
        swprintf(msg, 700, L"Extraction cancelled — %llu file%s written%s",
                 (unsigned long long)nfiles, plural, tail.c_str());
    else if (okAll)
        swprintf(msg, 700, L"Extracted %llu file%s (%s)%s",
                 (unsigned long long)nfiles, plural,
                 PrettySize((long long)nbytes).c_str(), tail.c_str());
    else
        swprintf(msg, 700, L"Finished with errors — %llu file%s written%s",
                 (unsigned long long)nfiles, plural, tail.c_str());
    MessageBoxW(b->hwnd, msg, APP_NAME,
                (okAll && !cancelled) ? MB_ICONINFORMATION : MB_ICONWARNING);
}

/* Fill b->entries from an open seekable archive handle. */
static int LoadArchiveEntries(Browser *b) {
    uint64_t n = squish_archive_count(b->arc);
    b->entries.reserve((size_t)n);
    for (uint64_t i = 0; i < n; i++) {
        squish_archive_entry e;
        if (squish_archive_stat(b->arc, i, &e) != SQUISH_OK) return SQUISH_E_FORMAT;
        ArcEntry a;
        a.path  = FromUtf8(e.path, (int)strlen(e.path));
        a.isDir = e.is_dir != 0;
        a.size  = e.size;
        a.index = i;
        a.dataOff = 0;
        b->entries.push_back(std::move(a));
    }
    return SQUISH_OK;
}

/* Allocate a Browser with the common defaults set. */
static Browser *NewBrowser(const std::wstring &archivePath, int threads) {
    Browser *b     = new Browser();
    b->archivePath = archivePath;
    b->arc         = nullptr;
    b->threads     = threads;
    b->cwd         = L"";
    b->sortCol     = 0;
    b->sortAsc     = true;
    b->hwnd        = nullptr;
    b->hlist       = nullptr;
    return b;
}

/* Adopt a fully populated Browser as the archive shown in the main window,
 * replacing (and freeing) any archive that was open before. The main window's
 * ListView becomes this archive's view. */
static void LoadBrowserIntoMain(Browser *b) {
    if (g_browser) {
        if (g_browser->arc) squish_archive_close(g_browser->arc);
        delete g_browser;
        g_browser = nullptr;
    }
    b->hwnd  = g_hwnd;
    b->hlist = g_list;
    g_browser = b;
    ListView_DeleteAllItems(g_list);
    NavigateTo(b, L"");
    std::wstring title = Basename(b->archivePath) + L" — " + APP_NAME;
    SetWindowTextW(g_hwnd, title.c_str());
    SetStatus(L"Archive opened.");
    SetFocus(g_list);
}

/* Close the open archive and return the main window to its empty state. */
static void CloseArchive(void) {
    if (!g_browser) return;
    if (g_browser->arc) squish_archive_close(g_browser->arc);
    delete g_browser;
    g_browser = nullptr;
    ListView_DeleteAllItems(g_list);
    SetWindowTextW(GetDlgItem(g_hwnd, IDC_PATH_STATIC), L"");
    SetWindowTextW(g_hwnd, APP_NAME);
    SetStatus(L"Ready.");
    UpdateChrome();
}

/* Open a seekable SQAR02 archive (plain .sq or an SFX payload) in the browser,
 * reading only its header + index — no decompression. */
static void OpenArchiveBrowser(const std::wstring &archivePath, int threads) {
    Browser *b = NewBrowser(archivePath, threads);
    SfxInfo si;
    bool sfx = ProbeSfx(archivePath, &si);
    int rc;
    if (sfx) {
        /* open_memory borrows the payload bytes, so the Browser must keep them. */
        if (!ReadRange(archivePath, si.payloadOff, si.payloadLen, &b->payload)) {
            MessageBoxW(g_hwnd, L"Could not read the archive payload.",
                        APP_NAME, MB_ICONERROR);
            delete b; return;
        }
        rc = squish_archive_open_memory(b->payload.data(), b->payload.size(),
                                        &b->arc);
    } else {
        rc = squish_archive_open(ToUtf8(archivePath).c_str(), &b->arc);
    }
    if (rc == SQUISH_OK) rc = LoadArchiveEntries(b);
    if (rc != SQUISH_OK) {
        if (b->arc) { squish_archive_close(b->arc); b->arc = nullptr; }
        MessageBoxW(g_hwnd, L"The archive could not be opened (corrupt index).",
                    APP_NAME, MB_ICONERROR);
        delete b; return;
    }
    LoadBrowserIntoMain(b);
}

/* Open the browser from an already-decompressed blob: a legacy SQAR01 tree
 * (list its members) or a single file's contents (one member). Takes ownership
 * of `blob`; members reference it, so it lives as long as the window. */
static void OpenBlobBrowser(const std::wstring &archivePath, std::string blob,
                            int threads) {
    Browser *b = NewBrowser(archivePath, threads);
    b->blob = std::move(blob);
    const unsigned char *bytes = (const unsigned char *)b->blob.data();
    size_t n = b->blob.size();
    if (LegacyIs(bytes, n)) {
        if (LegacyList(bytes, n, &b->entries) != SQUISH_OK) {
            MessageBoxW(g_hwnd,
                L"The archive directory is corrupt and could not be listed.",
                APP_NAME, MB_ICONERROR);
            delete b; return;
        }
    } else {
        /* A single file: one member named after the stored (SFX) name or the
         * name a plain .sq would extract to; its bytes are the whole blob. */
        std::wstring nm;
        SfxInfo si;
        if (ProbeSfx(archivePath, &si)) {
            std::wstring stored;
            ReadSfxName(archivePath, si, &stored);
            nm = SafeStoredName(stored);
        } else {
            nm = Basename(OutputPath(archivePath, false));
        }
        if (nm.empty()) nm = L"file.out";
        ArcEntry e = { nm, false, (uint64_t)n, 0, 0 };
        b->entries.push_back(std::move(e));
    }
    LoadBrowserIntoMain(b);
}

/* --- custom toolbar glyphs (drawn with GDI+ so they scale with DPI) ---------
 * Four flat, colourful icons rendered on a 16x16 design grid scaled to `px`:
 * an amber open folder (Open), a purple box squeezed by arrows (Compress), a
 * green arrow lifting out of a tray (Extract), and a blue up-arrow (Up). Each
 * is rasterised to a premultiplied 32-bpp bitmap and added to an image list. */
static void RoundRectPath(Gdiplus::GraphicsPath &p, Gdiplus::REAL x,
                          Gdiplus::REAL y, Gdiplus::REAL w, Gdiplus::REAL h,
                          Gdiplus::REAL r) {
    p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    p.CloseFigure();
}

static void GlyphOpen(Gdiplus::Graphics &g, Gdiplus::REAL u) {
    using namespace Gdiplus;
    SolidBrush dark(Color(255, 0xD8, 0x8C, 0x14));
    SolidBrush lite(Color(255, 0xFF, 0xC9, 0x4D));
    g.FillRectangle(&dark, 1.5f * u, 2.6f * u, 6.0f * u, 2.4f * u);   /* tab   */
    GraphicsPath body; RoundRectPath(body, 1.5f * u, 3.8f * u,
                                     13.0f * u, 9.7f * u, 1.1f * u);
    g.FillPath(&dark, &body);                                        /* back  */
    PointF front[4] = { PointF(1.6f * u, 13.5f * u), PointF(4.3f * u, 6.9f * u),
                        PointF(15.5f * u, 6.9f * u), PointF(12.8f * u, 13.5f * u) };
    g.FillPolygon(&lite, front, 4);                                  /* open lid */
}

static void GlyphCompress(Gdiplus::Graphics &g, Gdiplus::REAL u) {
    using namespace Gdiplus;
    SolidBrush box(Color(255, 0x6E, 0x3A, 0xA6));
    SolidBrush white(Color(255, 0xFF, 0xFF, 0xFF));
    GraphicsPath p; RoundRectPath(p, 2.0f * u, 2.0f * u,
                                  12.0f * u, 12.0f * u, 2.0f * u);
    g.FillPath(&box, &p);
    PointF top[3] = { PointF(5.6f * u, 4.2f * u), PointF(10.4f * u, 4.2f * u),
                      PointF(8.0f * u, 7.0f * u) };                  /* ▼ */
    PointF bot[3] = { PointF(5.6f * u, 11.8f * u), PointF(10.4f * u, 11.8f * u),
                      PointF(8.0f * u, 9.0f * u) };                  /* ▲ */
    g.FillPolygon(&white, top, 3);
    g.FillPolygon(&white, bot, 3);
    g.FillRectangle(&white, 5.0f * u, 7.55f * u, 6.0f * u, 0.9f * u);/* seam */
}

static void GlyphExtract(Gdiplus::Graphics &g, Gdiplus::REAL u) {
    using namespace Gdiplus;
    SolidBrush tray(Color(255, 0x54, 0x64, 0x74));
    g.FillRectangle(&tray, 2.5f * u,  9.0f * u,  2.0f * u, 5.0f * u);/* left  */
    g.FillRectangle(&tray, 11.5f * u, 9.0f * u,  2.0f * u, 5.0f * u);/* right */
    g.FillRectangle(&tray, 2.5f * u,  12.4f * u, 11.0f * u, 1.6f * u);/* base */
    SolidBrush green(Color(255, 0x33, 0xA8, 0x4A));
    g.FillRectangle(&green, 6.9f * u, 5.2f * u, 2.2f * u, 5.8f * u);  /* shaft */
    PointF head[3] = { PointF(5.0f * u, 5.6f * u), PointF(11.0f * u, 5.6f * u),
                       PointF(8.0f * u, 1.8f * u) };                 /* ▲ out */
    g.FillPolygon(&green, head, 3);
}

static void GlyphUp(Gdiplus::Graphics &g, Gdiplus::REAL u) {
    using namespace Gdiplus;
    SolidBrush blue(Color(255, 0x2E, 0x77, 0xC9));
    g.FillRectangle(&blue, 6.8f * u, 6.8f * u, 2.4f * u, 6.7f * u);  /* shaft */
    PointF head[3] = { PointF(3.8f * u, 7.4f * u), PointF(12.2f * u, 7.4f * u),
                       PointF(8.0f * u, 2.5f * u) };                 /* ▲ */
    g.FillPolygon(&blue, head, 3);
}

/* Rasterise `bmp` (straight ARGB) into a premultiplied top-down 32-bpp DIB so
 * comctl32 alpha-blends the icon cleanly over the toolbar. */
static HBITMAP PremultipliedDib(Gdiplus::Bitmap &bmp, int px) {
    using namespace Gdiplus;
    Rect rc(0, 0, px, px);
    BitmapData bd;
    if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
        return nullptr;
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize     = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth    = px;
    bi.bmiHeader.biHeight   = -px;          /* top-down */
    bi.bmiHeader.biPlanes   = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (dib && bits) {
        for (int y = 0; y < px; y++) {
            const BYTE *s = (const BYTE *)bd.Scan0 + (size_t)y * bd.Stride;
            BYTE *d = (BYTE *)bits + (size_t)y * px * 4;
            for (int x = 0; x < px; x++, s += 4, d += 4) {
                unsigned a = s[3];
                d[0] = (BYTE)(s[0] * a / 255);   /* B */
                d[1] = (BYTE)(s[1] * a / 255);   /* G */
                d[2] = (BYTE)(s[2] * a / 255);   /* R */
                d[3] = (BYTE)a;
            }
        }
    }
    bmp.UnlockBits(&bd);
    return dib;
}

/* Build the toolbar's 4-glyph image list at the given icon size. */
static HIMAGELIST BuildToolbarImageList(int px) {
    HIMAGELIST il = ImageList_Create(px, px, ILC_COLOR32, 4, 0);
    if (!il) return nullptr;
    void (*draws[4])(Gdiplus::Graphics &, Gdiplus::REAL) =
        { GlyphOpen, GlyphCompress, GlyphExtract, GlyphUp };
    for (int i = 0; i < 4; i++) {
        Gdiplus::Bitmap bmp(px, px, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        draws[i](g, (Gdiplus::REAL)px / 16.0f);
        g.Flush(Gdiplus::FlushIntentionSync);
        HBITMAP hb = PremultipliedDib(bmp, px);
        if (hb) { ImageList_Add(il, hb, nullptr); DeleteObject(hb); }
    }
    return il;
}

/* Build the top toolbar (WinZip/PeaZip-style icon+text buttons) with the custom
 * glyphs above. */
static void CreateToolbar(HWND hwnd) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    g_toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST |
        TBSTYLE_TOOLTIPS | CCS_NODIVIDER | CCS_NORESIZE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR, hi, nullptr);
    SendMessageW(g_toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(g_toolbar, TB_SETEXTENDEDSTYLE, 0,
                 TBSTYLE_EX_DRAWDDARROWS | TBSTYLE_EX_MIXEDBUTTONS);

    g_tbImages = BuildToolbarImageList(S(20));
    SendMessageW(g_toolbar, TB_SETIMAGELIST, 0, (LPARAM)g_tbImages);

    INT_PTR sOpen = SendMessageW(g_toolbar, TB_ADDSTRINGW, 0, (LPARAM)L"Open\0");
    INT_PTR sComp = SendMessageW(g_toolbar, TB_ADDSTRINGW, 0, (LPARAM)L"Compress\0");
    INT_PTR sExtr = SendMessageW(g_toolbar, TB_ADDSTRINGW, 0, (LPARAM)L"Extract\0");
    INT_PTR sUp   = SendMessageW(g_toolbar, TB_ADDSTRINGW, 0, (LPARAM)L"Up\0");

    TBBUTTON tb[5] = { 0 };
    tb[0].iBitmap = 0;  tb[0].idCommand = IDM_OPEN;
    tb[0].fsState = TBSTATE_ENABLED; tb[0].fsStyle = BTNS_AUTOSIZE | BTNS_SHOWTEXT;
    tb[0].iString = sOpen;
    tb[1].iBitmap = 1;  tb[1].idCommand = IDM_COMPRESS;
    tb[1].fsState = TBSTATE_ENABLED;
    tb[1].fsStyle = BTNS_AUTOSIZE | BTNS_SHOWTEXT | BTNS_WHOLEDROPDOWN;
    tb[1].iString = sComp;
    tb[2].iBitmap = 2;  tb[2].idCommand = IDM_EXTRACT;
    tb[2].fsState = 0;  tb[2].fsStyle = BTNS_AUTOSIZE | BTNS_SHOWTEXT;
    tb[2].iString = sExtr;
    tb[3].fsStyle = BTNS_SEP;
    tb[4].iBitmap = 3;  tb[4].idCommand = IDM_UP;
    tb[4].fsState = 0;  tb[4].fsStyle = BTNS_AUTOSIZE | BTNS_SHOWTEXT;
    tb[4].iString = sUp;
    SendMessageW(g_toolbar, TB_ADDBUTTONS, 5, (LPARAM)tb);
    SendMessageW(g_toolbar, TB_AUTOSIZE, 0, 0);
}

static void CreateControls(HWND hwnd) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    const DWORD ES = WS_CHILD | WS_VISIBLE;

    CreateToolbar(hwnd);

    /* Address bar: "Location:" + the current archive/folder path. */
    CreateWindowExW(0, L"STATIC", L"Location:", ES | SS_CENTERIMAGE,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LOC_LABEL, hi, nullptr);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
                    ES | SS_PATHELLIPSIS | SS_CENTERIMAGE,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_PATH_STATIC, hi, nullptr);
    /* Compression options, right-aligned on the address row. */
    CreateWindowExW(0, L"BUTTON", L"Self-extracting .exe",
                    ES | WS_TABSTOP | BS_AUTOCHECKBOX,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SFX_CHECK, hi, nullptr);
    CreateWindowExW(0, L"STATIC", L"CPU:", ES | SS_CENTERIMAGE,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CPU_LABEL, hi, nullptr);
    CreateWindowExW(0, L"COMBOBOX", L"",
                    ES | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CPU_COMBO, hi, nullptr);

    /* The central file list — the archive browser. */
    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                    ES | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LV, hi, nullptr);
    ListView_SetExtendedListViewStyle(g_list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
    HIMAGELIST il = SysImageListSmall();
    if (il) ListView_SetImageList(g_list, il, LVSIL_SMALL);
    LVCOLUMNW c = { 0 };
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = (LPWSTR)L"Name"; c.cx = S(340); c.iSubItem = 0; ListView_InsertColumn(g_list, 0, &c);
    c.pszText = (LPWSTR)L"Size"; c.cx = S(110); c.iSubItem = 1; ListView_InsertColumn(g_list, 1, &c);
    c.pszText = (LPWSTR)L"Type"; c.cx = S(150); c.iSubItem = 2; ListView_InsertColumn(g_list, 2, &c);

    /* Empty-state hint, drawn over the (hidden) list when no archive is open. */
    g_hint = CreateWindowExW(0, L"STATIC",
        L"No archive open\r\n\r\n"
        L"Open  —  browse an existing .sq or self-extracting archive\r\n"
        L"Compress  —  pack a file or folder into a new .sq\r\n\r\n"
        L"…or drag a file, folder, or archive onto this window.",
        ES | SS_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_HINT, hi, nullptr);

    /* Status bar with an embedded progress bar over its middle part. */
    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"Ready.",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, hi, nullptr);
    g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD,
        0, 0, 0, 0, g_status, (HMENU)(INT_PTR)IDC_PROGRESS, hi, nullptr);
    SendMessageW(g_progress, PBM_SETRANGE32, 0, 100);

    for (HWND h : { GetDlgItem(hwnd, IDC_LOC_LABEL), GetDlgItem(hwnd, IDC_PATH_STATIC),
                    GetDlgItem(hwnd, IDC_CPU_LABEL), GetDlgItem(hwnd, IDC_CPU_COMBO),
                    GetDlgItem(hwnd, IDC_SFX_CHECK), g_list, g_hint })
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);

    /* CPU-cores combo: 1..ncpu, default 4 clamped to the machine (never "all"). */
    int ncpu = squish_threads();
    if (ncpu < 1) ncpu = 1;
    HWND combo = GetDlgItem(hwnd, IDC_CPU_COMBO);
    for (int i = 1; i <= ncpu; i++) {
        wchar_t s[16];
        swprintf(s, 16, L"%d", i);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)s);
    }
    int def = ncpu < DEFAULT_THREADS ? ncpu : DEFAULT_THREADS;
    SendMessageW(combo, CB_SETCURSEL, def - 1, 0);
}

/* Lay out the toolbar, address row, file list, and status bar for the current
 * client size. Called on every WM_SIZE. */
static void LayoutMain(HWND hwnd) {
    if (!g_toolbar || !g_status) return;
    SendMessageW(g_status, WM_SIZE, 0, 0);   /* re-dock the status bar to bottom */

    RECT rc; GetClientRect(hwnd, &rc);
    SIZE ts; SendMessageW(g_toolbar, TB_GETMAXSIZE, 0, (LPARAM)&ts);
    int tbH = ts.cy + S(6);
    MoveWindow(g_toolbar, 0, 0, rc.right, tbH, TRUE);

    RECT sbr; GetWindowRect(g_status, &sbr);
    int sbH = sbr.bottom - sbr.top;

    int pad = S(8), rowH = S(23), rowY = tbH + S(5);

    /* Right cluster on the address row: [ Self-extracting .exe ] [CPU:] [combo] */
    int cx = rc.right - pad;
    int comboW = S(52); cx -= comboW; int comboX = cx;
    cx -= S(4);
    int cpuW = S(30);   cx -= cpuW;   int cpuX = cx;
    cx -= S(12);
    int sfxW = S(150);  cx -= sfxW;   int sfxX = cx;
    int pathRight = cx - S(12);

    MoveWindow(GetDlgItem(hwnd, IDC_SFX_CHECK), sfxX,   rowY, sfxW,   rowH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_CPU_LABEL), cpuX,   rowY, cpuW,   rowH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_CPU_COMBO), comboX, rowY, comboW, S(200), TRUE);

    int locW = S(56);
    MoveWindow(GetDlgItem(hwnd, IDC_LOC_LABEL), pad, rowY, locW, rowH, TRUE);
    int pathX = pad + locW + S(4);
    int pathW = pathRight - pathX; if (pathW < S(60)) pathW = S(60);
    MoveWindow(GetDlgItem(hwnd, IDC_PATH_STATIC), pathX, rowY, pathW, rowH, TRUE);

    int listY = rowY + rowH + S(6);
    int listH = rc.bottom - sbH - listY; if (listH < 0) listH = 0;
    MoveWindow(g_list, pad, listY, rc.right - 2 * pad, listH, TRUE);
    MoveWindow(g_hint, pad, listY + S(40), rc.right - 2 * pad,
               listH > S(80) ? listH - S(80) : listH, TRUE);

    /* Status-bar parts: [ status text | progress | counts ]. */
    int parts[3];
    parts[0] = rc.right - S(280); if (parts[0] < S(80)) parts[0] = S(80);
    parts[1] = rc.right - S(120); if (parts[1] < parts[0] + S(40)) parts[1] = parts[0] + S(40);
    parts[2] = -1;
    SendMessageW(g_status, SB_SETPARTS, 3, (LPARAM)parts);
    RECT prc; SendMessageW(g_status, SB_GETRECT, 1, (LPARAM)&prc);
    MoveWindow(g_progress, prc.left + S(2), prc.top + S(2),
               (prc.right - prc.left) - S(4), (prc.bottom - prc.top) - S(4), TRUE);
}

static HMENU BuildMenu(void) {
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open archive…\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_COMPRESS_FILE,   L"Compress &file…");
    AppendMenuW(file, MF_STRING, IDM_COMPRESS_FOLDER, L"Compress f&older…");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_CLOSE_ARCHIVE, L"&Close archive");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    HMENU archive = CreatePopupMenu();
    AppendMenuW(archive, MF_STRING, IDM_EXTRACT,     L"&Extract selected…");
    AppendMenuW(archive, MF_STRING, IDM_EXTRACT_ALL, L"Extract &all…");
    AppendMenuW(archive, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(archive, MF_STRING, IDM_UP,          L"&Up one level\tBackspace");
    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, IDM_REGISTER,
                L"&Install Explorer context menu");
    AppendMenuW(tools, MF_STRING, IDM_UNREGISTER,
                L"&Remove Explorer context menu");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About WinSquish");
    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file,    L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)archive, L"&Archive");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools,   L"&Tools");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help,    L"&Help");
    return bar;
}

/* Route a dropped path: an archive/stream opens in the browser; anything else
 * (a plain file or a folder) is compressed. */
static void HandleDrop(const std::wstring &p) {
    if (g_busy) return;
    if (IsDirectory(p)) { g_source = p; StartJob(true, g_source); return; }
    if (FileSize(p) < 0) return;
    SfxInfo si;
    bool sfx = ProbeSfx(p, &si);
    char kind = ProbePayloadKind(p, sfx ? si.payloadOff : 0);
    if (kind) BrowsePath(p);
    else { g_source = p; StartJob(true, g_source); }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateControls(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        UpdateChrome();               /* start in the empty state */
        return 0;

    case WM_SIZE:
        LayoutMain(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(600);
        mm->ptMinTrackSize.y = S(380);
        return 0;
    }

    case WM_DROPFILES: {
        wchar_t path[MAX_PATH];
        if (!g_busy && DragQueryFileW((HDROP)wp, 0, path, MAX_PATH))
            HandleDrop(path);
        DragFinish((HDROP)wp);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:            OpenArchiveDialog();               return 0;
        case IDM_COMPRESS:        /* toolbar main click without dropdown */
        case IDM_COMPRESS_FILE:   CompressFileDialog();              return 0;
        case IDM_COMPRESS_FOLDER: CompressFolderDialog();            return 0;
        case IDM_EXTRACT:
            if (g_browser) ExtractSelection(g_browser, false);       return 0;
        case IDM_EXTRACT_ALL:
            if (g_browser) ExtractSelection(g_browser, true);        return 0;
        case IDM_UP:
            if (g_browser) NavigateTo(g_browser, ParentCwd(g_browser->cwd));
            return 0;
        case IDM_CLOSE_ARCHIVE:   CloseArchive();                    return 0;
        case IDC_SFX_CHECK:
            if (HIWORD(wp) == BN_CLICKED) RefreshSourceStatus();
            return 0;
        case IDM_REGISTER:
            /* The GUI registers per-user (HKCU): no elevation needed. */
            MessageBoxW(hwnd, RegisterShell(HKEY_CURRENT_USER)
                ? L"Context-menu entries installed for the current user.\n\n"
                  L"Right-click any file for \"Compress to .sq\" or\n"
                  L"\"Compress to self-extracting .exe\", and any .sq file\n"
                  L"for \"Extract with WinSquish\"."
                : L"Registration failed (registry access denied?).",
                APP_NAME, MB_ICONINFORMATION);
            return 0;
        case IDM_UNREGISTER:
            UnregisterShell(HKEY_CURRENT_USER);
            MessageBoxW(hwnd, L"Context-menu entries removed.",
                        APP_NAME, MB_ICONINFORMATION);
            return 0;
        case IDM_ABOUT:        ShowAbout();                        return 0;
        case IDM_EXIT:         DestroyWindow(hwnd);                return 0;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        /* Toolbar "Compress" dropdown: choose file vs. folder. */
        if (nh->idFrom == IDC_TOOLBAR && nh->code == TBN_DROPDOWN) {
            NMTOOLBARW *tb = (NMTOOLBARW *)lp;
            if (tb->iItem == IDM_COMPRESS && !g_busy) {
                RECT rb; SendMessageW(g_toolbar, TB_GETRECT, IDM_COMPRESS, (LPARAM)&rb);
                MapWindowPoints(g_toolbar, HWND_DESKTOP, (POINT *)&rb, 2);
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, IDM_COMPRESS_FILE,   L"Compress File…");
                AppendMenuW(m, MF_STRING, IDM_COMPRESS_FOLDER, L"Compress Folder…");
                TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN,
                               rb.left, rb.bottom, 0, hwnd, nullptr);
                DestroyMenu(m);
            }
            return TBDDRET_DEFAULT;
        }
        /* File-list interactions, forwarded to the open archive. */
        if (nh->idFrom == IDC_LV && g_browser) {
            Browser *b = g_browser;
            switch (nh->code) {
            case LVN_ITEMACTIVATE: {
                NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lp;
                if (ia->iItem >= 0) ActivateRow(b, ia->iItem);
                return 0;
            }
            case LVN_KEYDOWN: {
                NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lp;
                if (kd->wVKey == VK_BACK) {
                    NavigateTo(b, ParentCwd(b->cwd)); return 0;
                }
                break;
            }
            case LVN_COLUMNCLICK: {
                NMLISTVIEW *nl = (NMLISTVIEW *)lp;
                if (nl->iSubItem == b->sortCol) b->sortAsc = !b->sortAsc;
                else { b->sortCol = nl->iSubItem; b->sortAsc = true; }
                BuildView(b); FillList(b); UpdateChrome();
                return 0;
            }
            case LVN_ITEMCHANGED:
                UpdateChrome();
                return 0;
            }
        }
        break;
    }

    case WM_APP_PROGRESS:
        SendMessageW(g_progress, PBM_SETPOS, wp, 0);
        {
            const wchar_t *verb = L"Compressing…";
            if (g_job) {
                if (g_job->listing)      verb = L"Reading…";
                else if (!g_job->compress) verb = L"Extracting…";
                else if (g_job->sfx)     verb = L"Building…";
            }
            wchar_t s[64];
            swprintf(s, 64, L"%s %d%%", verb, (int)wp);
            SetStatus(s);
        }
        return 0;

    case WM_APP_DONE:
        OnJobDone((int)wp);
        return 0;

    case WM_CLOSE:
        if (g_busy &&
            MessageBoxW(hwnd, L"An operation is still running. Quit anyway?\n"
                              L"(The partial output file will be incomplete.)",
                        APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_browser) {
            if (g_browser->arc) squish_archive_close(g_browser->arc);
            delete g_browser;
            g_browser = nullptr;
        }
        if (g_tbImages) { ImageList_Destroy(g_tbImages); g_tbImages = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --- entry point -------------------------------------------------------------*/
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    /* parse command line */
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring file;
    int autoMode = 0;   /* 0=none 1=compress 2=decompress 3=compress-sfx */
    bool quiet = false; /* suppress register/unregister confirmation dialogs */
    bool allUsers = false;  /* register/unregister in HKLM (system-wide) */
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--quiet")         quiet = true;
        else if (a == L"--allusers") allUsers = true;
    }
    HKEY regRoot = allUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--register") {
            bool ok = RegisterShell(regRoot);
            if (!quiet)
                MessageBoxW(nullptr, ok
                    ? L"WinSquish context-menu entries installed."
                    : L"Registration failed.", APP_NAME,
                    ok ? MB_ICONINFORMATION : MB_ICONERROR);
            return ok ? 0 : 1;
        }
        if (a == L"--unregister") {
            UnregisterShell(regRoot);
            if (!quiet)
                MessageBoxW(nullptr, L"WinSquish context-menu entries removed.",
                            APP_NAME, MB_ICONINFORMATION);
            return 0;
        }
        if (a == L"--compress")            autoMode = 1;
        else if (a == L"--decompress")     autoMode = 2;
        else if (a == L"--compress-sfx")   autoMode = 3;
        else if (a == L"--view")           autoMode = 4;
        else if (a == L"--quiet")          ; /* handled in the pre-scan above */
        else if (a == L"--allusers")       ; /* handled in the pre-scan above */
        else if (file.empty())             file = a;
    }
    LocalFree(argv);

    /* If we were double-clicked as a self-extracting archive (our own image
     * carries an SFX trailer) with no other request, extract ourselves. */
    if (file.empty() && autoMode == 0) {
        SfxInfo selfInfo;
        if (ProbeSfx(ExePath(), &selfInfo)) { file = ExePath(); autoMode = 2; }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc = { sizeof icc,
        ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES |
        ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdipIn;
    Gdiplus::GdiplusStartup(&g_gdipToken, &gdipIn, nullptr);

    g_dpi = GetDpiForSystem();
    NONCLIENTMETRICSW ncm = { sizeof ncm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    RECT r = { 0, 0, S(940), S(600) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, WND_CLASS, APP_NAME,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, BuildMenu(), hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    if (!file.empty()) {
        if (autoMode == 3) CheckDlgButton(hwnd, IDC_SFX_CHECK, BST_CHECKED);
        if (autoMode == 1 || autoMode == 3) {
            g_source = file;
            StartJob(true, file);               /* compress (.sq or SFX)      */
        } else if (autoMode == 2) {
            StartJob(false, file);              /* extract straight to disk   */
        } else if (autoMode == 4) {
            BrowsePath(file);                   /* browse the archive         */
        } else {
            /* A bare path: browse it if it is an archive, else queue it for
             * compression so the toolbar's Compress acts on it. */
            SfxInfo si;
            bool sfx = ProbeSfx(file, &si);
            char kind = IsDirectory(file) ? 0
                      : ProbePayloadKind(file, sfx ? si.payloadOff : 0);
            if (kind) BrowsePath(file);
            else { g_source = file; RefreshSourceStatus(); }
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(g_font);
    Gdiplus::GdiplusShutdown(g_gdipToken);
    return (int)msg.wParam;
}
