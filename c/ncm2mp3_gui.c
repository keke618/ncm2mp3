// ncm2mp3_gui.c —— NCM 转 MP3（Win32 图形界面）
//
// 功能与 Python 版一致：
//   拖拽文件/文件夹、批量添加、原地或指定目录输出、可选删除源文件、进度条
//
// 编译（VS2022 x64）：
//   rc /fo ncm2mp3_gui.res ncm2mp3_gui.rc
//   cl /O2 /utf-8 ncm2mp3_gui.c ncm2mp3_gui.res bcrypt.lib comctl32.lib shell32.lib ole32.lib

#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#define NCM_KEY  "hzHRAmso5kInbaxW"
#define PREFIX   "neteasecloudmusic"
#define MAXP     32768
#define MAXFILES 4096

#define ID_LIST      1001
#define ID_ADDFILE   1002
#define ID_ADDDIR    1003
#define ID_CLEAR     1004
#define ID_INPLACE   1005
#define ID_CUSTOM    1006
#define ID_OUTEDIT   1007
#define ID_BROWSE    1008
#define ID_DELSRC    1009
#define ID_START     1010
#define ID_PROG      1011
#define ID_STATUS    1012
#define ID_DROPTIP   1013

static HWND hList, hEdit, hProg, hStatus, hStart, hChk, hR1, hR2, hBrowse;
static HFONT hFont, hFontTitle;
static HBRUSH hBgBrush;
static wchar_t *g_files[MAXFILES];
static int g_count = 0;
static volatile BOOL g_running = FALSE;

// ---------------------------------------------------------------- AES
static int aes_ecb_decrypt(const unsigned char *in, DWORD in_len, unsigned char *out, DWORD *out_len)
{
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    int ret = -1;
    DWORD cb = 0;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0)))
        return -1;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                                          (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0,
                                                   (PUCHAR)NCM_KEY, (ULONG)strlen(NCM_KEY), 0)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptDecrypt(hKey, (PUCHAR)in, in_len, NULL, NULL, 0,
                                      out, in_len, &cb, 0)))
        goto done;
    *out_len = cb;
    ret = 0;
done:
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ret;
}

// ---------------------------------------------------------------- RC4 变种
static void build_key_box(const unsigned char *key, int key_len, unsigned char *box)
{
    int i, last = 0;
    for (i = 0; i < 256; i++) box[i] = (unsigned char)i;
    for (i = 0; i < 256; i++) {
        unsigned char c = box[i];
        last = (c + last + key[i % key_len]) & 0xFF;
        box[i] = box[last];
        box[last] = c;
    }
}

static void decrypt_audio(unsigned char *data, size_t len, const unsigned char *key, int key_len)
{
    unsigned char box[256];
    size_t i;
    build_key_box(key, key_len, box);
    for (i = 0; i < len; i++) {
        unsigned int j = (unsigned int)((i + 1) & 0xFF);
        data[i] ^= box[(box[j] + box[(box[j] + j) & 0xFF]) & 0xFF];
    }
}

static unsigned int rd_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

// ---------------------------------------------------------------- 文件
static unsigned char *read_file(const wchar_t *path, size_t *size)
{
    FILE *fp = _wfopen(path, L"rb");
    if (!fp) return NULL;
    _fseeki64(fp, 0, SEEK_END);
    __int64 len = _ftelli64(fp);
    if (len <= 0 || len > (__int64)1 << 31) { fclose(fp); return NULL; }
    _fseeki64(fp, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)len);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) { free(buf); fclose(fp); return NULL; }
    fclose(fp);
    *size = (size_t)len;
    return buf;
}

// 返回 0 成功；out_dir 为 NULL 表示原地
static int convert_one(const wchar_t *src, const wchar_t *out_dir, BOOL del_src, wchar_t *out_path, int out_cch)
{
    size_t size = 0;
    unsigned char *raw = read_file(src, &size);
    if (!raw) return -1;
    if (size < 32 || memcmp(raw, "CTENFDAM", 8) != 0) { free(raw); return -2; }

    size_t pos = 10;
    unsigned int key_len = rd_u32(raw + pos); pos += 4;
    if (key_len == 0 || key_len > 0x10000 || pos + key_len > size) { free(raw); return -2; }

    unsigned char *kdata = (unsigned char *)malloc(key_len);
    if (!kdata) { free(raw); return -3; }
    for (unsigned int i = 0; i < key_len; i++) kdata[i] = raw[pos + i] ^ 0x64;
    pos += key_len;

    unsigned int dec_len = key_len - (key_len % 16);
    unsigned char *plain = (unsigned char *)calloc(dec_len + 16, 1);
    DWORD out_len = 0;
    if (!plain || aes_ecb_decrypt(kdata, dec_len, plain, &out_len) != 0) {
        free(kdata); free(plain); free(raw); return -3;
    }
    free(kdata);

    unsigned int plen = out_len;
    unsigned char pad = plain[plen - 1];
    if (pad > 0 && pad <= 16) plen -= pad;

    unsigned char *audio_key;
    int audio_key_len;
    if (plen > 17 && memcmp(plain, PREFIX, 17) == 0) {
        audio_key = plain + 17;
        audio_key_len = (int)(plen - 17);
    } else {
        free(plain); free(raw); return -4;
    }

    unsigned int meta_len = rd_u32(raw + pos); pos += 4 + meta_len;
    pos += 4 + 5;
    if (pos + 4 > size) { free(plain); free(raw); return -5; }
    unsigned int img_len = rd_u32(raw + pos); pos += 4 + img_len;
    if (pos >= size) { free(plain); free(raw); return -5; }

    size_t audio_len = size - pos;
    unsigned char *audio = (unsigned char *)malloc(audio_len);
    if (!audio) { free(plain); free(raw); return -6; }
    memcpy(audio, raw + pos, audio_len);
    decrypt_audio(audio, audio_len, audio_key, audio_key_len);
    free(plain);
    free(raw);

    const wchar_t *ext = (audio_len >= 4 && memcmp(audio, "fLaC", 4) == 0) ? L".flac" : L".mp3";

    wchar_t base[MAXP];
    wcsncpy(base, src, MAXP - 8);
    base[MAXP - 8] = 0;
    wchar_t *dot = wcsrchr(base, L'.');
    wchar_t *slash = wcsrchr(base, L'\\');
    if (dot && (!slash || dot > slash)) *dot = 0;

    wchar_t target[MAXP];
    if (out_dir) _snwprintf(target, MAXP, L"%ls\\%ls%ls", out_dir,
                            wcsrchr(base, L'\\') ? wcsrchr(base, L'\\') + 1 : base, ext);
    else _snwprintf(target, MAXP, L"%ls%ls", base, ext);

    wchar_t final_path[MAXP];
    wcscpy(final_path, target);
    int n = 2;
    while (GetFileAttributesW(final_path) != INVALID_FILE_ATTRIBUTES) {
        wchar_t tmp2[MAXP];
        wcscpy(tmp2, target);
        wchar_t *d2 = wcsrchr(tmp2, L'.');
        if (d2) *d2 = 0;
        _snwprintf(final_path, MAXP, L"%ls (%d)%ls", tmp2, n++, ext);
    }

    FILE *fp = _wfopen(final_path, L"wb");
    if (!fp) { free(audio); return -7; }
    size_t wrote = fwrite(audio, 1, audio_len, fp);
    fclose(fp);
    free(audio);
    if (wrote != audio_len) return -7;

    if (del_src) _wremove(src);

    if (out_path && out_cch > 0) {
        wcsncpy(out_path, final_path, out_cch - 1);
        out_path[out_cch - 1] = 0;
    }
    return 0;
}

// ---------------------------------------------------------------- 列表
static void add_one(const wchar_t *path)
{
    if (g_count >= MAXFILES) return;
    for (int i = 0; i < g_count; i++) {
        if (_wcsicmp(g_files[i], path) == 0) return;
    }
    size_t len = wcslen(path);
    wchar_t *copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) return;
    wcscpy(copy, path);
    g_files[g_count++] = copy;

    const wchar_t *name = wcsrchr(path, L'\\');
    wchar_t buf[MAXP];
    _snwprintf(buf, MAXP, L"  %ls", name ? name + 1 : path);
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)buf);
}

static void add_path(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return;

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        wchar_t pattern[MAXP];
        _snwprintf(pattern, MAXP, L"%ls\\*", path);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t full[MAXP];
            _snwprintf(full, MAXP, L"%ls\\%ls", path, fd.cFileName);
            add_path(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return;
    }

    const wchar_t *dot = wcsrchr(path, L'.');
    if (dot && _wcsicmp(dot, L".ncm") == 0) add_one(path);
}

static void refresh_count(void)
{
    wchar_t buf[64];
    _snwprintf(buf, 64, L"%d 个文件", g_count);
    SetDlgItemTextW(GetParent(hList), ID_STATUS, buf);
}

static void set_status(const wchar_t *text)
{
    SetWindowTextW(hStatus, text);
}

// ---------------------------------------------------------------- 界面
static void layout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    MoveWindow(GetDlgItem(hwnd, ID_DROPTIP), 20, 70, W - 40, 56, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ADDFILE), 20, 136, 100, 30, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ADDDIR), 128, 136, 110, 30, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_CLEAR), 246, 136, 90, 30, TRUE);
    MoveWindow(hStatus, W - 140, 142, 120, 20, TRUE);

    int listTop = 176, listH = H - 176 - 200;
    MoveWindow(hList, 20, listTop, W - 40, listH, TRUE);

    int y = listTop + listH + 12;
    MoveWindow(GetDlgItem(hwnd, ID_INPLACE), 24, y, 240, 22, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_CUSTOM), 24, y + 26, 70, 22, TRUE);
    MoveWindow(hEdit, 94, y + 25, W - 40 - 94 - 76, 24, TRUE);
    MoveWindow(hBrowse, W - 40 - 68, y + 24, 68, 26, TRUE);
    MoveWindow(hChk, 24, y + 56, 320, 22, TRUE);

    MoveWindow(hProg, 20, y + 88, W - 40, 16, TRUE);
    MoveWindow(hStart, 20, y + 114, 110, 34, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_STATUS), 142, y + 122, W - 170, 22, TRUE);
}

static void pump_messages(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void do_convert(HWND hwnd)
{
    if (g_running) return;
    if (g_count == 0) {
        MessageBoxW(hwnd, L"先添加几个 .ncm 文件吧", L"提示", MB_ICONINFORMATION);
        return;
    }

    wchar_t out_dir[MAXP];
    out_dir[0] = 0;
    BOOL custom = (SendMessageW(hR2, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (custom) {
        GetWindowTextW(hEdit, out_dir, MAXP);
        if (out_dir[0] == 0) {
            MessageBoxW(hwnd, L"请选择导出目录", L"提示", MB_ICONWARNING);
            return;
        }
        CreateDirectoryW(out_dir, NULL);
    }

    BOOL del_src = (SendMessageW(hChk, BM_GETCHECK, 0, 0) == BST_CHECKED);

    g_running = TRUE;
    EnableWindow(hStart, FALSE);
    SendMessageW(hProg, PBM_SETRANGE32, 0, g_count);
    SendMessageW(hProg, PBM_SETPOS, 0, 0);

    int ok = 0, fail = 0;
    for (int i = 0; i < g_count; i++) {
        wchar_t out[MAXP];
        wchar_t namebuf[MAXP];
        const wchar_t *name = wcsrchr(g_files[i], L'\\');
        name = name ? name + 1 : g_files[i];

        int rc = convert_one(g_files[i], custom ? out_dir : NULL, del_src, out, MAXP);
        if (rc == 0) {
            ok++;
            const wchar_t *oname = wcsrchr(out, L'\\');
            _snwprintf(namebuf, MAXP, L"  ✓ %ls", oname ? oname + 1 : out);
        } else {
            fail++;
            _snwprintf(namebuf, MAXP, L"  ✗ %ls", name);
        }
        SendMessageW(hList, LB_DELETESTRING, i, 0);
        SendMessageW(hList, LB_INSERTSTRING, i, (LPARAM)namebuf);

        SendMessageW(hProg, PBM_SETPOS, i + 1, 0);
        wchar_t st[128];
        _snwprintf(st, 128, L"%d/%d", i + 1, g_count);
        set_status(st);
        pump_messages();
    }

    g_running = FALSE;
    EnableWindow(hStart, TRUE);

    wchar_t msg[256];
    if (fail == 0) {
        _snwprintf(msg, 256, L"全部转换成功：%d 个", ok);
        set_status(msg);
        MessageBoxW(hwnd, msg, L"完成", MB_ICONINFORMATION);
    } else {
        _snwprintf(msg, 256, L"成功 %d 个，失败 %d 个\n失败的通常是文件损坏或不是 ncm", ok, fail);
        set_status(msg);
        MessageBoxW(hwnd, msg, L"完成", MB_ICONWARNING);
    }
}

static void pick_files(HWND hwnd)
{
    wchar_t buf[MAXP];
    buf[0] = 0;
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"网易云音乐 (*.ncm)\0*.ncm\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAXP;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) return;

    wchar_t dir[MAXP];
    wcscpy(dir, buf);
    wchar_t *p = buf + wcslen(buf) + 1;
    if (*p == 0) {
        add_path(dir);
    } else {
        while (*p) {
            wchar_t full[MAXP];
            _snwprintf(full, MAXP, L"%ls\\%ls", dir, p);
            add_one(full);
            p += wcslen(p) + 1;
        }
    }
    refresh_count();
}

static void pick_dir(HWND hwnd, BOOL for_output)
{
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.lpszTitle = for_output ? L"选择导出目录" : L"选择包含 ncm 的文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAXP];
    if (SHGetPathFromIDListW(pidl, path)) {
        if (for_output) SetWindowTextW(hEdit, path);
        else { add_path(path); refresh_count(); }
    }
    CoTaskMemFree(pidl);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm;
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        hFont = CreateFontIndirectW(&ncm.lfMessageFont);

        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -22;
        lf.lfWeight = FW_BOLD;
        wcscpy(lf.lfFaceName, L"Microsoft YaHei UI");
        hFontTitle = CreateFontIndirectW(&lf);

        hBgBrush = CreateSolidBrush(RGB(244, 245, 247));

        HWND hTitle = CreateWindowW(L"STATIC", L"NCM 转 MP3", WS_CHILD | WS_VISIBLE,
                                    20, 14, 300, 32, hwnd, NULL, NULL, NULL);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

        HWND hSub = CreateWindowW(L"STATIC", L"网易云音乐加密格式解密 · 把 .ncm 拖进窗口",
                                  WS_CHILD | WS_VISIBLE, 22, 46, 520, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hSub, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND tip = CreateWindowW(L"STATIC", L"把 .ncm 文件或文件夹拖到这里\n（也可以点下面的按钮选择）",
                                 WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | WS_BORDER,
                                 20, 70, 600, 56, hwnd, (HMENU)ID_DROPTIP, NULL, NULL);
        SendMessageW(tip, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND b1 = CreateWindowW(L"BUTTON", L"添加文件", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                20, 136, 100, 30, hwnd, (HMENU)ID_ADDFILE, NULL, NULL);
        HWND b2 = CreateWindowW(L"BUTTON", L"添加文件夹", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                128, 136, 110, 30, hwnd, (HMENU)ID_ADDDIR, NULL, NULL);
        HWND b3 = CreateWindowW(L"BUTTON", L"清空列表", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                246, 136, 90, 30, hwnd, (HMENU)ID_CLEAR, NULL, NULL);
        SendMessageW(b1, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(b2, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(b3, WM_SETFONT, (WPARAM)hFont, TRUE);

        hList = CreateWindowW(L"LISTBOX", NULL,
                              WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                              LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                              20, 176, 600, 300, hwnd, (HMENU)ID_LIST, NULL, NULL);
        SendMessageW(hList, WM_SETFONT, (WPARAM)hFont, TRUE);

        hR1 = CreateWindowW(L"BUTTON", L"原地生成（和原文件放一起）",
                            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                            24, 490, 260, 22, hwnd, (HMENU)ID_INPLACE, NULL, NULL);
        hR2 = CreateWindowW(L"BUTTON", L"导出到",
                            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                            24, 516, 70, 22, hwnd, (HMENU)ID_CUSTOM, NULL, NULL);
        SendMessageW(hR1, BM_SETCHECK, BST_CHECKED, 0);

        hEdit = CreateWindowW(L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                              94, 515, 420, 24, hwnd, (HMENU)ID_OUTEDIT, NULL, NULL);
        EnableWindow(hEdit, FALSE);

        hBrowse = CreateWindowW(L"BUTTON", L"浏览", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                520, 514, 68, 26, hwnd, (HMENU)ID_BROWSE, NULL, NULL);
        EnableWindow(hBrowse, FALSE);

        hChk = CreateWindowW(L"BUTTON", L"转换成功后删除原 .ncm 文件",
                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             24, 546, 320, 22, hwnd, (HMENU)ID_DELSRC, NULL, NULL);

        hProg = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE,
                              20, 578, 600, 16, hwnd, (HMENU)ID_PROG, NULL, NULL);
        SendMessageW(hProg, PBM_SETRANGE32, 0, 100);

        hStart = CreateWindowW(L"BUTTON", L"开始转换", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               20, 604, 110, 34, hwnd, (HMENU)ID_START, NULL, NULL);

        hStatus = CreateWindowW(L"STATIC", L"0 个文件", WS_CHILD | WS_VISIBLE,
                                142, 612, 480, 22, hwnd, (HMENU)ID_STATUS, NULL, NULL);

        SendMessageW(hChk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hR1, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hR2, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hStart, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_DROPFILES: {
        HDROP hd = (HDROP)wParam;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t path[MAXP];
            DragQueryFileW(hd, i, path, MAXP);
            add_path(path);
        }
        DragFinish(hd);
        refresh_count();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_ADDFILE: pick_files(hwnd); break;
        case ID_ADDDIR: pick_dir(hwnd, FALSE); break;
        case ID_BROWSE: pick_dir(hwnd, TRUE); break;
        case ID_CLEAR:
            if (g_running) break;
            for (int i = 0; i < g_count; i++) free(g_files[i]);
            g_count = 0;
            SendMessageW(hList, LB_RESETCONTENT, 0, 0);
            SendMessageW(hProg, PBM_SETPOS, 0, 0);
            refresh_count();
            break;
        case ID_INPLACE:
            EnableWindow(hEdit, FALSE);
            EnableWindow(hBrowse, FALSE);
            break;
        case ID_CUSTOM:
            EnableWindow(hEdit, TRUE);
            EnableWindow(hBrowse, TRUE);
            SetFocus(hEdit);
            break;
        case ID_START:
            do_convert(hwnd);
            break;
        }
        return 0;

    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wParam, RGB(244, 245, 247));
        return (LRESULT)hBgBrush;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitialize(NULL);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(244, 245, 247));
    wc.lpszClassName = L"Ncm2Mp3Gui";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    RegisterClassExW(&wc);

    int W = 680, H = 720;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"Ncm2Mp3Gui", L"NCM 转 MP3",
                                WS_OVERLAPPEDWINDOW,
                                sx, sy, W, H, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    // 支持把文件拖到 exe 图标上启动
    if (cmd && cmd[0]) {
        wchar_t *p = cmd;
        while (*p) {
            wchar_t path[MAXP];
            if (*p == L'"') {
                p++;
                int i = 0;
                while (*p && *p != L'"' && i < MAXP - 1) path[i++] = *p++;
                path[i] = 0;
                if (*p == L'"') p++;
            } else {
                int i = 0;
                while (*p && *p != L' ' && i < MAXP - 1) path[i++] = *p++;
                path[i] = 0;
            }
            if (path[0]) add_path(path);
            while (*p == L' ') p++;
        }
        refresh_count();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
