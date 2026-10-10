/* OfflineTags filter plug-in (.8bf) for Affinity and other Photoshop-compatible hosts.
 *
 * One menu entry, "Analyze": it looks for privacy badges on the current layer
 * and applies what each badge asks for. Blur Me pixelates the wearer's face,
 * Tag Me and Upload Me draw a consent box, No Photo changes nothing and warns.
 * A message box then says what was found and what was done.
 *
 * The plug-in does no image analysis itself. It writes the pixels to a BMP,
 * runs "offlinetags-bridge process" on it (which talks to the daemon, starting
 * it if needed) and copies back the pixels that came back changed.
 *
 * It finds the client through OFFLINETAGS_BRIDGE_CLIENT or the path stored in
 * %USERPROFILE%\.offlinetags-bridge\client.txt by affinity\install.ps1.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "filter_api.h"

#define TITLE L"OfflineTags"
#define PATH_CHARS 1024
#define CLIENT_TIMEOUT_MS (5 * 60 * 1000)

/* exit codes of "offlinetags-bridge process" */
enum { CLIENT_DONE = 0, CLIENT_ERROR = 1, CLIENT_NO_PHOTO = 2 };

static void trace(const char *format, ...);

static void notify(const wchar_t *text, UINT icon)
{
    trace("message: %ls", text);
    /* set by the tests, which have nobody to click OK */
    if (GetEnvironmentVariableW(L"OFFLINETAGS_BRIDGE_QUIET", NULL, 0) > 0)
        return;
    MessageBoxW(GetActiveWindow(), text, TITLE, MB_OK | MB_TASKMODAL | icon);
}

/* %USERPROFILE%\.offlinetags-bridge, the same directory the Python side uses.
 * Not %TEMP% or %APPDATA%: packaged apps such as Affinity get a private view of
 * those, which a daemon started elsewhere could not read. */
static BOOL state_dir(wchar_t *out)
{
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", out, PATH_CHARS - 64);
    if (len == 0 || len >= PATH_CHARS - 64)
        return FALSE;
    wcscat_s(out, PATH_CHARS, L"\\.offlinetags-bridge");
    return TRUE;
}

/* Appends a line to %USERPROFILE%\.offlinetags-bridge\plugin.log. A host gives a
 * plug-in no console, so this is the only way to see what it was asked to do. */
static void trace(const char *format, ...)
{
    wchar_t path[PATH_CHARS];
    if (!state_dir(path))
        return;
    CreateDirectoryW(path, NULL);
    wcscat_s(path, PATH_CHARS, L"\\plugin.log");

    WIN32_FILE_ATTRIBUTE_DATA info;
    BOOL restart = GetFileAttributesExW(path, GetFileExInfoStandard, &info) && info.nFileSizeLow > 256 * 1024;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              restart ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;

    char line[600];
    SYSTEMTIME now;
    GetLocalTime(&now);
    int used = sprintf_s(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d ",
                         now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    va_list args;
    va_start(args, format);
    int more = vsnprintf_s(line + used, sizeof line - used - 2, _TRUNCATE, format, args);
    va_end(args);
    used += more < 0 ? (int)strlen(line + used) : more;
    line[used++] = '\n';
    DWORD written;
    WriteFile(file, line, (DWORD)used, &written, NULL);
    CloseHandle(file);
}

static BOOL read_file(const wchar_t *path, uint8_t **data, DWORD *size)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    LARGE_INTEGER length;
    BOOL ok = GetFileSizeEx(file, &length) && length.QuadPart < 0x7fffffff;
    DWORD got = 0;
    *data = ok ? malloc((size_t)length.QuadPart + 1) : NULL;
    ok = *data && ReadFile(file, *data, (DWORD)length.QuadPart, &got, NULL) && got == length.QuadPart;
    CloseHandle(file);
    if (!ok) {
        free(*data);
        *data = NULL;
        return FALSE;
    }
    (*data)[got] = 0;
    *size = got;
    return TRUE;
}

static BOOL client_path(const wchar_t *dir, wchar_t *out)
{
    DWORD len = GetEnvironmentVariableW(L"OFFLINETAGS_BRIDGE_CLIENT", out, PATH_CHARS);
    if (len > 0 && len < PATH_CHARS)
        return TRUE;

    wchar_t file[PATH_CHARS];
    swprintf_s(file, PATH_CHARS, L"%s\\client.txt", dir);
    uint8_t *text;
    DWORD size;
    if (!read_file(file, &text, &size))
        return FALSE;
    const char *start = (const char *)text;
    if (size >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF)
        start += 3; /* UTF-8 byte order mark */
    int chars = MultiByteToWideChar(CP_UTF8, 0, start, -1, out, PATH_CHARS);
    free(text);
    if (chars <= 1)
        return FALSE;
    for (wchar_t *end = out + wcslen(out); end > out && wcschr(L"\r\n\t ", end[-1]); )
        *--end = 0;
    return out[0] != 0;
}

/* Runs the client and returns its exit code, or -1 if it could not be run.
 * The client's console output goes to log_path. */
static int run_client(const wchar_t *client, const wchar_t *input, const wchar_t *output,
                      const wchar_t *log_path)
{
    SECURITY_ATTRIBUTES inherit = { sizeof inherit, NULL, TRUE };
    HANDLE log = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE)
        return -1;

    wchar_t command[4 * PATH_CHARS];
    swprintf_s(command, 4 * PATH_CHARS, L"\"%s\" process \"%s\" -o \"%s\" --json",
               client, input, output);

    STARTUPINFOW startup = { sizeof startup };
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process;
    DWORD code = (DWORD)-1;
    if (CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        if (WaitForSingleObject(process.hProcess, CLIENT_TIMEOUT_MS) != WAIT_OBJECT_0)
            TerminateProcess(process.hProcess, (UINT)-1);
        else
            GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    CloseHandle(log);
    return (int)code;
}

static uint8_t sample_to_8bit(const uint8_t *sample, int bytes)
{
    if (bytes == 1)
        return *sample;
    /* 16-bit samples run from 0 to 32768 in this interface */
    uint32_t v = *(const uint16_t *)sample;
    v = (v * 255 + 16384) >> 15;
    return (uint8_t)(v > 255 ? 255 : v);
}

static void sample_from_8bit(uint8_t *sample, int bytes, uint8_t value)
{
    if (bytes == 1)
        *sample = value;
    else
        *(uint16_t *)sample = (uint16_t)((value * 32768u + 127) / 255);
}

static BOOL write_file(const wchar_t *path, const void *head, DWORD head_size,
                       const void *body, DWORD body_size)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written;
    BOOL ok = WriteFile(file, head, head_size, &written, NULL) && written == head_size
           && WriteFile(file, body, body_size, &written, NULL) && written == body_size;
    CloseHandle(file);
    return ok;
}

/* Shows what the client printed when it failed. */
static void report_failure(const wchar_t *log_path)
{
    wchar_t message[1200] = L"OfflineTags could not process the image.\n\n";
    uint8_t *text;
    DWORD size;
    if (read_file(log_path, &text, &size)) {
        size_t used = wcslen(message);
        MultiByteToWideChar(CP_ACP, 0, (const char *)text, -1, message + used, (int)(1200 - used - 1));
        message[1199] = 0;
        free(text);
    }
    notify(message, MB_ICONERROR);
}

typedef struct { int blur, no_photo, tag, upload; } TagCounts;

static int count_occurrences(const char *text, const char *needle)
{
    int count = 0;
    for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle))
        count++;
    return count;
}

/* Counts the detections in the JSON line the client printed. */
static TagCounts read_tags(const wchar_t *log_path)
{
    TagCounts tags = { 0 };
    uint8_t *text;
    DWORD size;
    if (read_file(log_path, &text, &size)) {
        tags.blur = count_occurrences((const char *)text, "\"tag_type\": \"Blur Me\"");
        tags.no_photo = count_occurrences((const char *)text, "\"tag_type\": \"No Photo\"");
        tags.tag = count_occurrences((const char *)text, "\"tag_type\": \"Tag Me\"");
        tags.upload = count_occurrences((const char *)text, "\"tag_type\": \"Upload Me\"");
        free(text);
    }
    return tags;
}

/* "Detected: 1 x Blur Me, 2 x Tag Me." */
static void describe_tags(const TagCounts *tags, wchar_t *out, size_t chars)
{
    const struct { int count; const wchar_t *name; } rows[] = {
        { tags->no_photo, L"No Photo" }, { tags->blur, L"Blur Me" },
        { tags->tag, L"Tag Me" }, { tags->upload, L"Upload Me" },
    };
    wcscpy_s(out, chars, L"Detected: ");
    BOOL any = FALSE;
    for (int i = 0; i < 4; i++) {
        if (rows[i].count == 0)
            continue;
        size_t used = wcslen(out);
        swprintf_s(out + used, chars - used, L"%s%d x %s", any ? L", " : L"", rows[i].count, rows[i].name);
        any = TRUE;
    }
    wcscat_s(out, chars, any ? L"." : L"no privacy tags.");
}

static int sample_bytes(const FilterRecord *rec)
{
    return rec->depth == 16 || rec->imageMode == plugInModeRGB48 ? 2 : 1;
}

/* The work: called with inData and outData covering filterRect, RGB interleaved. */
static int16_t filter_pixels(FilterRecord *rec)
{
    const int width = rec->inRect.right - rec->inRect.left;
    const int height = rec->inRect.bottom - rec->inRect.top;
    const int bytes = sample_bytes(rec);
    const int pixel = 3 * bytes;
    const uint8_t *in = rec->inData;
    uint8_t *out = rec->outData;
    trace("filter %dx%d, %d bytes per sample, row bytes in %d out %d, inData %p outData %p",
          width, height, bytes, rec->inRowBytes, rec->outRowBytes, (void *)in, (void *)out);
    if (!in || !out || width <= 0 || height <= 0)
        return filterBadParameters;

    /* whatever happens below, the output starts as an exact copy */
    for (int y = 0; y < height; y++)
        memcpy(out + (size_t)y * rec->outRowBytes, in + (size_t)y * rec->inRowBytes, (size_t)width * pixel);

    wchar_t dir[PATH_CHARS], client[PATH_CHARS];
    if (!state_dir(dir) || !client_path(dir, client)) {
        notify(L"The OfflineTags client is not set up.\n\n"
               L"Run affinity\\install.ps1 from the offlinetags-bridge folder.", MB_ICONERROR);
        return userCanceledErr;
    }

    wchar_t tmp[PATH_CHARS], input[PATH_CHARS], output[PATH_CHARS], log_path[PATH_CHARS];
    swprintf_s(tmp, PATH_CHARS, L"%s\\tmp", dir);
    CreateDirectoryW(dir, NULL);
    CreateDirectoryW(tmp, NULL);
    DWORD id = GetCurrentProcessId();
    swprintf_s(input, PATH_CHARS, L"%s\\plugin-%lu-in.bmp", tmp, id);
    swprintf_s(output, PATH_CHARS, L"%s\\plugin-%lu-out.bmp", tmp, id);
    swprintf_s(log_path, PATH_CHARS, L"%s\\plugin-%lu.log", tmp, id);
    DeleteFileW(output);

    /* 24-bit BMP: rows bottom-up, BGR, padded to 4 bytes */
    const DWORD stride = ((DWORD)width * 3 + 3) & ~3u;
    const DWORD image_size = stride * (DWORD)height;
    uint8_t *sent = calloc(image_size, 1);
    if (!sent)
        return userCanceledErr;
    for (int y = 0; y < height; y++) {
        const uint8_t *src = in + (size_t)y * rec->inRowBytes;
        uint8_t *dst = sent + (size_t)(height - 1 - y) * stride;
        for (int x = 0; x < width; x++, src += pixel, dst += 3) {
            dst[2] = sample_to_8bit(src, bytes);
            dst[1] = sample_to_8bit(src + bytes, bytes);
            dst[0] = sample_to_8bit(src + 2 * bytes, bytes);
        }
    }

#pragma pack(push, 1)
    struct { BITMAPFILEHEADER file; BITMAPINFOHEADER info; } head = { 0 };
#pragma pack(pop)
    head.file.bfType = 0x4D42;
    head.file.bfOffBits = sizeof head;
    head.file.bfSize = sizeof head + image_size;
    head.info.biSize = sizeof head.info;
    head.info.biWidth = width;
    head.info.biHeight = height;
    head.info.biPlanes = 1;
    head.info.biBitCount = 24;
    head.info.biSizeImage = image_size;

    int16_t result = userCanceledErr;
    uint8_t *reply = NULL;
    DWORD reply_size = 0;

    if (!write_file(input, &head, sizeof head, sent, image_size)) {
        notify(L"OfflineTags could not write its temporary file.", MB_ICONERROR);
        goto done;
    }

    const int exit_code = run_client(client, input, output, log_path);
    trace("client exit code %d", exit_code);
    if (exit_code != CLIENT_DONE && exit_code != CLIENT_NO_PHOTO) {
        report_failure(log_path);
        goto done;
    }

    const TagCounts tags = read_tags(log_path);
    wchar_t detected[200], message[600];
    describe_tags(&tags, detected, 200);

    if (exit_code == CLIENT_NO_PHOTO) {
        swprintf_s(message, 600, L"%s\n\nA 'No Photo' tag is present - do not share this photo.\n"
                                 L"The image was not changed.", detected);
        notify(message, MB_ICONWARNING);
        goto done;
    }

    if (!read_file(output, &reply, &reply_size)) {
        swprintf_s(message, 600, L"%s\n\nNothing to apply. The image was not changed.", detected);
        notify(message, MB_ICONINFORMATION);
        goto done;
    }

    const BITMAPFILEHEADER *file = (const BITMAPFILEHEADER *)reply;
    const BITMAPINFOHEADER *info = (const BITMAPINFOHEADER *)(reply + sizeof *file);
    if (reply_size < sizeof head || file->bfType != 0x4D42 || info->biBitCount != 24
        || info->biCompression != BI_RGB || info->biWidth != width || abs(info->biHeight) != height
        || (uint64_t)file->bfOffBits + image_size > reply_size) {
        notify(L"OfflineTags returned an image it should not have.", MB_ICONERROR);
        goto done;
    }

    /* write back only the pixels that changed, so 16-bit data keeps its precision elsewhere */
    const uint8_t *got = reply + file->bfOffBits;
    const BOOL top_down = info->biHeight < 0;
    for (int y = 0; y < height; y++) {
        const uint8_t *before = sent + (size_t)(height - 1 - y) * stride;
        const uint8_t *after = got + (size_t)(top_down ? y : height - 1 - y) * stride;
        uint8_t *dst = out + (size_t)y * rec->outRowBytes;
        for (int x = 0; x < width; x++, before += 3, after += 3, dst += pixel) {
            if (before[0] == after[0] && before[1] == after[1] && before[2] == after[2])
                continue;
            sample_from_8bit(dst, bytes, after[2]);
            sample_from_8bit(dst + bytes, bytes, after[1]);
            sample_from_8bit(dst + 2 * bytes, bytes, after[0]);
        }
    }
    result = noErr;

    swprintf_s(message, 600, L"%s\n\nApplied:%s%s", detected,
               tags.blur ? L"\n- pixelated the face of each 'Blur Me' wearer" : L"",
               tags.tag + tags.upload ? L"\n- drew a consent box around each 'Tag Me' / 'Upload Me' wearer" : L"");
    notify(message, MB_ICONINFORMATION);

done:
    free(sent);
    free(reply);
    DeleteFileW(input);
    DeleteFileW(output);
    DeleteFileW(log_path);
    return result;
}

static const Rect16 EMPTY_RECT = { 0, 0, 0, 0 };

typedef int16_t (*AdvanceStateProc)(void);

/* set once the pixels were handled, so a later Continue does not do it again */
static BOOL finished;

__declspec(dllexport) void PluginMain(const int16_t selector, FilterRecord *rec, intptr_t *data, int16_t *result)
{
    (void)data;
    *result = noErr;

    switch (selector) {
    case filterSelectorAbout:
        /* rec is an AboutRecord here, do not touch it */
        notify(L"OfflineTags\n\nFinds privacy badges and applies what each one asks for.\n"
               L"https://github.com/Andx667/offlinetags-bridge", MB_ICONINFORMATION);
        break;

    case filterSelectorStart: {
        trace("start: mode %d, depth %d, planes %d, image %dx%d, filterRect (%d,%d)-(%d,%d), "
              "filterCase %d, advanceState %p",
              rec->imageMode, rec->depth, rec->planes, rec->imageSize.h, rec->imageSize.v,
              rec->filterRect.left, rec->filterRect.top, rec->filterRect.right, rec->filterRect.bottom,
              rec->filterCase, rec->advanceState);
        const BOOL rgb = rec->imageMode == plugInModeRGBColor || rec->imageMode == plugInModeRGB48;
        const BOOL depth_ok = rec->depth == 0 || rec->depth == 8 || rec->depth == 16;
        if (!rgb || !depth_ok || rec->planes < 3) {
            notify(L"OfflineTags works on 8-bit and 16-bit RGB images only.", MB_ICONWARNING);
            *result = userCanceledErr;
            break;
        }
        /* ask for the whole area at once */
        finished = FALSE;
        rec->inRect = rec->outRect = rec->filterRect;
        rec->inLoPlane = rec->outLoPlane = 0;
        rec->inHiPlane = rec->outHiPlane = 2;
        rec->maskRect = EMPTY_RECT;

        /* Modern hosts hand over the buffers right away through advanceState.
         * Without it, the host answers the rectangles above by calling Continue. */
        if (rec->advanceState) {
            int16_t err = ((AdvanceStateProc)rec->advanceState)();
            trace("advanceState returned %d, inData %p, outData %p", err, rec->inData, rec->outData);
            if (err == noErr && rec->inData && rec->outData) {
                *result = filter_pixels(rec);
                finished = TRUE;
                rec->inRect = rec->outRect = rec->maskRect = EMPTY_RECT;
                if (*result == noErr)
                    ((AdvanceStateProc)rec->advanceState)(); /* hands the output back */
            }
        }
        trace("start done: result %d, finished %d", *result, finished);
        break;
    }

    case filterSelectorContinue:
        trace("continue: finished %d, inData %p, outData %p", finished, rec->inData, rec->outData);
        if (!finished) {
            *result = filter_pixels(rec);
            finished = TRUE;
        }
        /* empty rectangles tell the host we are done */
        rec->inRect = rec->outRect = rec->maskRect = EMPTY_RECT;
        trace("continue done: result %d", *result);
        break;

    default: /* Parameters, Prepare, Finish: nothing to do */
        trace("selector %d", selector);
        break;
    }
}
