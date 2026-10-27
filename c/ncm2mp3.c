// ncm2mp3.c —— 网易云 .ncm 解密为 mp3 / flac
//
// 编译（VS2022 x64 开发人员命令提示符）：
//     cl /O2 /utf-8 ncm2mp3.c bcrypt.lib
//
// 用法：
//     ncm2mp3.exe <文件或文件夹> [更多...]
//     直接把文件/文件夹拖到 exe 图标上也可以
//
// 说明：
//     * AES 用 Windows 自带 BCrypt，不依赖 openssl
//     * 支持中文/日文路径（全程宽字符）

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "bcrypt.lib")

#define NCM_KEY   "hzHRAmso5kInbaxW"
#define PREFIX    "neteasecloudmusic"
#define MAX_PATH_LONG 32768

static int g_ok = 0, g_fail = 0;
static unsigned long long g_bytes = 0;

// ---------------------------------------------------------------- AES
// 用 BCrypt 做 AES-128-ECB 解密（无填充）
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

    // 关闭填充：不传 BCRYPT_BLOCK_PADDING
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

// ---------------------------------------------------------------- 工具
static unsigned int rd_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

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

static void print_w(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0, written = 0;
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        // 控制台：直接写宽字符，中文不会乱码
        WriteConsoleW(h, buf, (DWORD)wcslen(buf), &written, NULL);
        WriteConsoleW(h, L"\r\n", 2, &written, NULL);
        return;
    }
    // 被重定向：转 UTF-8 输出
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
    if (n > 1) {
        char *u = (char *)malloc(n);
        if (u) {
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, u, n, NULL, NULL);
            fwrite(u, 1, (size_t)n - 1, stdout);
            fputc('\n', stdout);
            free(u);
        }
        return;
    }
    wprintf(L"%ls\n", buf);
}

// ---------------------------------------------------------------- 转换
// 返回 0 成功
static int convert_one(const wchar_t *src)
{
    size_t size = 0;
    unsigned char *raw = read_file(src, &size);
    if (!raw) {
        print_w(L"  [失败] 打不开文件: %ls", src);
        g_fail++;
        return 1;
    }
    if (size < 32 || memcmp(raw, "CTENFDAM", 8) != 0) {
        print_w(L"  [失败] 不是 ncm 文件: %ls", src);
        free(raw); g_fail++;
        return 1;
    }

    size_t pos = 10;
    unsigned int key_len = rd_u32(raw + pos); pos += 4;
    if (key_len == 0 || key_len > 0x10000 || pos + key_len > size) {
        print_w(L"  [失败] 密钥长度异常: %ls", src);
        free(raw); g_fail++;
        return 1;
    }

    // 密钥数据 ^0x64
    unsigned char *kdata = (unsigned char *)malloc(key_len);
    if (!kdata) { free(raw); g_fail++; return 1; }
    for (unsigned int i = 0; i < key_len; i++) kdata[i] = raw[pos + i] ^ 0x64;
    pos += key_len;

    // AES 解密（长度按 16 对齐）
    unsigned int dec_len = key_len - (key_len % 16);
    unsigned char *plain = (unsigned char *)calloc(dec_len + 16, 1);
    DWORD out_len = 0;
    if (!plain || aes_ecb_decrypt(kdata, dec_len, plain, &out_len) != 0) {
        print_w(L"  [失败] 密钥解密失败: %ls", src);
        free(kdata); free(plain); free(raw); g_fail++;
        return 1;
    }
    free(kdata);

    // 去掉 PKCS7 填充
    unsigned int plen = out_len;
    unsigned char pad = plain[plen - 1];
    if (pad > 0 && pad <= 16) plen -= pad;

    // 去掉前缀
    unsigned char *audio_key = NULL;
    int audio_key_len = 0;
    if (plen > 17 && memcmp(plain, PREFIX, 17) == 0) {
        audio_key = plain + 17;
        audio_key_len = (int)(plen - 17);
    } else {
        print_w(L"  [失败] 密钥格式不符: %ls", src);
        free(plain); free(raw); g_fail++;
        return 1;
    }

    // 跳过元数据、CRC、封面
    unsigned int meta_len = rd_u32(raw + pos); pos += 4 + meta_len;
    pos += 4 + 5;
    if (pos + 4 > size) {
        print_w(L"  [失败] 文件不完整: %ls", src);
        free(plain); free(raw); g_fail++;
        return 1;
    }
    unsigned int img_len = rd_u32(raw + pos); pos += 4 + img_len;
    if (pos >= size) {
        print_w(L"  [失败] 文件不完整: %ls", src);
        free(plain); free(raw); g_fail++;
        return 1;
    }

    // 还原音频
    size_t audio_len = size - pos;
    unsigned char *audio = (unsigned char *)malloc(audio_len);
    if (!audio) { free(plain); free(raw); g_fail++; return 1; }
    memcpy(audio, raw + pos, audio_len);
    decrypt_audio(audio, audio_len, audio_key, audio_key_len);
    free(plain);
    free(raw);

    const wchar_t *ext = (audio_len >= 4 && memcmp(audio, "fLaC", 4) == 0) ? L".flac" : L".mp3";

    // 输出路径：源文件改扩展名
    wchar_t out[MAX_PATH_LONG];
    wcsncpy(out, src, MAX_PATH_LONG - 8);
    out[MAX_PATH_LONG - 8] = 0;
    wchar_t *dot = wcsrchr(out, L'.');
    wchar_t *slash = wcsrchr(out, L'\\');
    if (dot && (!slash || dot > slash)) *dot = 0;
    wcscat(out, ext);

    // 重名加序号
    wchar_t final_path[MAX_PATH_LONG];
    wcscpy(final_path, out);
    int n = 2;
    while (GetFileAttributesW(final_path) != INVALID_FILE_ATTRIBUTES) {
        wchar_t base[MAX_PATH_LONG];
        wcscpy(base, out);
        wchar_t *d2 = wcsrchr(base, L'.');
        if (d2) *d2 = 0;
        _snwprintf(final_path, MAX_PATH_LONG, L"%ls (%d)%ls", base, n++, ext);
    }

    FILE *fp = _wfopen(final_path, L"wb");
    if (!fp) {
        print_w(L"  [失败] 写不了输出文件: %ls", final_path);
        free(audio); g_fail++;
        return 1;
    }
    size_t wrote = fwrite(audio, 1, audio_len, fp);
    fclose(fp);
    free(audio);

    if (wrote != audio_len) {
        print_w(L"  [失败] 写入不完整: %ls", final_path);
        g_fail++;
        return 1;
    }

    // 只打文件名，避免刷屏
    const wchar_t *name = wcsrchr(final_path, L'\\');
    print_w(L"  [OK] %ls  (%.1f MB)", name ? name + 1 : final_path, audio_len / 1048576.0);
    g_ok++;
    g_bytes += audio_len;
    return 0;
}

// ---------------------------------------------------------------- 遍历
static void handle_path(const wchar_t *path);

static void scan_dir(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH_LONG];
    _snwprintf(pattern, MAX_PATH_LONG, L"%ls\\*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t full[MAX_PATH_LONG];
        _snwprintf(full, MAX_PATH_LONG, L"%ls\\%ls", dir, fd.cFileName);
        handle_path(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void handle_path(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        print_w(L"  [跳过] 路径不存在: %ls", path);
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        scan_dir(path);
        return;
    }
    // 只处理 .ncm
    const wchar_t *dot = wcsrchr(path, L'.');
    if (!dot || _wcsicmp(dot, L".ncm") != 0) return;
    convert_one(path);
}

// ---------------------------------------------------------------- 入口
int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        print_w(L"NCM 转 MP3 —— 命令行版");
        print_w(L"");
        print_w(L"用法：把 .ncm 文件或文件夹拖到本程序图标上");
        print_w(L"      或 ncm2mp3.exe <文件/文件夹> [更多...]");
        print_w(L"");
        print_w(L"输出：与源文件同目录，同名的 .mp3 / .flac");
        system("pause");
        return 1;
    }

    SetConsoleOutputCP(CP_UTF8);
    print_w(L"开始转换，共 %d 个参数", argc - 1);

    for (int i = 1; i < argc; i++) {
        handle_path(argv[i]);
    }

    print_w(L"");
    print_w(L"完成：成功 %d 个，失败 %d 个，输出 %.1f MB",
            g_ok, g_fail, g_bytes / 1048576.0);

    if (argc == 2 && !getenv("NCM_NOPAUSE")) system("pause");
    return g_fail == 0 ? 0 : 1;
}
