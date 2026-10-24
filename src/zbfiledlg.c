/*
 * zbfiledlg.dll - replaces Wine's IFileOpenDialog / IFileSaveDialog with the
 * native Linux (xdg-desktop-portal) file chooser.
 *
 * Registered as InprocServer32 for CLSID_FileOpenDialog / CLSID_FileSaveDialog
 * in a single prefix. Every call is forwarded to Wine's builtin comdlg32
 * object, except Show() and the result getters. If the Linux helper cannot be
 * started, Show() falls back to the Wine dialog.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>

#define MAX_RESULTS 256

typedef NTSTATUS (CDECL *spawnvp_fn)(char *const argv[], int wait);
typedef char *(CDECL *get_unix_fn)(const WCHAR *dos);
typedef WCHAR *(CDECL *get_dos_fn)(const char *unix);

static HINSTANCE self;
static LONG locks;

struct dialog
{
    IFileSaveDialog iface;      /* IFileOpenDialog shares the IFileDialog prefix */
    LONG ref;
    BOOL save;
    IFileDialog *inner;
    /* state captured from the application */
    WCHAR *title;
    WCHAR *filename;
    WCHAR *default_ext;
    COMDLG_FILTERSPEC *filters;
    UINT nfilters;
    UINT filter_index;          /* 1-based, as in the API */
    FILEOPENDIALOGOPTIONS options;
    /* result of the native dialog */
    BOOL have_result;
    UINT nresults;
    IShellItem *results[MAX_RESULTS];
};

static inline struct dialog *impl(void *iface) { return (struct dialog *)iface; }

static void trace(const char *fmt, ...)
{
    static WCHAR path[MAX_PATH];
    static int enabled = -1;
    FILE *f;
    va_list args;
    if (enabled < 0) enabled = GetEnvironmentVariableA("ZBFILEDLG_DEBUG", NULL, 0) > 0;
    if (!enabled) return;
    if (!path[0])
    {
        WCHAR *p;
        GetModuleFileNameW(self, path, MAX_PATH);
        if ((p = wcsrchr(path, '\\'))) lstrcpyW(p + 1, L"zbfiledlg.log");
    }
    if (!(f = _wfopen(path, L"a"))) return;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

static void trace_guid(const char *what, REFIID riid)
{
    trace("%s {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", what, riid->Data1, riid->Data2, riid->Data3,
          riid->Data4[0], riid->Data4[1], riid->Data4[2], riid->Data4[3],
          riid->Data4[4], riid->Data4[5], riid->Data4[6], riid->Data4[7]);
}

static WCHAR *strdupW(const WCHAR *s)
{
    WCHAR *r;
    if (!s) return NULL;
    r = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(s) + 1) * sizeof(WCHAR));
    lstrcpyW(r, s);
    return r;
}

static char *to_utf8(const WCHAR *s)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *r = HeapAlloc(GetProcessHeap(), 0, len);
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r, len, NULL, NULL);
    return r;
}

static void free_filters(struct dialog *d)
{
    UINT i;
    for (i = 0; i < d->nfilters; i++)
    {
        HeapFree(GetProcessHeap(), 0, (void *)d->filters[i].pszName);
        HeapFree(GetProcessHeap(), 0, (void *)d->filters[i].pszSpec);
    }
    HeapFree(GetProcessHeap(), 0, d->filters);
    d->filters = NULL;
    d->nfilters = 0;
}

static void free_results(struct dialog *d)
{
    UINT i;
    for (i = 0; i < d->nresults; i++) IShellItem_Release(d->results[i]);
    d->nresults = 0;
    d->have_result = FALSE;
}

/* ---- native dialog ---------------------------------------------------- */

static char *unix_path(const WCHAR *dos)
{
    static get_unix_fn get_unix;
    char *p, *r;
    if (!get_unix) get_unix = (get_unix_fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    if (!get_unix || !(p = get_unix(dos))) return NULL;
    r = HeapAlloc(GetProcessHeap(), 0, strlen(p) + 1);
    strcpy(r, p);
    HeapFree(GetProcessHeap(), 0, p);
    return r;
}

static WCHAR *dos_path(const char *unix)
{
    static get_dos_fn get_dos;
    if (!get_dos) get_dos = (get_dos_fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
    return get_dos ? get_dos(unix) : NULL;
}

static char *folder_of(struct dialog *d)
{
    IShellItem *item = NULL;
    WCHAR *path = NULL;
    char *r = NULL;
    if (SUCCEEDED(IFileDialog_GetFolder(d->inner, &item)) && item)
    {
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)))
        {
            r = unix_path(path);
            CoTaskMemFree(path);
        }
        IShellItem_Release(item);
    }
    return r;
}

/* Like the Windows dialog: a save name without extension gets the one of the
 * selected file type (or the default extension). Takes ownership of path. */
static WCHAR *add_extension(struct dialog *d, WCHAR *path)
{
    const WCHAR *name = wcsrchr(path, '\\'), *ext = NULL, *spec;
    WCHAR buf[64], *r;
    size_t n;

    name = name ? name + 1 : path;
    if (wcschr(name, '.')) return path;
    if (d->filter_index && d->filter_index <= d->nfilters &&
        (spec = d->filters[d->filter_index - 1].pszSpec) && spec[0] == '*' && spec[1] == '.' &&
        spec[2] && spec[2] != '*')
    {
        n = wcscspn(spec + 2, L";");
        if (n < ARRAYSIZE(buf))
        {
            memcpy(buf, spec + 2, n * sizeof(WCHAR));
            buf[n] = 0;
            ext = buf;
        }
    }
    if (!ext) ext = d->default_ext;
    if (!ext || !ext[0]) return path;
    if (ext[0] == '.') ext++;
    r = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(path) + lstrlenW(ext) + 2) * sizeof(WCHAR));
    swprintf(r, lstrlenW(path) + lstrlenW(ext) + 2, L"%ls.%ls", path, ext);
    HeapFree(GetProcessHeap(), 0, path);
    return r;
}

/* Runs the helper and pumps messages until it writes its result file.
 * Returns S_OK, HRESULT_FROM_WIN32(ERROR_CANCELLED), or E_FAIL to fall back. */
static HRESULT run_native(struct dialog *d, HWND owner)
{
    static spawnvp_fn spawnvp;
    char *argv[64], parent[32], index[16];
    char *script_u = NULL, *out_u = NULL, *folder = NULL, *strings[64];
    WCHAR script[MAX_PATH], tmpdir[MAX_PATH], out[MAX_PATH], *p;
    int argc = 0, nstrings = 0, i;
    HWND top;
    BOOL disabled = FALSE;
    HRESULT hr = E_FAIL;
    FILE *f;
    char line[4096];

    if (!spawnvp) spawnvp = (spawnvp_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
    if (!spawnvp) return E_FAIL;

    GetModuleFileNameW(self, script, MAX_PATH);
    if ((p = wcsrchr(script, '\\'))) lstrcpyW(p + 1, L"zbfiledlg.py");
    GetTempPathW(MAX_PATH, tmpdir);
    swprintf(out, MAX_PATH, L"%lszbfiledlg-%lu-%lu.txt", tmpdir, GetCurrentProcessId(), GetTickCount());
    DeleteFileW(out);
    if (!(script_u = unix_path(script)) || !(out_u = unix_path(out))) goto done;

    top = owner ? GetAncestor(owner, GA_ROOT) : GetActiveWindow();
    parent[0] = 0;
    if (top) sprintf(parent, "%lx", (unsigned long)(ULONG_PTR)GetPropA(top, "__wine_x11_whole_window"));

    argv[argc++] = "/usr/bin/env";
    argv[argc++] = "-u";
    argv[argc++] = "LD_LIBRARY_PATH";
    argv[argc++] = "-u";
    argv[argc++] = "LD_PRELOAD";
    argv[argc++] = "/usr/bin/python3";
    argv[argc++] = script_u;
    argv[argc++] = "--mode";
    argv[argc++] = d->options & FOS_PICKFOLDERS ? "folder" : d->save ? "save"
                 : d->options & FOS_ALLOWMULTISELECT ? "open-multi" : "open";
    argv[argc++] = "--out";
    argv[argc++] = out_u;
    if (parent[0] && strcmp(parent, "0"))
    {
        argv[argc++] = "--parent";
        argv[argc++] = parent;
    }
    if (d->title)
    {
        argv[argc++] = "--title";
        argv[argc++] = strings[nstrings++] = to_utf8(d->title);
    }
    if (d->filename)
    {
        argv[argc++] = "--name";
        argv[argc++] = strings[nstrings++] = to_utf8(d->filename);
    }
    if ((folder = folder_of(d)))
    {
        argv[argc++] = "--folder";
        argv[argc++] = folder;
    }
    for (i = 0; i < (int)d->nfilters && argc < 56; i++)
    {
        WCHAR buf[2048];
        swprintf(buf, 2048, L"%ls|%ls", d->filters[i].pszName, d->filters[i].pszSpec);
        argv[argc++] = "--filter";
        argv[argc++] = strings[nstrings++] = to_utf8(buf);
    }
    sprintf(index, "%u", d->filter_index ? d->filter_index - 1 : 0);
    argv[argc++] = "--filter-index";
    argv[argc++] = index;
    argv[argc] = NULL;

    if (spawnvp(argv, 0)) goto done;

    /* behave like a modal dialog: owner disabled, messages keep flowing */
    if (top && IsWindowEnabled(top))
    {
        EnableWindow(top, FALSE);
        disabled = TRUE;
    }
    for (;;)
    {
        MSG msg;
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) break;
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
    if (disabled)
    {
        EnableWindow(top, TRUE);
        SetForegroundWindow(top);
    }

    if (!(f = _wfopen(out, L"r"))) goto done;
    if (fgets(line, sizeof(line), f))
    {
        if (!strncmp(line, "OK", 2) && fgets(line, sizeof(line), f))
        {
            d->filter_index = atoi(line) + 1;
            while (d->nresults < MAX_RESULTS && fgets(line, sizeof(line), f))
            {
                WCHAR *dos;
                IShellItem *item;
                line[strcspn(line, "\r\n")] = 0;
                trace("helper path: %s", line);
                if (!line[0] || !(dos = dos_path(line))) continue;
                if (d->save)
                {
                    /* the file usually does not exist yet, so build a simple pidl */
                    PIDLIST_ABSOLUTE pidl;
                    dos = add_extension(d, dos);
                    trace("save path: %ls", dos);
                    if ((pidl = SHSimpleIDListFromPath(dos)))
                    {
                        if (SUCCEEDED(SHCreateItemFromIDList(pidl, &IID_IShellItem, (void **)&item)))
                            d->results[d->nresults++] = item;
                        CoTaskMemFree(pidl);
                    }
                }
                else if (SUCCEEDED(SHCreateItemFromParsingName(dos, NULL, &IID_IShellItem, (void **)&item)))
                    d->results[d->nresults++] = item;
                HeapFree(GetProcessHeap(), 0, dos);
            }
            if (d->nresults)
            {
                d->have_result = TRUE;
                hr = S_OK;
            }
            else hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        else if (!strncmp(line, "CANCEL", 6))
            hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    fclose(f);
    DeleteFileW(out);

done:
    trace("run_native hr %08lx results %u", hr, d->nresults);
    for (i = 0; i < nstrings; i++) HeapFree(GetProcessHeap(), 0, strings[i]);
    HeapFree(GetProcessHeap(), 0, script_u);
    HeapFree(GetProcessHeap(), 0, out_u);
    HeapFree(GetProcessHeap(), 0, folder);
    return hr;
}

/* ---- IUnknown / IModalWindow ------------------------------------------ */

static HRESULT STDMETHODCALLTYPE dlg_QueryInterface(IFileSaveDialog *iface, REFIID riid, void **out)
{
    trace_guid("QueryInterface", riid);
    struct dialog *d = impl(iface);
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IModalWindow) ||
        IsEqualIID(riid, &IID_IFileDialog) ||
        (!d->save && IsEqualIID(riid, &IID_IFileOpenDialog)) ||
        (d->save && IsEqualIID(riid, &IID_IFileSaveDialog)))
    {
        *out = iface;
        InterlockedIncrement(&d->ref);
        return S_OK;
    }
    /* IFileDialogCustomize, IObjectWithSite, ... come from the Wine object */
    return IFileDialog_QueryInterface(d->inner, riid, out);
}

static ULONG STDMETHODCALLTYPE dlg_AddRef(IFileSaveDialog *iface)
{
    trace("dlg_AddRef");
    return InterlockedIncrement(&impl(iface)->ref);
}

static ULONG STDMETHODCALLTYPE dlg_Release(IFileSaveDialog *iface)
{
    trace("dlg_Release");
    struct dialog *d = impl(iface);
    ULONG ref = InterlockedDecrement(&d->ref);
    if (!ref)
    {
        free_results(d);
        free_filters(d);
        HeapFree(GetProcessHeap(), 0, d->title);
        HeapFree(GetProcessHeap(), 0, d->filename);
        HeapFree(GetProcessHeap(), 0, d->default_ext);
        IFileDialog_Release(d->inner);
        HeapFree(GetProcessHeap(), 0, d);
        InterlockedDecrement(&locks);
    }
    return ref;
}

static HRESULT STDMETHODCALLTYPE dlg_Show(IFileSaveDialog *iface, HWND owner)
{
    trace("dlg_Show");
    struct dialog *d = impl(iface);
    HRESULT hr;
    free_results(d);
    hr = run_native(d, owner);
    if (hr == E_FAIL) return IFileDialog_Show(d->inner, owner);
    return hr;
}

/* ---- IFileDialog ------------------------------------------------------ */

static HRESULT STDMETHODCALLTYPE dlg_SetFileTypes(IFileSaveDialog *iface, UINT count, const COMDLG_FILTERSPEC *specs)
{
    trace("dlg_SetFileTypes");
    struct dialog *d = impl(iface);
    UINT i;
    HRESULT hr = IFileDialog_SetFileTypes(d->inner, count, specs);
    if (FAILED(hr)) return hr;
    free_filters(d);
    d->filters = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*specs));
    for (i = 0; i < count; i++)
    {
        d->filters[i].pszName = strdupW(specs[i].pszName ? specs[i].pszName : L"");
        d->filters[i].pszSpec = strdupW(specs[i].pszSpec ? specs[i].pszSpec : L"*");
    }
    d->nfilters = count;
    for (i = 0; i < count; i++) trace("  filter %u: %ls | %ls", i, d->filters[i].pszName, d->filters[i].pszSpec);
    if (!d->filter_index) d->filter_index = 1;
    return hr;
}

static HRESULT STDMETHODCALLTYPE dlg_SetFileTypeIndex(IFileSaveDialog *iface, UINT index)
{
    trace("dlg_SetFileTypeIndex");
    struct dialog *d = impl(iface);
    d->filter_index = index;
    return IFileDialog_SetFileTypeIndex(d->inner, index);
}

static HRESULT STDMETHODCALLTYPE dlg_GetFileTypeIndex(IFileSaveDialog *iface, UINT *index)
{
    trace("dlg_GetFileTypeIndex");
    struct dialog *d = impl(iface);
    if (!d->have_result) return IFileDialog_GetFileTypeIndex(d->inner, index);
    if (!index) return E_INVALIDARG;
    *index = d->filter_index;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dlg_Advise(IFileSaveDialog *iface, IFileDialogEvents *events, DWORD *cookie)
{
    trace("dlg_Advise");
    return IFileDialog_Advise(impl(iface)->inner, events, cookie);
}

static HRESULT STDMETHODCALLTYPE dlg_Unadvise(IFileSaveDialog *iface, DWORD cookie)
{
    trace("dlg_Unadvise");
    return IFileDialog_Unadvise(impl(iface)->inner, cookie);
}

static HRESULT STDMETHODCALLTYPE dlg_SetOptions(IFileSaveDialog *iface, FILEOPENDIALOGOPTIONS options)
{
    trace("dlg_SetOptions");
    struct dialog *d = impl(iface);
    HRESULT hr = IFileDialog_SetOptions(d->inner, options);
    if (SUCCEEDED(hr)) d->options = options;
    return hr;
}

static HRESULT STDMETHODCALLTYPE dlg_GetOptions(IFileSaveDialog *iface, FILEOPENDIALOGOPTIONS *options)
{
    trace("dlg_GetOptions");
    return IFileDialog_GetOptions(impl(iface)->inner, options);
}

static HRESULT STDMETHODCALLTYPE dlg_SetDefaultFolder(IFileSaveDialog *iface, IShellItem *item)
{
    trace("dlg_SetDefaultFolder");
    return IFileDialog_SetDefaultFolder(impl(iface)->inner, item);
}

static HRESULT STDMETHODCALLTYPE dlg_SetFolder(IFileSaveDialog *iface, IShellItem *item)
{
    trace("dlg_SetFolder");
    return IFileDialog_SetFolder(impl(iface)->inner, item);
}

static HRESULT STDMETHODCALLTYPE dlg_GetFolder(IFileSaveDialog *iface, IShellItem **item)
{
    trace("dlg_GetFolder");
    struct dialog *d = impl(iface);
    if (d->have_result && item && SUCCEEDED(IShellItem_GetParent(d->results[0], item))) return S_OK;
    return IFileDialog_GetFolder(d->inner, item);
}

static HRESULT STDMETHODCALLTYPE dlg_GetCurrentSelection(IFileSaveDialog *iface, IShellItem **item)
{
    trace("dlg_GetCurrentSelection");
    struct dialog *d = impl(iface);
    if (!d->have_result) return IFileDialog_GetCurrentSelection(d->inner, item);
    if (!item) return E_INVALIDARG;
    *item = d->results[0];
    IShellItem_AddRef(*item);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dlg_SetFileName(IFileSaveDialog *iface, LPCWSTR name)
{
    trace("dlg_SetFileName");
    struct dialog *d = impl(iface);
    HeapFree(GetProcessHeap(), 0, d->filename);
    d->filename = strdupW(name);
    return IFileDialog_SetFileName(d->inner, name);
}

static HRESULT STDMETHODCALLTYPE dlg_GetFileName(IFileSaveDialog *iface, LPWSTR *name)
{
    trace("dlg_GetFileName");
    struct dialog *d = impl(iface);
    if (d->have_result && name &&
        SUCCEEDED(IShellItem_GetDisplayName(d->results[0], SIGDN_PARENTRELATIVEPARSING, name)))
        return S_OK;
    return IFileDialog_GetFileName(d->inner, name);
}

static HRESULT STDMETHODCALLTYPE dlg_SetTitle(IFileSaveDialog *iface, LPCWSTR title)
{
    trace("dlg_SetTitle");
    struct dialog *d = impl(iface);
    HeapFree(GetProcessHeap(), 0, d->title);
    d->title = strdupW(title);
    return IFileDialog_SetTitle(d->inner, title);
}

static HRESULT STDMETHODCALLTYPE dlg_SetOkButtonLabel(IFileSaveDialog *iface, LPCWSTR text)
{
    trace("dlg_SetOkButtonLabel");
    return IFileDialog_SetOkButtonLabel(impl(iface)->inner, text);
}

static HRESULT STDMETHODCALLTYPE dlg_SetFileNameLabel(IFileSaveDialog *iface, LPCWSTR text)
{
    trace("dlg_SetFileNameLabel");
    return IFileDialog_SetFileNameLabel(impl(iface)->inner, text);
}

static HRESULT STDMETHODCALLTYPE dlg_GetResult(IFileSaveDialog *iface, IShellItem **item)
{
    trace("dlg_GetResult");
    struct dialog *d = impl(iface);
    if (!d->have_result) return IFileDialog_GetResult(d->inner, item);
    if (!item) return E_INVALIDARG;
    *item = d->results[0];
    IShellItem_AddRef(*item);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dlg_AddPlace(IFileSaveDialog *iface, IShellItem *item, FDAP where)
{
    trace("dlg_AddPlace");
    return IFileDialog_AddPlace(impl(iface)->inner, item, where);
}

static HRESULT STDMETHODCALLTYPE dlg_SetDefaultExtension(IFileSaveDialog *iface, LPCWSTR ext)
{
    struct dialog *d = impl(iface);
    trace("dlg_SetDefaultExtension %ls", ext ? ext : L"(null)");
    HeapFree(GetProcessHeap(), 0, d->default_ext);
    d->default_ext = strdupW(ext);
    return IFileDialog_SetDefaultExtension(d->inner, ext);
}

static HRESULT STDMETHODCALLTYPE dlg_Close(IFileSaveDialog *iface, HRESULT hr)
{
    trace("dlg_Close");
    return IFileDialog_Close(impl(iface)->inner, hr);
}

static HRESULT STDMETHODCALLTYPE dlg_SetClientGuid(IFileSaveDialog *iface, REFGUID guid)
{
    trace("dlg_SetClientGuid");
    return IFileDialog_SetClientGuid(impl(iface)->inner, guid);
}

static HRESULT STDMETHODCALLTYPE dlg_ClearClientData(IFileSaveDialog *iface)
{
    trace("dlg_ClearClientData");
    return IFileDialog_ClearClientData(impl(iface)->inner);
}

static HRESULT STDMETHODCALLTYPE dlg_SetFilter(IFileSaveDialog *iface, IShellItemFilter *filter)
{
    trace("dlg_SetFilter");
    return IFileDialog_SetFilter(impl(iface)->inner, filter);
}

/* ---- IFileOpenDialog -------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dlg_GetResults(IFileOpenDialog *iface, IShellItemArray **items)
{
    trace("dlg_GetResults");
    struct dialog *d = impl(iface);
    PIDLIST_ABSOLUTE pidls[MAX_RESULTS];
    UINT i, n = 0;
    HRESULT hr;
    if (!d->have_result) return IFileOpenDialog_GetResults((IFileOpenDialog *)d->inner, items);
    if (!items) return E_INVALIDARG;
    for (i = 0; i < d->nresults; i++)
        if (SUCCEEDED(SHGetIDListFromObject((IUnknown *)d->results[i], &pidls[n]))) n++;
    hr = SHCreateShellItemArrayFromIDLists(n, (PCIDLIST_ABSOLUTE_ARRAY)pidls, items);
    for (i = 0; i < n; i++) CoTaskMemFree(pidls[i]);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dlg_GetSelectedItems(IFileOpenDialog *iface, IShellItemArray **items)
{
    trace("dlg_GetSelectedItems");
    struct dialog *d = impl(iface);
    if (!d->have_result) return IFileOpenDialog_GetSelectedItems((IFileOpenDialog *)d->inner, items);
    return dlg_GetResults(iface, items);
}

/* ---- IFileSaveDialog -------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dlg_SetSaveAsItem(IFileSaveDialog *iface, IShellItem *item)
{
    trace("dlg_SetSaveAsItem");
    return IFileSaveDialog_SetSaveAsItem((IFileSaveDialog *)impl(iface)->inner, item);
}

static HRESULT STDMETHODCALLTYPE dlg_SetProperties(IFileSaveDialog *iface, IPropertyStore *store)
{
    trace("dlg_SetProperties");
    return IFileSaveDialog_SetProperties((IFileSaveDialog *)impl(iface)->inner, store);
}

static HRESULT STDMETHODCALLTYPE dlg_SetCollectedProperties(IFileSaveDialog *iface, IPropertyDescriptionList *list, WINBOOL append)
{
    trace("dlg_SetCollectedProperties");
    return IFileSaveDialog_SetCollectedProperties((IFileSaveDialog *)impl(iface)->inner, list, append);
}

static HRESULT STDMETHODCALLTYPE dlg_GetProperties(IFileSaveDialog *iface, IPropertyStore **store)
{
    trace("dlg_GetProperties");
    return IFileSaveDialog_GetProperties((IFileSaveDialog *)impl(iface)->inner, store);
}

static HRESULT STDMETHODCALLTYPE dlg_ApplyProperties(IFileSaveDialog *iface, IShellItem *item, IPropertyStore *store,
                                                     HWND hwnd, IFileOperationProgressSink *sink)
{
    trace("dlg_ApplyProperties");
    return IFileSaveDialog_ApplyProperties((IFileSaveDialog *)impl(iface)->inner, item, store, hwnd, sink);
}

#define FILEDIALOG_METHODS \
    dlg_QueryInterface, dlg_AddRef, dlg_Release, dlg_Show, \
    dlg_SetFileTypes, dlg_SetFileTypeIndex, dlg_GetFileTypeIndex, dlg_Advise, dlg_Unadvise, \
    dlg_SetOptions, dlg_GetOptions, dlg_SetDefaultFolder, dlg_SetFolder, dlg_GetFolder, \
    dlg_GetCurrentSelection, dlg_SetFileName, dlg_GetFileName, dlg_SetTitle, dlg_SetOkButtonLabel, \
    dlg_SetFileNameLabel, dlg_GetResult, dlg_AddPlace, dlg_SetDefaultExtension, dlg_Close, \
    dlg_SetClientGuid, dlg_ClearClientData, dlg_SetFilter

static const IFileSaveDialogVtbl save_vtbl =
{
    FILEDIALOG_METHODS,
    dlg_SetSaveAsItem, dlg_SetProperties, dlg_SetCollectedProperties, dlg_GetProperties, dlg_ApplyProperties
};

/* IFileOpenDialog methods take an IFileOpenDialog*, same object layout */
static const struct
{
    void *methods[27];
    HRESULT (STDMETHODCALLTYPE *GetResults)(IFileOpenDialog *, IShellItemArray **);
    HRESULT (STDMETHODCALLTYPE *GetSelectedItems)(IFileOpenDialog *, IShellItemArray **);
} open_vtbl =
{
    { FILEDIALOG_METHODS },
    dlg_GetResults, dlg_GetSelectedItems
};

/* ---- class factory ---------------------------------------------------- */

struct factory
{
    IClassFactory iface;
    BOOL save;
};

static HRESULT STDMETHODCALLTYPE cf_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE cf_AddRef(IClassFactory *iface) { return 2; }
static ULONG STDMETHODCALLTYPE cf_Release(IClassFactory *iface) { return 1; }

static HRESULT STDMETHODCALLTYPE cf_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
    struct factory *cf = (struct factory *)iface;
    HRESULT (WINAPI *get_class)(REFCLSID, REFIID, void **);
    IClassFactory *wine_cf;
    struct dialog *d;
    HMODULE comdlg;
    HRESULT hr;

    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(comdlg = LoadLibraryW(L"comdlg32.dll"))) return E_FAIL;
    if (!(get_class = (void *)GetProcAddress(comdlg, "DllGetClassObject"))) return E_FAIL;
    hr = get_class(cf->save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog, &IID_IClassFactory, (void **)&wine_cf);
    if (FAILED(hr)) return hr;

    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    hr = IClassFactory_CreateInstance(wine_cf, NULL, &IID_IFileDialog, (void **)&d->inner);
    IClassFactory_Release(wine_cf);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, d);
        return hr;
    }
    d->iface.lpVtbl = cf->save ? (IFileSaveDialogVtbl *)&save_vtbl : (IFileSaveDialogVtbl *)&open_vtbl;
    d->save = cf->save;
    d->ref = 1;
    IFileDialog_GetOptions(d->inner, &d->options);
    InterlockedIncrement(&locks);
    hr = dlg_QueryInterface(&d->iface, riid, out);
    dlg_Release(&d->iface);
    return hr;
}

static HRESULT STDMETHODCALLTYPE cf_LockServer(IClassFactory *iface, BOOL lock)
{
    if (lock) InterlockedIncrement(&locks);
    else InterlockedDecrement(&locks);
    return S_OK;
}

static const IClassFactoryVtbl cf_vtbl =
{
    cf_QueryInterface, cf_AddRef, cf_Release, cf_CreateInstance, cf_LockServer
};

static struct factory open_factory = { { (IClassFactoryVtbl *)&cf_vtbl }, FALSE };
static struct factory save_factory = { { (IClassFactoryVtbl *)&cf_vtbl }, TRUE };

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    if (IsEqualCLSID(clsid, &CLSID_FileOpenDialog))
        return IClassFactory_QueryInterface(&open_factory.iface, riid, out);
    if (IsEqualCLSID(clsid, &CLSID_FileSaveDialog))
        return IClassFactory_QueryInterface(&save_factory.iface, riid, out);
    *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return locks ? S_FALSE : S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
