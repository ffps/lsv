#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <d2d1.h>

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>
#include <new>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")


#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"


// ============================================================
// Registry
// ============================================================

static const wchar_t* REG_PATH =
    L"Software\\LightSpeed";


// ============================================================
// Direct2D
// ============================================================

static ID2D1Factory* g_factory = nullptr;
static ID2D1HwndRenderTarget* g_target = nullptr;
static ID2D1Bitmap* g_bitmap = nullptr;


// ============================================================
// Image
// ============================================================

static UINT g_imageWidth = 0;
static UINT g_imageHeight = 0;

// Original image dimensions, before WIC screen scaling.
static UINT g_sourceWidth = 0;
static UINT g_sourceHeight = 0;

static std::wstring g_currentFile;
static std::vector<std::wstring> g_files;
static size_t g_currentIndex = 0;

// GIF animation state.
static IWICBitmapDecoder* g_gifDecoder = nullptr;
static UINT g_gifFrameCount = 0;
static UINT g_gifFrameIndex = 0;
static UINT g_gifDelayMs = 100;
static bool g_gifPlaying = false;

// Fully composited native-resolution pixels of the currently displayed GIF frame.
// Used by clipboard copy so Copy copies the current frame, not frame 0.
static std::vector<BYTE> g_gifFramePixels;
static constexpr UINT GIF_TIMER_ID = 0x4C53;


// ============================================================
// Window
// ============================================================

static bool g_fullscreen = true;

static RECT g_windowedRect{};
static bool g_haveWindowedRect = false;


// ============================================================
// Zoom
// ============================================================

static void SetAutoScale(HWND hwnd);
static void UpdateWindowTitle(HWND hwnd);

static bool g_autoScale = true;
static float g_zoom = 1.0f;
static bool g_zoomWasAuto = false;

// False on initial open: small images stay 1:1.
// True after explicit Auto Fit (* / menu): small images may be enlarged.
static bool g_autoFitAllowUpscale = false;

// Manual rotation for the current image: 0, 90, 180, 270 degrees clockwise.
static int g_manualRotation = 0;

static float g_zoomBeforeOperation = 1.0f;


// ============================================================
// Pan
// ============================================================

static float g_panX = 0.0f;
static float g_panY = 0.0f;


// ============================================================
// Mouse dragging
// ============================================================

static bool g_dragging = false;

static POINT g_dragStart{};

static float g_dragStartPanX = 0.0f;
static float g_dragStartPanY = 0.0f;

// Shift + mouse selection
static bool g_selecting = false;
static POINT g_selectionStart{};
static POINT g_selectionEnd{};


// ============================================================
// Cleanup
// ============================================================

static void ReleaseBitmap()
{
    if (g_bitmap)
    {
        g_bitmap->Release();
        g_bitmap = nullptr;
    }
}

static void ReleaseGifDecoder()
{
    if (g_gifDecoder)
    {
        g_gifDecoder->Release();
        g_gifDecoder = nullptr;
    }

    g_gifFrameCount = 0;
    g_gifFrameIndex = 0;
    g_gifDelayMs = 100;
    g_gifPlaying = false;
    g_gifFramePixels.clear();
}


static void ReleaseAll()
{
    ReleaseBitmap();

    if (g_target)
    {
        g_target->Release();
        g_target = nullptr;
    }

    if (g_factory)
    {
        g_factory->Release();
        g_factory = nullptr;
    }
}


// ============================================================
// Registry helpers
// ============================================================

static bool ReadDword(
    HKEY key,
    const wchar_t* name,
    DWORD& value)
{
    DWORD type = 0;
    DWORD size = sizeof(value);

    return
        RegQueryValueExW(
            key,
            name,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(&value),
            &size) == ERROR_SUCCESS
        &&
        type == REG_DWORD;
}


static bool ReadString(
    HKEY key,
    const wchar_t* name,
    std::wstring& value)
{
    DWORD type = 0;
    DWORD size = 0;

    if (RegQueryValueExW(
            key,
            name,
            nullptr,
            &type,
            nullptr,
            &size) != ERROR_SUCCESS ||
        type != REG_SZ ||
        size == 0)
    {
        return false;
    }

    std::vector<wchar_t> buffer(
        (size / sizeof(wchar_t)) + 1);

    if (RegQueryValueExW(
            key,
            name,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(buffer.data()),
            &size) != ERROR_SUCCESS ||
        type != REG_SZ)
    {
        return false;
    }

    value.assign(buffer.data());
    return !value.empty();
}


static void SaveString(
    HKEY key,
    const wchar_t* name,
    const std::wstring& value)
{
    RegSetValueExW(
        key,
        name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}


static bool LoadWindowSettings(
    bool& fullscreen,
    RECT& windowRect)
{
    HKEY key = nullptr;

    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            REG_PATH,
            0,
            KEY_READ,
            &key) != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD fullscreenValue = 0;

    if (!ReadDword(
            key,
            L"Fullscreen",
            fullscreenValue))
    {
        RegCloseKey(key);
        return false;
    }

    DWORD left = 0;
    DWORD top = 0;
    DWORD width = 0;
    DWORD height = 0;

    if (!ReadDword(key, L"WindowLeft", left) ||
        !ReadDword(key, L"WindowTop", top) ||
        !ReadDword(key, L"WindowWidth", width) ||
        !ReadDword(key, L"WindowHeight", height))
    {
        RegCloseKey(key);
        return false;
    }

    RegCloseKey(key);

    fullscreen =
        fullscreenValue != 0;

    windowRect.left =
        static_cast<LONG>(left);

    windowRect.top =
        static_cast<LONG>(top);

    windowRect.right =
        windowRect.left +
        static_cast<LONG>(width);

    windowRect.bottom =
        windowRect.top +
        static_cast<LONG>(height);

    if (width < 200 ||
        height < 150)
    {
        return false;
    }

    return true;
}


static void SaveWindowSettings(
    HWND hwnd)
{
    HKEY key = nullptr;

    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            REG_PATH,
            0,
            nullptr,
            0,
            KEY_WRITE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS)
    {
        return;
    }

    DWORD fullscreen =
        g_fullscreen ? 1 : 0;

    RegSetValueExW(
        key,
        L"Fullscreen",
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&fullscreen),
        sizeof(fullscreen));

    RECT rc{};

    if (!g_fullscreen)
    {
        if (GetWindowRect(
                hwnd,
                &rc))
        {
            g_windowedRect = rc;
            g_haveWindowedRect = true;
        }
    }

    if (g_haveWindowedRect)
    {
        DWORD left =
            static_cast<DWORD>(
                g_windowedRect.left);

        DWORD top =
            static_cast<DWORD>(
                g_windowedRect.top);

        DWORD width =
            static_cast<DWORD>(
                g_windowedRect.right -
                g_windowedRect.left);

        DWORD height =
            static_cast<DWORD>(
                g_windowedRect.bottom -
                g_windowedRect.top);

        RegSetValueExW(
            key,
            L"WindowLeft",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&left),
            sizeof(left));

        RegSetValueExW(
            key,
            L"WindowTop",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&top),
            sizeof(top));

        RegSetValueExW(
            key,
            L"WindowWidth",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&width),
            sizeof(width));

        RegSetValueExW(
            key,
            L"WindowHeight",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&height),
            sizeof(height));
    }

    RegCloseKey(key);
}


// ============================================================
// Console / UI output
// ============================================================

static void ConsoleWrite(
    const wchar_t* text,
    bool error)
{
    HANDLE handle =
        GetStdHandle(
            error
                ? STD_ERROR_HANDLE
                : STD_OUTPUT_HANDLE);

    if (handle == nullptr ||
        handle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD written = 0;

    if (WriteConsoleW(
            handle,
            text,
            static_cast<DWORD>(wcslen(text)),
            &written,
            nullptr))
    {
        return;
    }

    int bytes =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);

    if (bytes <= 0)
        return;

    std::vector<char> utf8(
        static_cast<size_t>(bytes));

    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            -1,
            utf8.data(),
            bytes,
            nullptr,
            nullptr) <= 0)
    {
        return;
    }

    WriteFile(
        handle,
        utf8.data(),
        static_cast<DWORD>(bytes - 1),
        &written,
        nullptr);
}

static void ShowInfo(
    HWND hwnd,
    const wchar_t* text,
    const wchar_t* title)
{
    MessageBoxW(
        hwnd,
        text,
        title,
        MB_OK | MB_ICONINFORMATION);
}

static void ShowError(
    HWND hwnd,
    const wchar_t* text,
    const wchar_t* title)
{
    MessageBoxW(
        hwnd,
        text,
        title,
        MB_OK | MB_ICONERROR);
}

static void ShowHelp()
{
    MessageBoxW(
        nullptr,
        L"LightSpeed Image Viewer\n\n"
        L"Usage: LightSpeed.exe <image>\n"
        L"       LightSpeed.exe /?\n"
        L"       LightSpeed.exe --help\n\n"
        L"Keys:\n"
        L"    Esc exit, PageUp/PageDown previous/next, I information,\n"
        L"    Alt+Enter or middle mouse button fullscreen/windowed,\n"
        L"    NumPad / 1:1, + / - zoom, * auto, C copy file, M move file\n",
        L"LightSpeed Image Viewer",
        MB_OK | MB_ICONINFORMATION);
}


// ============================================================
// File helpers
// ============================================================

static bool HasExtension(
    const std::wstring& filename,
    const wchar_t* ext)
{
    const size_t pos =
        filename.find_last_of(L'.');

    if (pos == std::wstring::npos)
        return false;

    return
        _wcsicmp(
            filename.c_str() + pos,
            ext) == 0;
}


static bool IsImageFile(
    const std::wstring& filename)
{
    // File list is intentionally filtered by extension.
    return
        HasExtension(filename, L".jpg") ||
        HasExtension(filename, L".jpeg") ||
        HasExtension(filename, L".png") ||
        HasExtension(filename, L".webp") ||
        HasExtension(filename, L".gif") ||
        HasExtension(filename, L".svg") ||
        HasExtension(filename, L".ico");
}


enum class ImageFormat
{
    Unknown,
    JPEG,
    PNG,
    WebP,
    GIF,
    ICO,
    SVG
};


static ImageFormat DetectImageFormat(
    const std::wstring& filename)
{
    HANDLE file =
        CreateFileW(
            filename.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ |
            FILE_SHARE_WRITE |
            FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return ImageFormat::Unknown;

    BYTE data[4096]{};
    DWORD bytesRead = 0;

    const BOOL ok =
        ReadFile(
            file,
            data,
            sizeof(data),
            &bytesRead,
            nullptr);

    CloseHandle(file);

    if (!ok)
        return ImageFormat::Unknown;

    // JPEG
    if (bytesRead >= 3 &&
        data[0] == 0xFF &&
        data[1] == 0xD8 &&
        data[2] == 0xFF)
    {
        return ImageFormat::JPEG;
    }

    // PNG
    if (bytesRead >= 8 &&
        data[0] == 0x89 &&
        data[1] == 0x50 &&
        data[2] == 0x4E &&
        data[3] == 0x47 &&
        data[4] == 0x0D &&
        data[5] == 0x0A &&
        data[6] == 0x1A &&
        data[7] == 0x0A)
    {
        return ImageFormat::PNG;
    }

    // WebP: RIFF....WEBP
    if (bytesRead >= 12 &&
        data[0] == 'R' &&
        data[1] == 'I' &&
        data[2] == 'F' &&
        data[3] == 'F' &&
        data[8] == 'W' &&
        data[9] == 'E' &&
        data[10] == 'B' &&
        data[11] == 'P')
    {
        return ImageFormat::WebP;
    }

    // GIF87a / GIF89a
    if (bytesRead >= 6 &&
        data[0] == 'G' &&
        data[1] == 'I' &&
        data[2] == 'F' &&
        ((data[3] == '8' && data[4] == '7' && data[5] == 'a') ||
         (data[3] == '8' && data[4] == '9' && data[5] == 'a')))
    {
        return ImageFormat::GIF;
    }

    // ICO
    if (bytesRead >= 4 &&
        data[0] == 0x00 &&
        data[1] == 0x00 &&
        data[2] == 0x01 &&
        data[3] == 0x00)
    {
        return ImageFormat::ICO;
    }

    // SVG: look for <svg in the beginning of the text.
    if (bytesRead > 0)
    {
        std::string text(
            reinterpret_cast<const char*>(data),
            bytesRead);

        size_t pos = 0;

        // UTF-8 BOM
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
        {
            pos = 3;
        }

        for (; pos + 4 <= text.size(); ++pos)
        {
            if (text[pos] != '<')
                continue;

            if ((text[pos + 1] == 's' ||
                 text[pos + 1] == 'S') &&
                (text[pos + 2] == 'v' ||
                 text[pos + 2] == 'V') &&
                (text[pos + 3] == 'g' ||
                 text[pos + 3] == 'G'))
            {
                return ImageFormat::SVG;
            }
        }
    }

    return ImageFormat::Unknown;
}


static bool IsSvgFile(
    const wchar_t* filename)
{
    return
        DetectImageFormat(filename) ==
        ImageFormat::SVG;
}


static const wchar_t* GetFormatName(
    const std::wstring& filename)
{
    switch (DetectImageFormat(filename))
    {
    case ImageFormat::JPEG:
        return L"JPEG";

    case ImageFormat::PNG:
        return L"PNG";

    case ImageFormat::WebP:
        return L"WebP";

    case ImageFormat::GIF:
        return L"GIF";

    case ImageFormat::ICO:
        return L"ICO";

    case ImageFormat::SVG:
        return L"SVG";

    default:
        return L"Unknown";
    }
}


// ============================================================
// File list
// ============================================================

static bool SameFilePath(
    const std::wstring& a,
    const std::wstring& b)
{
    wchar_t fullA[32768]{};
    wchar_t fullB[32768]{};

    const DWORD lenA =
        GetFullPathNameW(
            a.c_str(),
            static_cast<DWORD>(_countof(fullA)),
            fullA,
            nullptr);

    const DWORD lenB =
        GetFullPathNameW(
            b.c_str(),
            static_cast<DWORD>(_countof(fullB)),
            fullB,
            nullptr);

    if (lenA == 0 || lenB == 0 ||
        lenA >= _countof(fullA) ||
        lenB >= _countof(fullB))
    {
        return
            _wcsicmp(
                a.c_str(),
                b.c_str()) == 0;
    }

    return
        _wcsicmp(
            fullA,
            fullB) == 0;
}


static void BuildFileList(
    const std::wstring& filename)
{
    g_files.clear();
    g_currentIndex = 0;

    const size_t slash =
        filename.find_last_of(L"\\/");

    std::wstring directory;

    if (slash == std::wstring::npos)
    {
        directory = L".";
    }
    else
    {
        directory =
            filename.substr(
                0,
                slash);
    }

    std::wstring pattern =
        directory + L"\\*";

    WIN32_FIND_DATAW fd{};

    HANDLE h =
        FindFirstFileW(
            pattern.c_str(),
            &fd);

    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes &
                FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }

            // Directory navigation is still filtered by
            // extension, as requested.
            if (!IsImageFile(fd.cFileName))
                continue;

            std::wstring fullPath =
                directory +
                L"\\" +
                fd.cFileName;

            g_files.push_back(
                fullPath);
        }
        while (FindNextFileW(h, &fd));

        FindClose(h);
    }

    // The explicitly supplied command-line file must always
    // be opened, even when it has no image extension.
    // Its actual format is determined from its contents.
    bool found = false;

    for (const std::wstring& file : g_files)
    {
        if (SameFilePath(file, filename))
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        g_files.push_back(filename);
    }

    std::sort(
        g_files.begin(),
        g_files.end(),
        [](const std::wstring& a,
           const std::wstring& b)
        {
            return
                _wcsicmp(
                    a.c_str(),
                    b.c_str()) < 0;
        });

    for (size_t i = 0;
         i < g_files.size();
         ++i)
    {
        if (SameFilePath(
                g_files[i],
                filename))
        {
            g_currentIndex = i;
            break;
        }
    }
}


// ============================================================
// WIC image loader
// ============================================================

static BYTE ReadExifOrientationFromFile(const wchar_t* filename)
{
    if (!filename)
        return 1;

    HANDLE file = CreateFileW(
        filename,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return 1;

    LARGE_INTEGER size{};
    BYTE orientation = 1;

    if (!GetFileSizeEx(file, &size) || size.QuadPart < 4)
    {
        CloseHandle(file);
        return orientation;
    }

    // EXIF is normally at the beginning of a JPEG. Keep the parser
    // strictly bounded so malformed metadata can never make us scan
    // an unbounded amount of data.
    constexpr DWORD MAX_READ = 4u * 1024u * 1024u;
    const DWORD bytesToRead =
        static_cast<DWORD>(
            (size.QuadPart < MAX_READ)
                ? size.QuadPart
                : MAX_READ);

    std::vector<BYTE> data(bytesToRead);
    DWORD bytesRead = 0;

    if (!ReadFile(file, data.data(), bytesToRead, &bytesRead, nullptr) ||
        bytesRead < 4)
    {
        CloseHandle(file);
        return orientation;
    }

    auto u16 = [&](size_t p, bool le) -> UINT16
    {
        if (le)
            return static_cast<UINT16>(data[p] | (data[p + 1] << 8));
        return static_cast<UINT16>((data[p] << 8) | data[p + 1]);
    };

    auto u32 = [&](size_t p, bool le) -> UINT32
    {
        if (le)
        {
            return static_cast<UINT32>(data[p]) |
                   (static_cast<UINT32>(data[p + 1]) << 8) |
                   (static_cast<UINT32>(data[p + 2]) << 16) |
                   (static_cast<UINT32>(data[p + 3]) << 24);
        }

        return (static_cast<UINT32>(data[p]) << 24) |
               (static_cast<UINT32>(data[p + 1]) << 16) |
               (static_cast<UINT32>(data[p + 2]) << 8) |
               static_cast<UINT32>(data[p + 3]);
    };

    const size_t n = bytesRead;

    // JPEG SOI.
    if (data[0] != 0xFF || data[1] != 0xD8)
    {
        CloseHandle(file);
        return orientation;
    }

    size_t p = 2;

    while (p + 4 <= n)
    {
        // Find the next JPEG marker.
        if (data[p] != 0xFF)
        {
            ++p;
            continue;
        }

        while (p < n && data[p] == 0xFF)
            ++p;

        if (p >= n)
            break;

        const BYTE marker = data[p++];

        // Start of scan: metadata after this point is not relevant.
        if (marker == 0xDA || marker == 0xD9)
            break;

        // Standalone markers have no length.
        if (marker >= 0xD0 && marker <= 0xD7)
            continue;

        if (p + 2 > n)
            break;

        const UINT16 segmentLength = u16(p, false);

        if (segmentLength < 2)
            break;

        const size_t segmentEnd =
            p + static_cast<size_t>(segmentLength);

        if (segmentEnd > n)
            break;

        if (marker == 0xE1 && segmentLength >= 8)
        {
            const size_t exif = p + 2;

            if (exif + 6 <= segmentEnd &&
                std::memcmp(data.data() + exif, "Exif\0\0", 6) == 0)
            {
                const size_t tiff = exif + 6;

                if (tiff + 8 <= segmentEnd &&
                    ((data[tiff] == 'I' && data[tiff + 1] == 'I') ||
                     (data[tiff] == 'M' && data[tiff + 1] == 'M')))
                {
                    const bool le = data[tiff] == 'I';

                    if (u16(tiff + 2, le) == 42)
                    {
                        const UINT32 ifdOffset = u32(tiff + 4, le);
                        const size_t ifd = tiff + ifdOffset;

                        if (ifd + 2 <= segmentEnd)
                        {
                            const UINT16 count = u16(ifd, le);
                            const size_t entries = ifd + 2;

                            for (UINT16 i = 0; i < count; ++i)
                            {
                                const size_t entry =
                                    entries + static_cast<size_t>(i) * 12;

                                if (entry + 12 > segmentEnd)
                                    break;

                                // Orientation tag 0x0112.
                                if (u16(entry, le) != 0x0112)
                                    continue;

                                const UINT16 type = u16(entry + 2, le);
                                const UINT32 valueCount = u32(entry + 4, le);

                                if (type != 3 || valueCount != 1)
                                    break;

                                const UINT16 value =
                                    u16(entry + 8, le);

                                if (value >= 1 && value <= 8)
                                    orientation = static_cast<BYTE>(value);

                                CloseHandle(file);
                                return orientation;
                            }
                        }
                    }
                }
            }
        }

        p = segmentEnd;
    }

    CloseHandle(file);
    return orientation;
}


static BYTE GetExifOrientation(const wchar_t* filename)
{
    return ReadExifOrientationFromFile(filename);
}


static void TransformPixelsExif(
    std::vector<BYTE>& pixels,
    UINT& width,
    UINT& height,
    BYTE orientation)
{
    if (orientation < 2 || orientation > 8 ||
        width == 0 || height == 0)
    {
        return;
    }

    const UINT newWidth =
        (orientation >= 5 && orientation <= 8)
            ? height
            : width;

    const UINT newHeight =
        (orientation >= 5 && orientation <= 8)
            ? width
            : height;

    const size_t oldStride = static_cast<size_t>(width) * 4;
    const size_t newStride = static_cast<size_t>(newWidth) * 4;

    std::vector<BYTE> transformed(
        newStride * newHeight);

    for (UINT y = 0; y < height; ++y)
    {
        for (UINT x = 0; x < width; ++x)
        {
            UINT nx = x;
            UINT ny = y;

            switch (orientation)
            {
            case 2:
                nx = width - 1 - x;
                break;

            case 3:
                nx = width - 1 - x;
                ny = height - 1 - y;
                break;

            case 4:
                ny = height - 1 - y;
                break;

            case 5: // transpose
                nx = y;
                ny = x;
                break;

            case 6: // 90 degrees clockwise
                nx = height - 1 - y;
                ny = x;
                break;

            case 7: // transverse
                nx = height - 1 - y;
                ny = width - 1 - x;
                break;

            case 8: // 90 degrees counter-clockwise
                nx = y;
                ny = width - 1 - x;
                break;
            }

            std::memcpy(
                transformed.data() +
                    static_cast<size_t>(ny) * newStride +
                    static_cast<size_t>(nx) * 4,
                pixels.data() +
                    static_cast<size_t>(y) * oldStride +
                    static_cast<size_t>(x) * 4,
                4);
        }
    }

    pixels.swap(transformed);
    width = newWidth;
    height = newHeight;
}


static void RotatePixelsBGRA(
    std::vector<BYTE>& pixels,
    UINT& width,
    UINT& height,
    int rotation)
{
    rotation &= 3;

    if (rotation == 0 ||
        width == 0 ||
        height == 0)
    {
        return;
    }

    const UINT newWidth =
        (rotation & 1) ? height : width;

    const UINT newHeight =
        (rotation & 1) ? width : height;

    const size_t oldStride =
        static_cast<size_t>(width) * 4;

    const size_t newStride =
        static_cast<size_t>(newWidth) * 4;

    std::vector<BYTE> rotated(
        newStride * newHeight);

    for (UINT y = 0; y < height; ++y)
    {
        for (UINT x = 0; x < width; ++x)
        {
            UINT nx = 0;
            UINT ny = 0;

            if (rotation == 1)
            {
                nx = height - 1 - y;
                ny = x;
            }
            else if (rotation == 2)
            {
                nx = width - 1 - x;
                ny = height - 1 - y;
            }
            else
            {
                nx = y;
                ny = width - 1 - x;
            }

            std::memcpy(
                rotated.data() +
                    static_cast<size_t>(ny) * newStride +
                    static_cast<size_t>(nx) * 4,
                pixels.data() +
                    static_cast<size_t>(y) * oldStride +
                    static_cast<size_t>(x) * 4,
                4);
        }
    }

    pixels.swap(rotated);
    width = newWidth;
    height = newHeight;
}


static HRESULT LoadImageWIC(
    ID2D1RenderTarget* target,
    const wchar_t* filename,
    UINT maxWidth,
    UINT maxHeight,
    bool fitToScreen)
{
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;

    // Declare before any goto cleanup so MSVC does not jump over
    // the construction of the vector.
    std::vector<BYTE> pixels;

    HRESULT hr =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic));

    if (FAILED(hr))
        return hr;

    hr =
        wic->CreateDecoderFromFilename(
            filename,
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            &decoder);

    if (FAILED(hr))
        goto cleanup;

    hr = decoder->GetFrame(0, &frame);

    if (FAILED(hr))
        goto cleanup;

    UINT srcWidth = 0;
    UINT srcHeight = 0;

    hr = frame->GetSize(&srcWidth, &srcHeight);

    if (FAILED(hr) || srcWidth == 0 || srcHeight == 0)
    {
        hr = FAILED(hr) ? hr : E_FAIL;
        goto cleanup;
    }

    g_sourceWidth = srcWidth;
    g_sourceHeight = srcHeight;

    UINT dstWidth = srcWidth;
    UINT dstHeight = srcHeight;

    if (fitToScreen &&
        (g_autoFitAllowUpscale ||
         srcWidth > maxWidth ||
         srcHeight > maxHeight))
    {
        const double sx =
            static_cast<double>(maxWidth) /
            static_cast<double>(srcWidth);

        const double sy =
            static_cast<double>(maxHeight) /
            static_cast<double>(srcHeight);

        const double scale = (sx < sy) ? sx : sy;

        dstWidth = static_cast<UINT>(srcWidth * scale + 0.5);
        dstHeight = static_cast<UINT>(srcHeight * scale + 0.5);

        if (dstWidth < 1) dstWidth = 1;
        if (dstHeight < 1) dstHeight = 1;
    }

    IWICBitmapSource* source = frame;

    if (dstWidth != srcWidth || dstHeight != srcHeight)
    {
        hr = wic->CreateBitmapScaler(&scaler);

        if (SUCCEEDED(hr))
        {
            hr = scaler->Initialize(
                source,
                dstWidth,
                dstHeight,
                WICBitmapInterpolationModeFant);
        }

        if (FAILED(hr))
            goto cleanup;

        source = scaler;
    }

    hr = wic->CreateFormatConverter(&converter);

    if (SUCCEEDED(hr))
    {
        hr = converter->Initialize(
            source,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
    }

    if (FAILED(hr))
        goto cleanup;

    UINT width = 0;
    UINT height = 0;

    hr = converter->GetSize(&width, &height);

    if (FAILED(hr) || width == 0 || height == 0)
    {
        hr = FAILED(hr) ? hr : E_FAIL;
        goto cleanup;
    }

    const UINT stride = width * 4;
    const size_t bufferSize = static_cast<size_t>(stride) * height;

    pixels.resize(bufferSize);

    hr = converter->CopyPixels(
        nullptr,
        stride,
        static_cast<UINT>(bufferSize),
        pixels.data());

    if (FAILED(hr))
        goto cleanup;

    // Read EXIF without WIC's metadata provider. This avoids hangs on
    // problematic camera metadata and keeps the parser strictly bounded.
    const BYTE exifOrientation = GetExifOrientation(filename);

    TransformPixelsExif(
        pixels,
        width,
        height,
        exifOrientation);

    // Manual rotation is applied after EXIF orientation.
    RotatePixelsBGRA(
        pixels,
        width,
        height,
        g_manualRotation);

    g_imageWidth = width;
    g_imageHeight = height;

    hr = target->CreateBitmap(
        D2D1::SizeU(width, height),
        pixels.data(),
        width * 4,
        D2D1::BitmapProperties(
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED)),
        &g_bitmap);

cleanup:
    if (converter)
        converter->Release();

    if (scaler)
        scaler->Release();

    if (frame)
        frame->Release();

    if (decoder)
        decoder->Release();

    if (wic)
        wic->Release();

    return hr;
}


// ============================================================
// SVG loader
// ============================================================

static HRESULT LoadSVG(
    ID2D1RenderTarget* target,
    const wchar_t* filename,
    UINT maxWidth,
    UINT maxHeight,
    bool fitToScreen)
{
    int needed =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            filename,
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);

    if (needed <= 0)
        return E_FAIL;

    char* utf8 =
        new (std::nothrow) char[needed];

    if (!utf8)
        return E_OUTOFMEMORY;

    WideCharToMultiByte(
        CP_UTF8,
        0,
        filename,
        -1,
        utf8,
        needed,
        nullptr,
        nullptr);

    NSVGimage* image =
        nsvgParseFromFile(
            utf8,
            "px",
            96.0f);

    delete[] utf8;

    if (!image)
        return E_FAIL;

    if (image->width <= 0.0f ||
        image->height <= 0.0f)
    {
        nsvgDelete(image);
        return E_FAIL;
    }

    g_sourceWidth =
        static_cast<UINT>(
            image->width + 0.5f);

    g_sourceHeight =
        static_cast<UINT>(
            image->height + 0.5f);

    g_imageWidth = g_sourceWidth;
    g_imageHeight = g_sourceHeight;

    double scale = 1.0;

    if (fitToScreen)
    {
        const double sx =
            static_cast<double>(maxWidth) /
            static_cast<double>(image->width);

        const double sy =
            static_cast<double>(maxHeight) /
            static_cast<double>(image->height);

        scale =
            (sx < sy) ? sx : sy;

        if (!g_autoFitAllowUpscale && scale > 1.0)
            scale = 1.0;
    }

    int width =
        static_cast<int>(
            image->width * scale + 0.5);

    int height =
        static_cast<int>(
            image->height * scale + 0.5);

    if (width < 1)
        width = 1;

    if (height < 1)
        height = 1;

    if (width > 16384 ||
        height > 16384)
    {
        nsvgDelete(image);
        return E_OUTOFMEMORY;
    }

    const size_t pixelCount =
        static_cast<size_t>(width) *
        static_cast<size_t>(height);

    if (pixelCount >
        (static_cast<size_t>(512) *
         1024 *
         1024) / 4)
    {
        nsvgDelete(image);
        return E_OUTOFMEMORY;
    }

    unsigned char* rgba =
        new (std::nothrow)
        unsigned char[pixelCount * 4];

    if (!rgba)
    {
        nsvgDelete(image);
        return E_OUTOFMEMORY;
    }

    std::memset(
        rgba,
        0,
        pixelCount * 4);

    NSVGrasterizer* rasterizer =
        nsvgCreateRasterizer();

    if (!rasterizer)
    {
        delete[] rgba;
        nsvgDelete(image);
        return E_FAIL;
    }

    nsvgRasterize(
        rasterizer,
        image,
        0.0f,
        0.0f,
        static_cast<float>(scale),
        rgba,
        width,
        height,
        width * 4);

    nsvgDeleteRasterizer(
        rasterizer);

    nsvgDelete(image);

    // RGBA -> premultiplied BGRA.
    for (size_t i = 0;
         i < pixelCount;
         ++i)
    {
        unsigned char* p =
            rgba + i * 4;

        const unsigned int r = p[0];
        const unsigned int g = p[1];
        const unsigned int b = p[2];
        const unsigned int a = p[3];

        p[0] =
            static_cast<unsigned char>(
                (b * a + 127) / 255);

        p[1] =
            static_cast<unsigned char>(
                (g * a + 127) / 255);

        p[2] =
            static_cast<unsigned char>(
                (r * a + 127) / 255);
    }

    D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f);

    HRESULT hr =
        target->CreateBitmap(
            D2D1::SizeU(
                static_cast<UINT32>(width),
                static_cast<UINT32>(height)),
            rgba,
            static_cast<UINT32>(
                width * 4),
            &props,
            &g_bitmap);

    delete[] rgba;

    return hr;
}


// ============================================================
// GIF animation
// ============================================================

static UINT GetGifMetadataUInt(
    IWICBitmapFrameDecode* frame,
    const wchar_t* name,
    UINT defaultValue)
{
    if (!frame)
        return defaultValue;

    IWICMetadataQueryReader* reader = nullptr;
    PROPVARIANT value;
    PropVariantInit(&value);

    UINT result = defaultValue;

    HRESULT hr = frame->GetMetadataQueryReader(&reader);

    if (SUCCEEDED(hr) && reader)
        hr = reader->GetMetadataByName(name, &value);

    if (SUCCEEDED(hr))
    {
        if (value.vt == VT_UI1)
            result = value.bVal;
        else if (value.vt == VT_UI2)
            result = value.uiVal;
        else if (value.vt == VT_UI4)
            result = value.ulVal;
    }

    PropVariantClear(&value);

    if (reader)
        reader->Release();

    return result;
}


static UINT GetGifFrameDelay(IWICBitmapFrameDecode* frame)
{
    // GIF Graphic Control Extension delay is stored in 1/100 second.
    UINT hundredths =
        GetGifMetadataUInt(
            frame,
            L"/grctlext/Delay",
            10);

    if (hundredths == 0)
        hundredths = 10;

    UINT delay = hundredths * 10;

    if (delay < 20)
        delay = 20;

    if (delay > 60000)
        delay = 60000;

    return delay;
}


static UINT GetGifDisposal(
    IWICBitmapFrameDecode* frame)
{
    return GetGifMetadataUInt(
        frame,
        L"/grctlext/Disposal",
        0);
}


static void StopGifPlayback(HWND hwnd)
{
    if (hwnd)
        KillTimer(hwnd, GIF_TIMER_ID);

    g_gifPlaying = false;
}


static void BlendGifPixels(
    std::vector<BYTE>& canvas,
    UINT canvasWidth,
    UINT canvasHeight,
    const std::vector<BYTE>& framePixels,
    UINT frameWidth,
    UINT frameHeight,
    UINT left,
    UINT top)
{
    if (canvas.empty() || framePixels.empty())
        return;

    const UINT copyWidth =
        (left < canvasWidth)
            ? std::min(frameWidth, canvasWidth - left)
            : 0;

    const UINT copyHeight =
        (top < canvasHeight)
            ? std::min(frameHeight, canvasHeight - top)
            : 0;

    for (UINT y = 0; y < copyHeight; ++y)
    {
        for (UINT x = 0; x < copyWidth; ++x)
        {
            const size_t si =
                (static_cast<size_t>(y) * frameWidth + x) * 4;

            const size_t di =
                (static_cast<size_t>(top + y) * canvasWidth +
                 (left + x)) * 4;

            const BYTE sa = framePixels[si + 3];

            if (sa == 255)
            {
                canvas[di + 0] = framePixels[si + 0];
                canvas[di + 1] = framePixels[si + 1];
                canvas[di + 2] = framePixels[si + 2];
                canvas[di + 3] = 255;
            }
            else if (sa != 0)
            {
                const BYTE da = canvas[di + 3];
                const UINT inv = 255 - sa;

                canvas[di + 0] = static_cast<BYTE>(
                    framePixels[si + 0] +
                    (static_cast<UINT>(canvas[di + 0]) * inv + 127) / 255);

                canvas[di + 1] = static_cast<BYTE>(
                    framePixels[si + 1] +
                    (static_cast<UINT>(canvas[di + 1]) * inv + 127) / 255);

                canvas[di + 2] = static_cast<BYTE>(
                    framePixels[si + 2] +
                    (static_cast<UINT>(canvas[di + 2]) * inv + 127) / 255);

                canvas[di + 3] = static_cast<BYTE>(
                    sa + (static_cast<UINT>(da) * inv + 127) / 255);
            }
        }
    }
}


static void ClearGifRect(
    std::vector<BYTE>& canvas,
    UINT canvasWidth,
    UINT canvasHeight,
    UINT left,
    UINT top,
    UINT width,
    UINT height)
{
    if (left >= canvasWidth || top >= canvasHeight)
        return;

    width = std::min(width, canvasWidth - left);
    height = std::min(height, canvasHeight - top);

    for (UINT y = 0; y < height; ++y)
    {
        BYTE* row =
            canvas.data() +
            (static_cast<size_t>(top + y) * canvasWidth + left) * 4;

        memset(row, 0, static_cast<size_t>(width) * 4);
    }
}


static HRESULT LoadGifFrame(
    HWND hwnd,
    UINT frameIndex)
{
    if (!g_gifDecoder ||
        frameIndex >= g_gifFrameCount)
    {
        return E_FAIL;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICImagingFactory* wic = nullptr;
    IWICBitmap* canvasBitmap = nullptr;
    IWICBitmapScaler* canvasScaler = nullptr;
    std::vector<BYTE> canvas;
    std::vector<BYTE> framePixels;
    std::vector<BYTE> restoreCanvas;

    UINT previousDisposal = 0;
    UINT previousLeft = 0;
    UINT previousTop = 0;
    UINT previousWidth = 0;
    UINT previousHeight = 0;

    RECT rc{};

    auto cleanupAndReturn = [&](HRESULT code) -> HRESULT
    {
        if (canvasScaler)
            canvasScaler->Release();

        if (scaler)
            scaler->Release();

        if (frame)
            frame->Release();

        if (wic)
            wic->Release();

        return code;
    };

    HRESULT hr =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic));

    if (FAILED(hr))
        return hr;

    const UINT canvasWidth = g_sourceWidth;
    const UINT canvasHeight = g_sourceHeight;

    if (canvasWidth == 0 || canvasHeight == 0)
    {
        hr = E_FAIL;
        return cleanupAndReturn(hr);
    }

    canvas.resize(
        static_cast<size_t>(canvasWidth) * canvasHeight * 4,
        0);

    // Rebuild the composed canvas from frame 0 through the requested frame.
    // This is intentional: GIF frames may contain only a small changed
    // rectangle and may use disposal methods 2/3. Displaying a raw frame
    // directly causes the old black/transparent background artifacts.
    for (UINT i = 0; i <= frameIndex; ++i)
    {
        if (i > 0)
        {
            if (previousDisposal == 2)
            {
                ClearGifRect(
                    canvas,
                    canvasWidth,
                    canvasHeight,
                    previousLeft,
                    previousTop,
                    previousWidth,
                    previousHeight);
            }
            else if (previousDisposal == 3 &&
                     restoreCanvas.size() == canvas.size())
            {
                canvas = restoreCanvas;
            }
        }

        if (frame)
        {
            frame->Release();
            frame = nullptr;
        }

        hr = g_gifDecoder->GetFrame(i, &frame);

        if (FAILED(hr))
            return cleanupAndReturn(hr);

        UINT frameWidth = 0;
        UINT frameHeight = 0;

        hr = frame->GetSize(&frameWidth, &frameHeight);

        if (FAILED(hr) || frameWidth == 0 || frameHeight == 0)
        {
            hr = FAILED(hr) ? hr : E_FAIL;
            return cleanupAndReturn(hr);
        }

        const UINT left =
            GetGifMetadataUInt(frame, L"/imgdesc/Left", 0);
        const UINT top =
            GetGifMetadataUInt(frame, L"/imgdesc/Top", 0);
        const UINT disposal =
            GetGifDisposal(frame);

        // Save the canvas before this frame for disposal method 3.
        if (disposal == 3)
            restoreCanvas = canvas;
        else
            restoreCanvas.clear();

        if (frameWidth != canvasWidth || frameHeight != canvasHeight)
        {
            hr = wic->CreateBitmapScaler(&scaler);

            if (SUCCEEDED(hr))
            {
                hr = scaler->Initialize(
                    frame,
                    frameWidth,
                    frameHeight,
                    WICBitmapInterpolationModeFant);
            }

            if (FAILED(hr))
                return cleanupAndReturn(hr);
        }

        IWICBitmapSource* source =
            scaler ? static_cast<IWICBitmapSource*>(scaler) :
                     static_cast<IWICBitmapSource*>(frame);

        IWICFormatConverter* converter = nullptr;

        hr = wic->CreateFormatConverter(&converter);

        if (SUCCEEDED(hr))
        {
            hr = converter->Initialize(
                source,
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
        }

        if (SUCCEEDED(hr))
        {
            UINT convertedWidth = 0;
            UINT convertedHeight = 0;

            hr = converter->GetSize(
                &convertedWidth,
                &convertedHeight);

            if (SUCCEEDED(hr))
            {
                const UINT stride = convertedWidth * 4;
                const size_t size =
                    static_cast<size_t>(stride) * convertedHeight;

                framePixels.resize(size);

                hr = converter->CopyPixels(
                    nullptr,
                    stride,
                    static_cast<UINT>(size),
                    framePixels.data());

                if (SUCCEEDED(hr))
                {
                    BlendGifPixels(
                        canvas,
                        canvasWidth,
                        canvasHeight,
                        framePixels,
                        convertedWidth,
                        convertedHeight,
                        left,
                        top);
                }
            }
        }

        if (converter)
            converter->Release();

        if (scaler)
        {
            scaler->Release();
            scaler = nullptr;
        }

        if (FAILED(hr))
            return cleanupAndReturn(hr);

        previousDisposal = disposal;
        previousLeft = left;
        previousTop = top;
        previousWidth = frameWidth;
        previousHeight = frameHeight;
    }

    GetClientRect(hwnd, &rc);

    UINT maxWidth = static_cast<UINT>(rc.right - rc.left);
    UINT maxHeight = static_cast<UINT>(rc.bottom - rc.top);

    if (maxWidth == 0) maxWidth = 1;
    if (maxHeight == 0) maxHeight = 1;

    UINT dstWidth = canvasWidth;
    UINT dstHeight = canvasHeight;

    if (g_autoScale &&
        (g_autoFitAllowUpscale ||
         canvasWidth > maxWidth ||
         canvasHeight > maxHeight))
    {
        const double sx =
            static_cast<double>(maxWidth) / canvasWidth;
        const double sy =
            static_cast<double>(maxHeight) / canvasHeight;
        const double scale = (sx < sy) ? sx : sy;

        dstWidth = static_cast<UINT>(canvasWidth * scale + 0.5);
        dstHeight = static_cast<UINT>(canvasHeight * scale + 0.5);

        if (dstWidth < 1) dstWidth = 1;
        if (dstHeight < 1) dstHeight = 1;
    }
    else if (!g_autoScale)
    {
        dstWidth = static_cast<UINT>(canvasWidth * g_zoom + 0.5f);
        dstHeight = static_cast<UINT>(canvasHeight * g_zoom + 0.5f);

        if (dstWidth < 1) dstWidth = 1;
        if (dstHeight < 1) dstHeight = 1;
    }

    // Keep the fully composited native-resolution frame for clipboard copy.
    // The displayed bitmap may be scaled, but Copy should use the original frame size.
    g_gifFramePixels = canvas;

    {
        IWICBitmap* sourceBitmap = nullptr;

        hr = wic->CreateBitmapFromMemory(
            canvasWidth,
            canvasHeight,
            GUID_WICPixelFormat32bppPBGRA,
            canvasWidth * 4,
            static_cast<UINT>(canvas.size()),
            canvas.data(),
            &sourceBitmap);

        if (FAILED(hr))
            return cleanupAndReturn(hr);

        IWICBitmapSource* displaySource = sourceBitmap;

        if (dstWidth != canvasWidth || dstHeight != canvasHeight)
        {
            hr = wic->CreateBitmapScaler(&canvasScaler);

            if (SUCCEEDED(hr))
            {
                hr = canvasScaler->Initialize(
                    sourceBitmap,
                    dstWidth,
                    dstHeight,
                    WICBitmapInterpolationModeFant);
            }

            if (FAILED(hr))
            {
                sourceBitmap->Release();
                return cleanupAndReturn(hr);
            }

            displaySource = canvasScaler;
        }

        const UINT stride = dstWidth * 4;
        const size_t size = static_cast<size_t>(stride) * dstHeight;
        framePixels.resize(size);

        hr = displaySource->CopyPixels(
            nullptr,
            stride,
            static_cast<UINT>(size),
            framePixels.data());

        if (SUCCEEDED(hr))
        {
            ReleaseBitmap();

            hr = g_target->CreateBitmap(
                D2D1::SizeU(dstWidth, dstHeight),
                framePixels.data(),
                stride,
                D2D1::BitmapProperties(
                    D2D1::PixelFormat(
                        DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED)),
                &g_bitmap);
        }

        if (canvasScaler)
        {
            canvasScaler->Release();
            canvasScaler = nullptr;
        }

        sourceBitmap->Release();
    }

    if (SUCCEEDED(hr))
    {
        g_imageWidth = dstWidth;
        g_imageHeight = dstHeight;
        g_gifFrameIndex = frameIndex;

        if (frame)
            g_gifDelayMs = GetGifFrameDelay(frame);
    }

    return cleanupAndReturn(hr);
}


static HRESULT LoadGif(
    HWND hwnd,
    const wchar_t* filename)
{
    ReleaseGifDecoder();

    IWICImagingFactory* wic = nullptr;

    HRESULT hr =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic));

    if (FAILED(hr))
        return hr;

    hr =
        wic->CreateDecoderFromFilename(
            filename,
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            &g_gifDecoder);

    if (FAILED(hr))
    {
        wic->Release();
        return hr;
    }

    hr = g_gifDecoder->GetFrameCount(&g_gifFrameCount);

    if (FAILED(hr) || g_gifFrameCount == 0)
    {
        if (SUCCEEDED(hr))
            hr = E_FAIL;

        wic->Release();
        ReleaseGifDecoder();
        return hr;
    }

    IWICBitmapFrameDecode* frame = nullptr;

    hr = g_gifDecoder->GetFrame(0, &frame);

    if (SUCCEEDED(hr))
        hr = frame->GetSize(&g_sourceWidth, &g_sourceHeight);

    if (frame)
        frame->Release();

    wic->Release();

    if (FAILED(hr) ||
        g_sourceWidth == 0 ||
        g_sourceHeight == 0)
    {
        ReleaseGifDecoder();
        return FAILED(hr) ? hr : E_FAIL;
    }

    hr = LoadGifFrame(hwnd, 0);

    if (FAILED(hr))
    {
        ReleaseGifDecoder();
        return hr;
    }

    if (g_gifFrameCount > 1)
    {
        g_gifPlaying = true;
        SetTimer(
            hwnd,
            GIF_TIMER_ID,
            g_gifDelayMs,
            nullptr);
    }

    return S_OK;
}


static bool IsCurrentGif()
{
    return
        g_gifDecoder != nullptr &&
        g_gifFrameCount > 0;
}


static void StepGifFrameBackward(
    HWND hwnd)
{
    if (!IsCurrentGif())
        return;

    StopGifPlayback(hwnd);

    if (g_gifFrameIndex == 0)
        return;

    const UINT previous = g_gifFrameIndex - 1;

    if (SUCCEEDED(LoadGifFrame(hwnd, previous)))
    {
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindowTitle(hwnd);
    }
}


static void StepGifFrameForward(
    HWND hwnd)
{
    if (!IsCurrentGif())
        return;

    StopGifPlayback(hwnd);

    if (g_gifFrameIndex + 1 >= g_gifFrameCount)
        return;

    const UINT next = g_gifFrameIndex + 1;

    if (SUCCEEDED(LoadGifFrame(hwnd, next)))
    {
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindowTitle(hwnd);
    }
}


// ============================================================
// Load current image
// ============================================================

static HRESULT LoadCurrentImage(
    HWND hwnd)
{
    if (g_currentIndex >= g_files.size())
        return E_FAIL;

    StopGifPlayback(hwnd);
    ReleaseGifDecoder();
    ReleaseBitmap();

    g_currentFile =
        g_files[g_currentIndex];

    RECT rc;

    GetClientRect(
        hwnd,
        &rc);

    UINT width =
        static_cast<UINT>(
            rc.right - rc.left);

    UINT height =
        static_cast<UINT>(
            rc.bottom - rc.top);

    if (width == 0)
        width = 1;

    if (height == 0)
        height = 1;

    if (IsSvgFile(
            g_currentFile.c_str()))
    {
        return LoadSVG(
            g_target,
            g_currentFile.c_str(),
            width,
            height,
            g_autoScale);
    }

    if (DetectImageFormat(g_currentFile) == ImageFormat::GIF)
    {
        return LoadGif(
            hwnd,
            g_currentFile.c_str());
    }

    return LoadImageWIC(
        g_target,
        g_currentFile.c_str(),
        width,
        height,
        g_autoScale);
}


// ============================================================
// Window title
// ============================================================

// Forward declaration: GetAutoScale is defined below.
static float GetAutoScale(HWND hwnd);

static void UpdateWindowTitle(HWND hwnd)
{
    if (g_fullscreen || g_currentFile.empty())
    {
        SetWindowTextW(hwnd, L"LightSpeed");
        return;
    }

    const size_t slash =
        g_currentFile.find_last_of(L"\\/");

    const wchar_t* name =
        (slash == std::wstring::npos)
            ? g_currentFile.c_str()
            : g_currentFile.c_str() + slash + 1;

    wchar_t title[1024];

    const float scale =
        g_autoScale
            ? GetAutoScale(hwnd)
            : g_zoom;

    if (DetectImageFormat(g_currentFile) == ImageFormat::GIF)
    {
        swprintf_s(
            title,
            L"%s %ux%u GIF %u/%u frames %.0f%%",
            name,
            g_sourceWidth,
            g_sourceHeight,
            g_gifFrameIndex + 1,
            g_gifFrameCount,
            scale * 100.0f);
    }
    else
    {
        swprintf_s(
            title,
            L"%s %ux%u %s 32-bit %.0f%%",
            name,
            g_sourceWidth,
            g_sourceHeight,
            GetFormatName(g_currentFile),
            scale * 100.0f);
    }

    SetWindowTextW(hwnd, title);
}


// ============================================================
// File information
// ============================================================

static void ShowFileInfo(
    HWND hwnd)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};

    if (!GetFileAttributesExW(
            g_currentFile.c_str(),
            GetFileExInfoStandard,
            &data))
    {
        ShowError(
            hwnd,
            L"Could not get file information.",
            L"Image information");

        return;
    }

    ULARGE_INTEGER size{};

    size.LowPart =
        data.nFileSizeLow;

    size.HighPart =
        data.nFileSizeHigh;

    const double kb =
        static_cast<double>(
            size.QuadPart) / 1024.0;

    const double mb =
        kb / 1024.0;

    wchar_t text[2048];

    if (mb >= 1.0)
    {
        swprintf_s(
            text,
            L"File:\n%s\n\n"
            L"Format: %s\n"
            L"Image: %u × %u\n"
            L"File size: %.2f MB\n\n"
            L"%zu / %zu",
            g_currentFile.c_str(),
            GetFormatName(g_currentFile),
            g_imageWidth,
            g_imageHeight,
            mb,
            g_currentIndex + 1,
            g_files.size());
    }
    else
    {
        swprintf_s(
            text,
            L"File:\n%s\n\n"
            L"Format: %s\n"
            L"Image: %u × %u\n"
            L"File size: %.1f KB\n\n"
            L"%zu / %zu",
            g_currentFile.c_str(),
            GetFormatName(g_currentFile),
            g_imageWidth,
            g_imageHeight,
            kb,
            g_currentIndex + 1,
            g_files.size());
    }

    ShowInfo(
        hwnd,
        text,
        L"Image information");
}


// ============================================================
// Navigation
// ============================================================

static void Navigate(
    HWND hwnd,
    int direction)
{
    if (g_files.empty())
        return;

    const size_t count =
        g_files.size();

    // Normally direction is +/-1 (PageUp/PageDown), but Home/End
    // use a larger delta to jump directly to the first/last file.
    const long long target =
        static_cast<long long>(g_currentIndex) +
        static_cast<long long>(direction);

    if (target < 0 ||
        target >= static_cast<long long>(count))
    {
        return;
    }

    g_currentIndex =
        static_cast<size_t>(target);

    g_panX = 0.0f;
    g_panY = 0.0f;
    g_manualRotation = 0;

    HRESULT hr =
        LoadCurrentImage(hwnd);

    if (SUCCEEDED(hr))
    {
        UpdateWindowTitle(hwnd);

        InvalidateRect(
            hwnd,
            nullptr,
            FALSE);
    }
}


// ============================================================
// Direct2D target
// ============================================================

static HRESULT CreateRenderTarget(
    HWND hwnd)
{
    if (g_target)
        return S_OK;

    RECT rc;

    GetClientRect(
        hwnd,
        &rc);

    UINT width =
        static_cast<UINT>(
            rc.right - rc.left);

    UINT height =
        static_cast<UINT>(
            rc.bottom - rc.top);

    if (width == 0)
        width = 1;

    if (height == 0)
        height = 1;

    return
        g_factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_IGNORE),
                96.0f,
                96.0f,
                D2D1_RENDER_TARGET_USAGE_NONE,
                D2D1_FEATURE_LEVEL_DEFAULT),

            D2D1::HwndRenderTargetProperties(
                hwnd,
                D2D1::SizeU(
                    width,
                    height),
                D2D1_PRESENT_OPTIONS_NONE),

            &g_target);
}


// ============================================================
// Auto scale
// ============================================================

static float GetAutoScale(
    HWND hwnd)
{
    RECT rc;

    GetClientRect(
        hwnd,
        &rc);

    const float windowWidth =
        static_cast<float>(
            rc.right - rc.left);

    const float windowHeight =
        static_cast<float>(
            rc.bottom - rc.top);

    if (windowWidth <= 0 ||
        windowHeight <= 0 ||
        g_imageWidth == 0 ||
        g_imageHeight == 0)
    {
        return 1.0f;
    }

    const float sx =
        windowWidth /
        static_cast<float>(g_imageWidth);

    const float sy =
        windowHeight /
        static_cast<float>(g_imageHeight);

    float scale =
        (sx < sy) ? sx : sy;

    // On initial open small images stay 1:1.
    // After explicit Auto Fit, allow them to be enlarged.
    if (!g_autoFitAllowUpscale && scale > 1.0f)
        scale = 1.0f;

    return scale;
}


// ============================================================
// Pan limits
// ============================================================

static void ClampPan(
    HWND hwnd)
{
    if (g_autoScale)
    {
        g_panX = 0.0f;
        g_panY = 0.0f;
        return;
    }

    RECT rc;

    GetClientRect(
        hwnd,
        &rc);

    const float windowWidth =
        static_cast<float>(
            rc.right - rc.left);

    const float windowHeight =
        static_cast<float>(
            rc.bottom - rc.top);

    const float width =
        static_cast<float>(g_imageWidth) *
        g_zoom;

    const float height =
        static_cast<float>(g_imageHeight) *
        g_zoom;

    const float centerX =
        (windowWidth - width) * 0.5f;

    const float centerY =
        (windowHeight - height) * 0.5f;

    // Keep at least 1 pixel of the image visible.
    if (width > windowWidth)
    {
        const float minLeft =
            -width + 1.0f;

        const float maxLeft =
            windowWidth - 1.0f;

        const float minPan =
            minLeft - centerX;

        const float maxPan =
            maxLeft - centerX;

        if (g_panX < minPan)
            g_panX = minPan;

        if (g_panX > maxPan)
            g_panX = maxPan;
    }
    else
    {
        g_panX = 0.0f;
    }

    if (height > windowHeight)
    {
        const float minTop =
            -height + 1.0f;

        const float maxTop =
            windowHeight - 1.0f;

        const float minPan =
            minTop - centerY;

        const float maxPan =
            maxTop - centerY;

        if (g_panY < minPan)
            g_panY = minPan;

        if (g_panY > maxPan)
            g_panY = maxPan;
    }
    else
    {
        g_panY = 0.0f;
    }
}


// ============================================================
// Draw
// ============================================================

static void DrawImage(
    HWND hwnd)
{
    if (!g_target || !g_bitmap)
        return;

    RECT rc;

    GetClientRect(
        hwnd,
        &rc);

    const float windowWidth =
        static_cast<float>(
            rc.right - rc.left);

    const float windowHeight =
        static_cast<float>(
            rc.bottom - rc.top);

    const float imageWidth =
        static_cast<float>(
            g_imageWidth);

    const float imageHeight =
        static_cast<float>(
            g_imageHeight);

    if (windowWidth <= 0 ||
        windowHeight <= 0 ||
        imageWidth <= 0 ||
        imageHeight <= 0)
    {
        return;
    }

    const float scale =
        g_autoScale
            ? GetAutoScale(hwnd)
            : g_zoom;

    const float width =
        imageWidth * scale;

    const float height =
        imageHeight * scale;

    float left =
        (windowWidth - width) * 0.5f +
        g_panX;

    float top =
        (windowHeight - height) * 0.5f +
        g_panY;

    if (width <= windowWidth)
    {
        left =
            (windowWidth - width) * 0.5f;
    }

    if (height <= windowHeight)
    {
        top =
            (windowHeight - height) * 0.5f;
    }

    g_target->BeginDraw();

    g_target->Clear(
        D2D1::ColorF(
            D2D1::ColorF::Black));

    // Use the 4-argument overload because
    // this is supported by the installed SDK.
    g_target->DrawBitmap(
        g_bitmap,
        D2D1::RectF(
            left,
            top,
            left + width,
            top + height),
        1.0f);

    if (g_selecting)
    {
        const float sx = static_cast<float>(
            std::min(g_selectionStart.x, g_selectionEnd.x));
        const float sy = static_cast<float>(
            std::min(g_selectionStart.y, g_selectionEnd.y));
        const float ex = static_cast<float>(
            std::max(g_selectionStart.x, g_selectionEnd.x));
        const float ey = static_cast<float>(
            std::max(g_selectionStart.y, g_selectionEnd.y));

        ID2D1SolidColorBrush* brush = nullptr;

        if (SUCCEEDED(g_target->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::White),
                &brush)))
        {
            g_target->DrawRectangle(
                D2D1::RectF(sx, sy, ex, ey),
                brush,
                1.0f);
            brush->Release();
        }
    }

    const HRESULT hr =
        g_target->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET)
    {
        g_target->Release();
        g_target = nullptr;
    }
}


// ============================================================
// Zoom
// ============================================================

static void SetZoom(
    HWND hwnd,
    float newZoom)
{
    if (newZoom <= 0.0f)
    {
        SetAutoScale(hwnd);
        return;
    }

    if (newZoom < 0.05f)
        newZoom = 0.05f;

    if (newZoom > 20.0f)
        newZoom = 20.0f;

    POINT cursor;

    GetCursorPos(&cursor);
    ScreenToClient(hwnd, &cursor);

    RECT rc;
    GetClientRect(hwnd, &rc);

    const float windowWidth =
        static_cast<float>(rc.right - rc.left);
    const float windowHeight =
        static_cast<float>(rc.bottom - rc.top);

    const bool oldAuto = g_autoScale;
    const float oldScale =
        oldAuto ? GetAutoScale(hwnd) : g_zoom;

    if (oldScale <= 0.0f)
        return;

    const float oldLeft =
        (windowWidth - static_cast<float>(g_imageWidth) * oldScale) * 0.5f + g_panX;
    const float oldTop =
        (windowHeight - static_cast<float>(g_imageHeight) * oldScale) * 0.5f + g_panY;

    const float imageX =
        (static_cast<float>(cursor.x) - oldLeft) / oldScale;
    const float imageY =
        (static_cast<float>(cursor.y) - oldTop) / oldScale;

    g_autoScale = false;
    g_zoom = newZoom;

    const float newLeft =
        static_cast<float>(cursor.x) - imageX * g_zoom;
    const float newTop =
        static_cast<float>(cursor.y) - imageY * g_zoom;

    g_panX =
        newLeft -
        (windowWidth - static_cast<float>(g_imageWidth) * g_zoom) * 0.5f;
    g_panY =
        newTop -
        (windowHeight - static_cast<float>(g_imageHeight) * g_zoom) * 0.5f;

    ClampPan(hwnd);

    if (SUCCEEDED(LoadCurrentImage(hwnd)))
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    UpdateWindowTitle(hwnd);
}

static void ZoomStep(
    HWND hwnd,
    int direction)
{
    constexpr float ZOOM_STEP = 1.25f;

    // Work from the scale currently visible on screen.
    // Auto Fit is treated as its actual current scale, so
    // + and - are true inverses and repeated presses behave
    // monotonically.
    const float currentScale =
        g_autoScale
            ? GetAutoScale(hwnd)
            : g_zoom;

    float newScale =
        direction > 0
            ? currentScale * ZOOM_STEP
            : currentScale / ZOOM_STEP;

    if (newScale < 0.01f)
        newScale = 0.01f;

    if (newScale > 100.0f)
        newScale = 100.0f;

    g_autoScale = false;
    g_zoom = newScale;
    g_zoomWasAuto = false;

    ClampPan(hwnd);

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);

    UpdateWindowTitle(hwnd);
}


// ============================================================
// 1:1
// ============================================================

static void SetOneToOne(
    HWND hwnd)
{
    g_autoScale = false;
    g_zoom = 1.0f;
    g_zoomWasAuto = false;
    g_autoFitAllowUpscale = false;

    g_panX = 0.0f;
    g_panY = 0.0f;

    if (SUCCEEDED(
            LoadCurrentImage(hwnd)))
    {
        InvalidateRect(
            hwnd,
            nullptr,
            FALSE);
    }

    UpdateWindowTitle(hwnd);
}


// ============================================================
// Auto Fit
// ============================================================

static void SetAutoScale(
    HWND hwnd)
{
    g_autoScale = true;
    g_zoom = 1.0f;
    g_zoomWasAuto = false;
    g_autoFitAllowUpscale = true;

    g_panX = 0.0f;
    g_panY = 0.0f;

    if (SUCCEEDED(
            LoadCurrentImage(hwnd)))
    {
        InvalidateRect(
            hwnd,
            nullptr,
            FALSE);
    }

    UpdateWindowTitle(hwnd);
}


// ============================================================
// Fullscreen
// ============================================================

static void ToggleFullscreen(
    HWND hwnd)
{
    if (g_fullscreen)
    {
        // ----------------------------------------------------
        // Fullscreen -> Windowed
        // ----------------------------------------------------

        if (!g_haveWindowedRect)
        {
            RECT rc{};

            if (GetWindowRect(
                    hwnd,
                    &rc))
            {
                g_windowedRect = rc;
                g_haveWindowedRect = true;
            }
        }

        g_fullscreen = false;

        SetWindowLongPtrW(
            hwnd,
            GWL_STYLE,
            WS_OVERLAPPEDWINDOW);

        SetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE,
            WS_EX_APPWINDOW);

        int left;
        int top;
        int width;
        int height;

        if (g_haveWindowedRect)
        {
            left =
                g_windowedRect.left;

            top =
                g_windowedRect.top;

            width =
                g_windowedRect.right -
                g_windowedRect.left;

            height =
                g_windowedRect.bottom -
                g_windowedRect.top;
        }
        else
        {
            HMONITOR monitor =
                MonitorFromWindow(
                    hwnd,
                    MONITOR_DEFAULTTONEAREST);

            MONITORINFO mi{};

            mi.cbSize =
                sizeof(mi);

            GetMonitorInfoW(
                monitor,
                &mi);

            const RECT& r =
                mi.rcWork;

            width =
                (r.right - r.left) * 3 / 4;

            height =
                (r.bottom - r.top) * 3 / 4;

            left =
                r.left +
                ((r.right - r.left) -
                 width) / 2;

            top =
                r.top +
                ((r.bottom - r.top) -
                 height) / 2;
        }

        SetWindowPos(
            hwnd,
            HWND_TOP,
            left,
            top,
            width,
            height,
            SWP_FRAMECHANGED |
            SWP_SHOWWINDOW);

        ShowWindow(
            hwnd,
            SW_SHOWNORMAL);

        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
    else
    {
        // ----------------------------------------------------
        // Windowed -> Fullscreen
        // ----------------------------------------------------

        RECT rc{};

        if (GetWindowRect(
                hwnd,
                &rc))
        {
            g_windowedRect = rc;
            g_haveWindowedRect = true;
        }

        g_fullscreen = true;

        HMONITOR monitor =
            MonitorFromWindow(
                hwnd,
                MONITOR_DEFAULTTONEAREST);

        MONITORINFO mi{};

        mi.cbSize =
            sizeof(mi);

        GetMonitorInfoW(
            monitor,
            &mi);

        SetWindowLongPtrW(
            hwnd,
            GWL_STYLE,
            WS_POPUP);

        SetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE,
            WS_EX_APPWINDOW);

        SetWindowPos(
            hwnd,
            HWND_TOP,
            mi.rcMonitor.left,
            mi.rcMonitor.top,
            mi.rcMonitor.right -
                mi.rcMonitor.left,
            mi.rcMonitor.bottom -
                mi.rcMonitor.top,
            SWP_FRAMECHANGED |
            SWP_SHOWWINDOW);

        ShowWindow(
            hwnd,
            SW_SHOW);

        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }

    SaveWindowSettings(hwnd);
    UpdateWindowTitle(hwnd);

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}


// ============================================================
// About dialog
// ============================================================

static LRESULT CALLBACK AboutWndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HINSTANCE hInstance =
            GetModuleHandleW(nullptr);

        HICON hIcon =
            static_cast<HICON>(LoadImageW(
                hInstance,
                MAKEINTRESOURCEW(1),
                IMAGE_ICON,
                32,
                32,
                LR_DEFAULTCOLOR));

        if (hIcon)
        {
            SendMessageW(
                hwnd,
                WM_SETICON,
                ICON_BIG,
                reinterpret_cast<LPARAM>(hIcon));

            SendMessageW(
                hwnd,
                WM_SETICON,
                ICON_SMALL,
                reinterpret_cast<LPARAM>(hIcon));

            HWND icon = CreateWindowExW(
                0,
                L"STATIC",
                nullptr,
                WS_CHILD | WS_VISIBLE | SS_ICON,
                18,
                18,
                32,
                32,
                hwnd,
                nullptr,
                hInstance,
                nullptr);

            if (icon)
            {
                SendMessageW(
                    icon,
                    STM_SETIMAGE,
                    IMAGE_ICON,
                    reinterpret_cast<LPARAM>(hIcon));
            }
        }

        CreateWindowExW(
            0,
            L"STATIC",
            L"LightSpeed Image Viewer",
            WS_CHILD | WS_VISIBLE,
            64,
            18,
            260,
            24,
            hwnd,
            nullptr,
            hInstance,
            nullptr);

        CreateWindowExW(
            0,
            L"STATIC",
            L"Version 0.22\n\nFree software",
            WS_CHILD | WS_VISIBLE,
            64,
            48,
            260,
            54,
            hwnd,
            nullptr,
            hInstance,
            nullptr);

        CreateWindowExW(
            0,
            L"STATIC",
            L"Written by ChatGPT",
            WS_CHILD | WS_VISIBLE,
            64,
            106,
            260,
            22,
            hwnd,
            nullptr,
            hInstance,
            nullptr);

        CreateWindowExW(
            0,
            L"BUTTON",
            L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            240,
            160,
            80,
            26,
            hwnd,
            reinterpret_cast<HMENU>(1),
            hInstance,
            nullptr);

        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));

        return reinterpret_cast<LRESULT>(
            GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == 1)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
    {
        HWND owner =
            GetWindow(hwnd, GW_OWNER);

        if (owner)
            EnableWindow(owner, TRUE);

        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowAbout(HWND owner)
{
    HINSTANCE hInstance =
        GetModuleHandleW(nullptr);

    static bool registered = false;

    if (!registered)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AboutWndProc;
        wc.hInstance = hInstance;
        wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"LightSpeedAboutWindow";

        if (!RegisterClassExW(&wc))
            return;

        registered = true;
    }

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);

    const int width = 360;
    const int height = 240;
    const int left =
        ownerRect.left +
        ((ownerRect.right - ownerRect.left) - width) / 2;
    const int top =
        ownerRect.top +
        ((ownerRect.bottom - ownerRect.top) - height) / 2;

    EnableWindow(owner, FALSE);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
        L"LightSpeedAboutWindow",
        L"About LightSpeed",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        left,
        top,
        width,
        height,
        owner,
        nullptr,
        hInstance,
        nullptr);

    if (!hwnd)
    {
        EnableWindow(owner, TRUE);
        return;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (IsWindow(owner))
        SetForegroundWindow(owner);
}


// ============================================================
// Clipboard / selection
// ============================================================

static void GetDisplayedImageRect(
    HWND hwnd,
    RECT& imageRect,
    float& scale)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);

    const float windowWidth =
        static_cast<float>(rc.right - rc.left);
    const float windowHeight =
        static_cast<float>(rc.bottom - rc.top);

    scale =
        g_autoScale
            ? GetAutoScale(hwnd)
            : g_zoom;

    const float width =
        static_cast<float>(g_imageWidth) * scale;
    const float height =
        static_cast<float>(g_imageHeight) * scale;

    float left =
        (windowWidth - width) * 0.5f + g_panX;
    float top =
        (windowHeight - height) * 0.5f + g_panY;

    if (width <= windowWidth)
        left = (windowWidth - width) * 0.5f;

    if (height <= windowHeight)
        top = (windowHeight - height) * 0.5f;

    imageRect.left = static_cast<LONG>(std::floor(left));
    imageRect.top = static_cast<LONG>(std::floor(top));
    imageRect.right = static_cast<LONG>(std::ceil(left + width));
    imageRect.bottom = static_cast<LONG>(std::ceil(top + height));
}


static bool GetSelectionImageRect(
    HWND hwnd,
    RECT& imageRect)
{
    RECT displayed{};
    float scale = 1.0f;
    GetDisplayedImageRect(hwnd, displayed, scale);

    RECT selection{
        std::min(g_selectionStart.x, g_selectionEnd.x),
        std::min(g_selectionStart.y, g_selectionEnd.y),
        std::max(g_selectionStart.x, g_selectionEnd.x),
        std::max(g_selectionStart.y, g_selectionEnd.y)
    };

    RECT clipped{};

    if (!IntersectRect(&clipped, &selection, &displayed))
        return false;

    const LONG left = static_cast<LONG>(
        std::floor((clipped.left - displayed.left) / scale));
    const LONG top = static_cast<LONG>(
        std::floor((clipped.top - displayed.top) / scale));
    const LONG right = static_cast<LONG>(
        std::ceil((clipped.right - displayed.left) / scale));
    const LONG bottom = static_cast<LONG>(
        std::ceil((clipped.bottom - displayed.top) / scale));

    imageRect.left = std::max<LONG>(0,
        std::min<LONG>(static_cast<LONG>(g_imageWidth), left));
    imageRect.top = std::max<LONG>(0,
        std::min<LONG>(static_cast<LONG>(g_imageHeight), top));
    imageRect.right = std::max<LONG>(imageRect.left,
        std::min<LONG>(static_cast<LONG>(g_imageWidth), right));
    imageRect.bottom = std::max<LONG>(imageRect.top,
        std::min<LONG>(static_cast<LONG>(g_imageHeight), bottom));

    return imageRect.right > imageRect.left &&
           imageRect.bottom > imageRect.top;
}


static bool LoadNativeClipboardPixels(
    std::vector<BYTE>& pixels,
    UINT& width,
    UINT& height)
{
    width = 0;
    height = 0;

    if (g_currentFile.empty())
        return false;

    // GIF: copy exactly the frame currently shown, at native resolution.
    // LoadGifFrame keeps the fully composited current frame in g_gifFramePixels.
    if (IsCurrentGif() && !g_gifFramePixels.empty())
    {
        width = g_sourceWidth;
        height = g_sourceHeight;
        pixels = g_gifFramePixels;
        return width != 0 && height != 0;
    }

    if (IsSvgFile(g_currentFile.c_str()))
    {
        int needed = WideCharToMultiByte(
            CP_UTF8, 0, g_currentFile.c_str(), -1,
            nullptr, 0, nullptr, nullptr);

        if (needed <= 0)
            return false;

        std::vector<char> utf8(static_cast<size_t>(needed));

        if (!WideCharToMultiByte(
                CP_UTF8, 0, g_currentFile.c_str(), -1,
                utf8.data(), needed, nullptr, nullptr))
            return false;

        NSVGimage* image = nsvgParseFromFile(
            utf8.data(), "px", 96.0f);

        if (!image || image->width <= 0.0f || image->height <= 0.0f)
        {
            if (image)
                nsvgDelete(image);
            return false;
        }

        width = static_cast<UINT>(image->width + 0.5f);
        height = static_cast<UINT>(image->height + 0.5f);

        const size_t pixelCount =
            static_cast<size_t>(width) * height;

        if (width == 0 || height == 0 ||
            pixelCount > (static_cast<size_t>(512) * 1024 * 1024) / 4)
        {
            nsvgDelete(image);
            return false;
        }

        pixels.resize(pixelCount * 4);
        std::memset(pixels.data(), 0, pixels.size());

        NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
        if (!rasterizer)
        {
            nsvgDelete(image);
            pixels.clear();
            return false;
        }

        nsvgRasterize(
            rasterizer,
            image,
            0.0f,
            0.0f,
            1.0f,
            pixels.data(),
            static_cast<int>(width),
            static_cast<int>(height),
            static_cast<int>(width * 4));

        nsvgDeleteRasterizer(rasterizer);
        nsvgDelete(image);

        // RGBA -> premultiplied BGRA.
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            const unsigned int r = pixels[i + 0];
            const unsigned int g = pixels[i + 1];
            const unsigned int b = pixels[i + 2];
            const unsigned int a = pixels[i + 3];

            pixels[i + 0] = static_cast<BYTE>((b * a + 127) / 255);
            pixels[i + 1] = static_cast<BYTE>((g * a + 127) / 255);
            pixels[i + 2] = static_cast<BYTE>((r * a + 127) / 255);
        }

        return true;
    }

    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic));

    if (FAILED(hr))
        return false;

    hr = wic->CreateDecoderFromFilename(
        g_currentFile.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);

    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, &frame);

    IWICBitmapSource* source = frame;

    if (SUCCEEDED(hr))
        hr = wic->CreateFormatConverter(&converter);

    if (SUCCEEDED(hr))
    {
        hr = converter->Initialize(
            source,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
    }

    if (SUCCEEDED(hr))
        hr = converter->GetSize(&width, &height);

    if (SUCCEEDED(hr))
    {
        const size_t stride = static_cast<size_t>(width) * 4;
        const size_t size = stride * height;

        if (size > static_cast<size_t>(512) * 1024 * 1024)
            hr = E_OUTOFMEMORY;
        else
        {
            pixels.resize(size);
            hr = converter->CopyPixels(
                nullptr,
                static_cast<UINT>(stride),
                static_cast<UINT>(size),
                pixels.data());
        }
    }

    if (SUCCEEDED(hr))
    {
        const BYTE exifOrientation =
            GetExifOrientation(g_currentFile.c_str());

        TransformPixelsExif(
            pixels,
            width,
            height,
            exifOrientation);

        RotatePixelsBGRA(
            pixels,
            width,
            height,
            g_manualRotation);
    }

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (wic) wic->Release();

    if (FAILED(hr))
    {
        pixels.clear();
        width = 0;
        height = 0;
        return false;
    }

    return true;
}


static HGLOBAL EncodePngClipboardData(
    const BYTE* pixels,
    UINT width,
    UINT height,
    UINT stride)
{
    if (!pixels || width == 0 || height == 0)
        return nullptr;

    HGLOBAL memory =
        GlobalAlloc(
            GMEM_MOVEABLE,
            0);

    if (!memory)
        return nullptr;

    IStream* stream = nullptr;

    HRESULT hr =
        CreateStreamOnHGlobal(
            memory,
            FALSE,
            &stream);

    if (FAILED(hr) || !stream)
    {
        GlobalFree(memory);
        return nullptr;
    }

    IWICImagingFactory* wic = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;

    bool ok = false;

    hr =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic));

    if (SUCCEEDED(hr))
    {
        hr =
            wic->CreateEncoder(
                GUID_ContainerFormatPng,
                nullptr,
                &encoder);
    }

    if (SUCCEEDED(hr))
    {
        hr =
            encoder->Initialize(
                stream,
                WICBitmapEncoderNoCache);
    }

    if (SUCCEEDED(hr))
    {
        hr =
            encoder->CreateNewFrame(
                &frame,
                nullptr);
    }

    if (SUCCEEDED(hr))
        hr = frame->Initialize(nullptr);

    if (SUCCEEDED(hr))
    {
        hr =
            frame->SetSize(
                width,
                height);
    }

    if (SUCCEEDED(hr))
    {
        WICPixelFormatGUID format =
            GUID_WICPixelFormat32bppBGRA;

        hr =
            frame->SetPixelFormat(
                &format);

        if (SUCCEEDED(hr) &&
            format != GUID_WICPixelFormat32bppBGRA)
        {
            hr = E_FAIL;
        }
    }

    if (SUCCEEDED(hr))
    {
        hr =
            frame->WritePixels(
                height,
                stride,
                stride * height,
                const_cast<BYTE*>(pixels));
    }

    if (SUCCEEDED(hr))
        hr = frame->Commit();

    if (SUCCEEDED(hr))
        hr = encoder->Commit();

    if (SUCCEEDED(hr))
    {
        hr =
            stream->Commit(
                STGC_DEFAULT);
    }

    if (SUCCEEDED(hr))
        ok = true;

    if (frame)
        frame->Release();

    if (encoder)
        encoder->Release();

    if (wic)
        wic->Release();

    stream->Release();

    if (!ok)
    {
        GlobalFree(memory);
        return nullptr;
    }

    return memory;
}

static bool CopyPixelsToClipboard(
    HWND hwnd,
    const std::vector<BYTE>& pixels,
    UINT width,
    UINT height,
    const RECT* crop)
{
    if (width == 0 || height == 0 || pixels.empty())
        return false;

    RECT src{
        0,
        0,
        static_cast<LONG>(width),
        static_cast<LONG>(height)};

    if (crop && !IntersectRect(&src, &src, crop))
        return false;

    const UINT outWidth = static_cast<UINT>(src.right - src.left);
    const UINT outHeight = static_cast<UINT>(src.bottom - src.top);
    const size_t srcStride = static_cast<size_t>(width) * 4;
    const size_t outStride = static_cast<size_t>(outWidth) * 4;
    const size_t pixelSize = outStride * outHeight;

    // Registered PNG clipboard format. Some older image utilities
    // do not read CF_DIB/CF_BITMAP but do read the standard PNG
    // clipboard format by name.
    // PNG encoder needs a contiguous crop buffer. The source image
    // keeps its original stride, so passing a cropped pointer with the
    // crop stride would make WritePixels read across rows incorrectly.
    std::vector<BYTE> pngPixels(pixelSize);

    for (UINT y = 0; y < outHeight; ++y)
    {
        const BYTE* srcRow =
            pixels.data() +
            static_cast<size_t>(src.top + static_cast<LONG>(y)) * srcStride +
            static_cast<size_t>(src.left) * 4;

        std::memcpy(
            pngPixels.data() + static_cast<size_t>(y) * outStride,
            srcRow,
            outStride);
    }

    HGLOBAL pngMemory =
        EncodePngClipboardData(
            pngPixels.data(),
            outWidth,
            outHeight,
            static_cast<UINT>(outStride));

    // CF_DIB: the format already used by LightSpeed.
    // Use a classic 24-bit bottom-up CF_DIB. Older clipboard readers
    // (including old image utilities) often do not understand a
    // top-down 32-bit DIB even though modern applications do.
    const size_t dibStride =
        ((static_cast<size_t>(outWidth) * 3 + 3) & ~static_cast<size_t>(3));
    const size_t dibPixelSize = dibStride * outHeight;
    const size_t dibSize = sizeof(BITMAPINFOHEADER) + dibPixelSize;
    HGLOBAL dibMemory = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (!dibMemory)
    {
        if (pngMemory)
            GlobalFree(pngMemory);
        return false;
    }

    BYTE* dibData = static_cast<BYTE*>(GlobalLock(dibMemory));
    if (!dibData)
    {
        if (pngMemory)
            GlobalFree(pngMemory);
        GlobalFree(dibMemory);
        return false;
    }

    BITMAPINFOHEADER* bih = reinterpret_cast<BITMAPINFOHEADER*>(dibData);
    ZeroMemory(bih, sizeof(*bih));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = static_cast<LONG>(outWidth);
    bih->biHeight = static_cast<LONG>(outHeight);
    bih->biPlanes = 1;
    bih->biBitCount = 24;
    bih->biCompression = BI_RGB;

    BYTE* dst = dibData + sizeof(BITMAPINFOHEADER);

    for (UINT y = 0; y < outHeight; ++y)
    {
        const BYTE* srcRow =
            pixels.data() +
            static_cast<size_t>(src.top + static_cast<LONG>(y)) * srcStride +
            static_cast<size_t>(src.left) * 4;

        BYTE* dstRow =
            dst + static_cast<size_t>(outHeight - 1 - y) * dibStride;

        for (UINT x = 0; x < outWidth; ++x)
        {
            // Source is premultiplied BGRA. For the legacy 24-bit DIB
            // alpha is discarded; un-premultiply RGB first.
            const BYTE b = srcRow[x * 4 + 0];
            const BYTE g = srcRow[x * 4 + 1];
            const BYTE r = srcRow[x * 4 + 2];
            const BYTE a = srcRow[x * 4 + 3];

            BYTE rr = r;
            BYTE gg = g;
            BYTE bb = b;

            if (a != 0 && a != 255)
            {
                rr = static_cast<BYTE>(std::min(255u, (static_cast<unsigned>(r) * 255u + a / 2u) / a));
                gg = static_cast<BYTE>(std::min(255u, (static_cast<unsigned>(g) * 255u + a / 2u) / a));
                bb = static_cast<BYTE>(std::min(255u, (static_cast<unsigned>(b) * 255u + a / 2u) / a));
            }

            dstRow[x * 3 + 0] = bb;
            dstRow[x * 3 + 1] = gg;
            dstRow[x * 3 + 2] = rr;
        }
    }

    GlobalUnlock(dibMemory);

    // CF_BITMAP: some older programs, including older image
    // utilities, prefer a real HBITMAP over CF_DIB.
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(outWidth);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(outHeight);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bitmapBits = nullptr;

    HBITMAP bitmap =
        CreateDIBSection(
            nullptr,
            &bitmapInfo,
            DIB_RGB_COLORS,
            &bitmapBits,
            nullptr,
            0);

    if (!bitmap)
    {
        if (pngMemory)
            GlobalFree(pngMemory);
        GlobalFree(dibMemory);
        return false;
    }

    for (UINT y = 0; y < outHeight; ++y)
    {
        const BYTE* srcRow =
            pixels.data() +
            static_cast<size_t>(src.top + static_cast<LONG>(y)) * srcStride +
            static_cast<size_t>(src.left) * 4;

        std::memcpy(
            static_cast<BYTE*>(bitmapBits) + static_cast<size_t>(y) * outStride,
            srcRow,
            outStride);
    }

    // CF_DIBV5: Windows screenshot tools commonly expose a
    // V5 DIB as well. This gives consumers another standard
    // clipboard representation without changing the image data.
    const size_t dibV5Size = sizeof(BITMAPV5HEADER) + pixelSize;
    HGLOBAL dibV5Memory = GlobalAlloc(GMEM_MOVEABLE, dibV5Size);

    if (dibV5Memory)
    {
        BYTE* v5Data = static_cast<BYTE*>(GlobalLock(dibV5Memory));

        if (v5Data)
        {
            BITMAPV5HEADER* v5 =
                reinterpret_cast<BITMAPV5HEADER*>(v5Data);

            ZeroMemory(v5, sizeof(*v5));
            v5->bV5Size = sizeof(BITMAPV5HEADER);
            v5->bV5Width = static_cast<LONG>(outWidth);
            v5->bV5Height = -static_cast<LONG>(outHeight);
            v5->bV5Planes = 1;
            v5->bV5BitCount = 32;
            v5->bV5Compression = BI_BITFIELDS;
            v5->bV5RedMask = 0x00FF0000;
            v5->bV5GreenMask = 0x0000FF00;
            v5->bV5BlueMask = 0x000000FF;
            v5->bV5AlphaMask = 0xFF000000;
            v5->bV5CSType = LCS_sRGB;

            BYTE* v5Pixels = v5Data + sizeof(BITMAPV5HEADER);

            for (UINT y = 0; y < outHeight; ++y)
            {
                const BYTE* srcRow =
                    pixels.data() +
                    static_cast<size_t>(src.top + static_cast<LONG>(y)) * srcStride +
                    static_cast<size_t>(src.left) * 4;

                std::memcpy(
                    v5Pixels + static_cast<size_t>(y) * outStride,
                    srcRow,
                    outStride);
            }

            GlobalUnlock(dibV5Memory);
        }
        else
        {
            GlobalFree(dibV5Memory);
            dibV5Memory = nullptr;
        }
    }

    if (!OpenClipboard(hwnd))
    {
        if (dibV5Memory)
            GlobalFree(dibV5Memory);
        DeleteObject(bitmap);
        GlobalFree(dibMemory);
        if (pngMemory)
            GlobalFree(pngMemory);
        return false;
    }

    EmptyClipboard();

    bool ok = true;

    const UINT pngFormat =
        RegisterClipboardFormatW(L"PNG");

    if (pngMemory && pngFormat != 0)
    {
        if (!SetClipboardData(pngFormat, pngMemory))
        {
            GlobalFree(pngMemory);
            pngMemory = nullptr;
        }
        else
        {
            pngMemory = nullptr;
        }
    }
    else if (pngMemory)
    {
        GlobalFree(pngMemory);
        pngMemory = nullptr;
    }

    if (!SetClipboardData(CF_DIB, dibMemory))
    {
        ok = false;
        GlobalFree(dibMemory);
        dibMemory = nullptr;
    }
    else
    {
        dibMemory = nullptr;
    }

    if (ok)
    {
        if (!SetClipboardData(CF_BITMAP, bitmap))
        {
            ok = false;
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
        else
        {
            bitmap = nullptr;
        }
    }

    if (ok && dibV5Memory)
    {
        if (!SetClipboardData(CF_DIBV5, dibV5Memory))
        {
            GlobalFree(dibV5Memory);
            dibV5Memory = nullptr;
        }
        else
        {
            dibV5Memory = nullptr;
        }
    }

    CloseClipboard();

    if (dibMemory)
        GlobalFree(dibMemory);

    if (bitmap)
        DeleteObject(bitmap);

    if (dibV5Memory)
        GlobalFree(dibV5Memory);

    if (pngMemory)
        GlobalFree(pngMemory);

    return ok;
}

static void CopyWholeImage(HWND hwnd)
{
    std::vector<BYTE> pixels;
    UINT width = 0;
    UINT height = 0;

    if (!LoadNativeClipboardPixels(pixels, width, height) ||
        !CopyPixelsToClipboard(hwnd, pixels, width, height, nullptr))
    {
        MessageBoxW(
            hwnd,
            L"Could not copy the image to the clipboard.",
            L"LightSpeed",
            MB_OK | MB_ICONERROR);
    }
}


static void CopySelection(HWND hwnd)
{
    RECT crop{};

    if (!GetSelectionImageRect(hwnd, crop))
        return;

    std::vector<BYTE> pixels;
    UINT width = 0;
    UINT height = 0;

    if (!LoadNativeClipboardPixels(pixels, width, height) ||
        !CopyPixelsToClipboard(hwnd, pixels, width, height, &crop))
    {
        MessageBoxW(
            hwnd,
            L"Could not copy the selected area to the clipboard.",
            L"LightSpeed",
            MB_OK | MB_ICONERROR);
    }
}


// ============================================================
// Copy / move file
// ============================================================

static bool IsDirectoryPath(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}


static std::wstring GetStoredDestination()
{
    HKEY key = nullptr;

    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            REG_PATH,
            0,
            KEY_READ,
            &key) != ERROR_SUCCESS)
    {
        return L"C:\\";
    }

    std::wstring path;
    const bool ok = ReadString(key, L"FileOperationPath", path);
    RegCloseKey(key);

    if (ok && IsDirectoryPath(path))
        return path;

    return L"C:\\";
}


static void SaveStoredDestination(const std::wstring& path)
{
    HKEY key = nullptr;

    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            REG_PATH,
            0,
            nullptr,
            0,
            KEY_WRITE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS)
    {
        return;
    }

    SaveString(key, L"FileOperationPath", path);
    RegCloseKey(key);
}


static std::wstring ChooseDestinationFolder(HWND hwnd)
{
    std::wstring initial = GetStoredDestination();

    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"Select destination folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = [](HWND dialog, UINT message, LPARAM, LPARAM data) -> int
    {
        if (message == BFFM_INITIALIZED)
        {
            SendMessageW(
                dialog,
                BFFM_SETSELECTIONW,
                TRUE,
                data);
        }

        return 0;
    };
    bi.lParam = reinterpret_cast<LPARAM>(initial.c_str());

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);

    if (!pidl)
        return std::wstring();

    wchar_t path[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);

    if (!ok || !IsDirectoryPath(path))
        return std::wstring();

    std::wstring result(path);
    SaveStoredDestination(result);
    return result;
}


static std::wstring JoinPath(
    const std::wstring& directory,
    const wchar_t* filename)
{
    if (directory.empty())
        return std::wstring(filename);

    const wchar_t last = directory.back();

    if (last == L'\\' || last == L'/')
        return directory + filename;

    return directory + L"\\" + filename;
}


static void CopyOrMoveCurrentFile(
    HWND hwnd,
    bool move)
{
    if (g_currentFile.empty())
        return;

    const std::wstring destination =
        ChooseDestinationFolder(hwnd);

    if (destination.empty())
        return;

    const size_t slash =
        g_currentFile.find_last_of(L"\\/");

    const wchar_t* filename =
        (slash == std::wstring::npos)
            ? g_currentFile.c_str()
            : g_currentFile.c_str() + slash + 1;

    const std::wstring target =
        JoinPath(destination, filename);

    if (SameFilePath(g_currentFile, target))
    {
        ShowError(
            hwnd,
            L"The source and destination are the same file.",
            move ? L"Move file" : L"Copy file");
        return;
    }

    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        wchar_t message[1024];

        wsprintfW(
            message,
            L"The file already exists:\n\n%s\n\nOverwrite it?",
            target.c_str());

        const int answer = MessageBoxW(
            hwnd,
            message,
            move ? L"Move file" : L"Copy file",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);

        if (answer != IDYES)
            return;
    }

    BOOL ok = FALSE;

    if (move)
    {
        ok = MoveFileExW(
            g_currentFile.c_str(),
            target.c_str(),
            MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
    }
    else
    {
        ok = CopyFileW(
            g_currentFile.c_str(),
            target.c_str(),
            FALSE);
    }

    if (!ok)
    {
        wchar_t message[1024];
        wsprintfW(
            message,
            L"Could not %s file.\n\n%s\n\nError: %lu",
            move ? L"move" : L"copy",
            target.c_str(),
            GetLastError());

        ShowError(
            hwnd,
            message,
            move ? L"Move file" : L"Copy file");
        return;
    }

    if (move)
    {
        // Keep the viewer in the original directory after moving.
        // Remove the moved file from the current list, then open the
        // next file at the same index. If the moved file was the last
        // one, the new last file becomes current.
        const size_t oldIndex = g_currentIndex;

        if (oldIndex < g_files.size())
            g_files.erase(g_files.begin() + static_cast<ptrdiff_t>(oldIndex));

        if (g_files.empty())
        {
            g_currentIndex = 0;
            g_currentFile.clear();
            UpdateWindowTitle(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        if (g_currentIndex >= g_files.size())
            g_currentIndex = g_files.size() - 1;

        g_panX = 0.0f;
        g_panY = 0.0f;
        g_manualRotation = 0;

        if (SUCCEEDED(LoadCurrentImage(hwnd)))
        {
            UpdateWindowTitle(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    else
    {
        // After a successful copy, leave the original file in place
        // and move to the next image in the current directory.
        Navigate(hwnd, +1);
    }
}


// ============================================================
// Window procedure
// ============================================================

static LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
    case WM_TIMER:
    {
        if (wParam == GIF_TIMER_ID &&
            IsCurrentGif() &&
            g_gifPlaying)
        {
            UINT next = g_gifFrameIndex + 1;

            if (next >= g_gifFrameCount)
                next = 0;

            if (SUCCEEDED(LoadGifFrame(hwnd, next)))
            {
                SetTimer(
                    hwnd,
                    GIF_TIMER_ID,
                    g_gifDelayMs,
                    nullptr);

                UpdateWindowTitle(hwnd);

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);
            }
            else
            {
                StopGifPlayback(hwnd);
            }

            return 0;
        }

        return 0;
    }


    case WM_PAINT:
    {
        PAINTSTRUCT ps;

        BeginPaint(
            hwnd,
            &ps);

        DrawImage(hwnd);

        EndPaint(
            hwnd,
            &ps);

        return 0;
    }


    case WM_SIZE:
    {
        if (g_target)
        {
            UINT width =
                static_cast<UINT>(
                    LOWORD(lParam));

            UINT height =
                static_cast<UINT>(
                    HIWORD(lParam));

            if (width && height)
            {
                g_target->Resize(
                    D2D1::SizeU(
                        width,
                        height));
            }
        }

        if (!g_fullscreen)
        {
            RECT rc{};

            if (GetWindowRect(
                    hwnd,
                    &rc))
            {
                g_windowedRect = rc;
                g_haveWindowedRect = true;
            }
        }

        ClampPan(hwnd);

        InvalidateRect(
            hwnd,
            nullptr,
            FALSE);

        return 0;
    }


    case WM_EXITSIZEMOVE:
    {
        if (!g_fullscreen)
        {
            RECT rc{};

            if (GetWindowRect(
                    hwnd,
                    &rc))
            {
                g_windowedRect = rc;
                g_haveWindowedRect = true;

                SaveWindowSettings(hwnd);
            }
        }

        return 0;
    }

    case WM_ERASEBKGND:
        return 1;


    // --------------------------------------------------------
    // Mouse
    // --------------------------------------------------------

    case WM_MOUSEWHEEL:
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const bool ctrl = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;

        if (ctrl)
        {
            if (delta > 0)
                ZoomStep(hwnd, +1);
            else if (delta < 0)
                ZoomStep(hwnd, -1);
        }
        else
        {
            Navigate(hwnd, delta > 0 ? -1 : +1);
        }
        return 0;
    }

    case WM_RBUTTONUP:
    {
        enum
        {
            IDM_PREVIOUS = 1001,
            IDM_NEXT = 1002,
            IDM_ZOOM_IN = 1003,
            IDM_ZOOM_OUT = 1004,
            IDM_ONE_TO_ONE = 1005,
            IDM_AUTO_FIT = 1006,
            IDM_FULLSCREEN = 1007,
            IDM_EXIT = 1008,
            IDM_ABOUT = 1009,
            IDM_COPY = 1010
        };

        HMENU menu = CreatePopupMenu();
        if (menu)
        {
            AppendMenuW(menu, MF_STRING | (g_fullscreen ? MF_CHECKED : 0), IDM_FULLSCREEN,
                g_fullscreen ? L"Windowed mode\tAlt+Enter" : L"Fullscreen\tAlt+Enter");
            AppendMenuW(menu, MF_STRING, IDM_COPY, L"Copy\tCtrl+C");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_PREVIOUS, L"Previous image\tPage Up");
            AppendMenuW(menu, MF_STRING, IDM_NEXT, L"Next image\tPage Down");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_ZOOM_IN, L"Zoom in\tNum +");
            AppendMenuW(menu, MF_STRING, IDM_ZOOM_OUT, L"Zoom out\tNum -");
            AppendMenuW(menu, MF_STRING, IDM_ONE_TO_ONE, L"1:1\tNum /");
            AppendMenuW(menu, MF_STRING, IDM_AUTO_FIT, L"Auto fit\tNum *");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About LightSpeed");
            AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit\tEsc");

            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &pt);

            const int command =
                TrackPopupMenu(
                    menu,
                    TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                    pt.x,
                    pt.y,
                    0,
                    hwnd,
                    nullptr);

            DestroyMenu(menu);

            switch (command)
            {
            case IDM_PREVIOUS:
                Navigate(hwnd, -1);
                break;

            case IDM_NEXT:
                Navigate(hwnd, +1);
                break;

            case IDM_ZOOM_IN:
                ZoomStep(hwnd, +1);
                break;

            case IDM_ZOOM_OUT:
                ZoomStep(hwnd, -1);
                break;

            case IDM_ONE_TO_ONE:
                SetZoom(hwnd, 1.0f);
                break;

            case IDM_AUTO_FIT:
                SetAutoScale(hwnd);
                break;

            case IDM_FULLSCREEN:
                ToggleFullscreen(hwnd);
                break;

            case IDM_EXIT:
                DestroyWindow(hwnd);
                break;

            case IDM_COPY:
                CopyWholeImage(hwnd);
                break;

            case IDM_ABOUT:
                ShowAbout(hwnd);
                break;
            }
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        if (GetKeyState(VK_SHIFT) & 0x8000)
        {
            g_selecting = true;
            g_selectionStart.x = x;
            g_selectionStart.y = y;
            g_selectionEnd = g_selectionStart;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        g_dragging = true;

        g_dragStart.x = x;
        g_dragStart.y = y;

        g_dragStartPanX = g_panX;
        g_dragStartPanY = g_panY;

        SetCapture(hwnd);

        return 0;
    }


    case WM_MOUSEMOVE:
    {
        if (g_selecting)
        {
            g_selectionEnd.x = GET_X_LPARAM(lParam);
            g_selectionEnd.y = GET_Y_LPARAM(lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g_dragging)
        {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);

            g_panX =
                g_dragStartPanX +
                static_cast<float>(x - g_dragStart.x);

            g_panY =
                g_dragStartPanY +
                static_cast<float>(y - g_dragStart.y);

            ClampPan(hwnd);

            InvalidateRect(hwnd, nullptr, FALSE);
        }

        return 0;
    }


    case WM_LBUTTONUP:
    {
        if (g_selecting)
        {
            g_selectionEnd.x = GET_X_LPARAM(lParam);
            g_selectionEnd.y = GET_Y_LPARAM(lParam);
            g_selecting = false;
            ReleaseCapture();
            CopySelection(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g_dragging)
        {
            g_dragging = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        return 0;
    }


    case WM_MBUTTONUP:
        ToggleFullscreen(hwnd);
        return 0;


    // --------------------------------------------------------
    // Keyboard
    // --------------------------------------------------------

    case WM_KEYDOWN:
    {
        const bool ctrl =
            (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (ctrl &&
            (wParam == 'C' || wParam == VK_INSERT))
        {
            CopyWholeImage(hwnd);
            return 0;
        }

        if (!ctrl && wParam == 'C')
        {
            CopyOrMoveCurrentFile(hwnd, false);
            return 0;
        }

        if (!ctrl && wParam == 'M')
        {
            CopyOrMoveCurrentFile(hwnd, true);
            return 0;
        }

        if (wParam == VK_ESCAPE)
        {
            DestroyWindow(hwnd);
            return 0;
        }

        // Home = first file
        if (wParam == VK_HOME)
        {
            if (!g_files.empty() && g_currentIndex != 0)
                Navigate(hwnd, -static_cast<int>(g_currentIndex));

            return 0;
        }

        // End = last file
        if (wParam == VK_END)
        {
            if (!g_files.empty())
            {
                const int lastIndex = static_cast<int>(g_files.size()) - 1;
                const int delta = lastIndex - static_cast<int>(g_currentIndex);
                if (delta != 0)
                    Navigate(hwnd, delta);
            }

            return 0;
        }

        // PageDown = next
        if (wParam == VK_NEXT)
        {
            Navigate(
                hwnd,
                +1);

            return 0;
        }

        // PageUp = previous
        if (wParam == VK_PRIOR)
        {
            Navigate(
                hwnd,
                -1);

            return 0;
        }

        // I = information
        if (wParam == 'I')
        {
            ShowFileInfo(hwnd);
            return 0;
        }

        // For GIFs, Left/Right stop automatic playback and
        // move one frame. There is no wraparound.
        if (!ctrl &&
            IsCurrentGif() &&
            wParam == VK_LEFT)
        {
            StepGifFrameBackward(hwnd);
            return 0;
        }

        if (!ctrl &&
            IsCurrentGif() &&
            wParam == VK_RIGHT)
        {
            StepGifFrameForward(hwnd);
            return 0;
        }

        // Ctrl+Left = 90 degrees counter-clockwise.
        if (ctrl && wParam == VK_LEFT)
        {
            g_manualRotation = (g_manualRotation + 3) & 3;
            g_panX = 0.0f;
            g_panY = 0.0f;

            if (SUCCEEDED(LoadCurrentImage(hwnd)))
                InvalidateRect(hwnd, nullptr, FALSE);

            UpdateWindowTitle(hwnd);
            return 0;
        }

        // Ctrl+Right = 90 degrees clockwise.
        if (ctrl && wParam == VK_RIGHT)
        {
            g_manualRotation = (g_manualRotation + 1) & 3;
            g_panX = 0.0f;
            g_panY = 0.0f;

            if (SUCCEEDED(LoadCurrentImage(hwnd)))
                InvalidateRect(hwnd, nullptr, FALSE);

            UpdateWindowTitle(hwnd);
            return 0;
        }

        // NumPad / = 1:1
        if (wParam == VK_DIVIDE)
        {
            SetOneToOne(hwnd);
            return 0;
        }

        // NumPad + = zoom in
        if (wParam == VK_ADD)
        {
            if (g_autoScale)
            {
                ZoomStep(hwnd, +1);
            }
            else
            {
                ZoomStep(hwnd, +1);
            }

            return 0;
        }

        // NumPad - = zoom out
        if (wParam == VK_SUBTRACT)
        {
            if (g_autoScale)
            {
                ZoomStep(hwnd, -1);
            }
            else
            {
                ZoomStep(hwnd, -1);
            }

            return 0;
        }

        // NumPad * = Auto
        if (wParam == VK_MULTIPLY)
        {
            SetAutoScale(hwnd);
            return 0;
        }

        return 0;
    }


    // --------------------------------------------------------
    // Alt + Enter
    // --------------------------------------------------------

    case WM_SYSKEYDOWN:
    {
        // F10 opens the same context menu as the right mouse button.
        if (wParam == VK_F10)
        {
            POINT pt{20, 20};
            SendMessageW(hwnd, WM_RBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
            return 0;
        }

        if (wParam == VK_RETURN)
        {
            ToggleFullscreen(hwnd);
            return 0;
        }

        return DefWindowProcW(
            hwnd,
            message,
            wParam,
            lParam);
    }


    case WM_LBUTTONDBLCLK:
        return 0;


    case WM_DESTROY:
        StopGifPlayback(hwnd);
        ReleaseGifDecoder();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam);
}


// ============================================================
// Entry point
// ============================================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    // --------------------------------------------------------
    // Command line
    // --------------------------------------------------------

    int argc = 0;

    LPWSTR* argv =
        CommandLineToArgvW(
            GetCommandLineW(),
            &argc);

    if (!argv || argc < 2)
    {
        if (argv)
            LocalFree(argv);

        ShowHelp();
        return 1;
    }

    if (_wcsicmp(argv[1], L"/?") == 0 ||
        _wcsicmp(argv[1], L"--help") == 0)
    {
        LocalFree(argv);
        ShowHelp();
        return 0;
    }

    const wchar_t* filename =
        argv[1];


    // --------------------------------------------------------
    // COM
    // --------------------------------------------------------

    HRESULT hr =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED);

    if (FAILED(hr))
    {
        LocalFree(argv);
        return 1;
    }


    // --------------------------------------------------------
    // Direct2D
    // --------------------------------------------------------

    hr =
        D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            &g_factory);

    if (FAILED(hr))
    {
        CoUninitialize();
        LocalFree(argv);

        ShowError(
            nullptr,
            L"Direct2D initialization failed.",
            L"LightSpeed");

        return 1;
    }


    // --------------------------------------------------------
    // Window settings
    // --------------------------------------------------------

    RECT savedWindowRect{};

    const bool haveSavedSettings =
        LoadWindowSettings(
            g_fullscreen,
            savedWindowRect);

    if (haveSavedSettings)
    {
        g_windowedRect =
            savedWindowRect;

        g_haveWindowedRect = true;
    }


    // --------------------------------------------------------
    // File list
    // --------------------------------------------------------

    BuildFileList(filename);

    if (g_files.empty())
    {
        g_files.push_back(filename);
        g_currentIndex = 0;
    }


    // --------------------------------------------------------
    // Monitor under mouse
    // --------------------------------------------------------

    POINT cursor;

    GetCursorPos(
        &cursor);

    HMONITOR monitor =
        MonitorFromPoint(
            cursor,
            MONITOR_DEFAULTTONEAREST);

    MONITORINFO mi{};

    mi.cbSize =
        sizeof(mi);

    GetMonitorInfoW(
        monitor,
        &mi);

    const RECT monitorRect =
        mi.rcMonitor;

    const UINT screenWidth =
        static_cast<UINT>(
            monitorRect.right -
            monitorRect.left);

    const UINT screenHeight =
        static_cast<UINT>(
            monitorRect.bottom -
            monitorRect.top);


    // --------------------------------------------------------
    // Window class
    // --------------------------------------------------------

    const wchar_t CLASS_NAME[] =
        L"LightSpeedWindow";

    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(wc);

    wc.hInstance =
        hInstance;

    wc.lpfnWndProc =
        WindowProc;

    wc.hIcon =
        LoadIconW(
            hInstance,
            MAKEINTRESOURCEW(1));

    wc.hIconSm =
        LoadIconW(
            hInstance,
            MAKEINTRESOURCEW(1));

    wc.lpszClassName =
        CLASS_NAME;

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW);

    wc.hbrBackground =
        static_cast<HBRUSH>(
            GetStockObject(
                BLACK_BRUSH));

    wc.style =
        CS_HREDRAW |
        CS_VREDRAW |
        CS_DBLCLKS;

    RegisterClassExW(&wc);


    // --------------------------------------------------------
    // Determine initial window
    // --------------------------------------------------------

    DWORD windowStyle;

    int windowX;
    int windowY;
    int windowWidth;
    int windowHeight;

    if (g_fullscreen)
    {
        windowStyle =
            WS_POPUP;

        windowX =
            monitorRect.left;

        windowY =
            monitorRect.top;

        windowWidth =
            static_cast<int>(
                screenWidth);

        windowHeight =
            static_cast<int>(
                screenHeight);
    }
    else if (g_haveWindowedRect)
    {
        windowStyle =
            WS_OVERLAPPEDWINDOW;

        windowX =
            g_windowedRect.left;

        windowY =
            g_windowedRect.top;

        windowWidth =
            g_windowedRect.right -
            g_windowedRect.left;

        windowHeight =
            g_windowedRect.bottom -
            g_windowedRect.top;
    }
    else
    {
        windowStyle =
            WS_OVERLAPPEDWINDOW;

        windowWidth =
            static_cast<int>(
                screenWidth * 3 / 4);

        windowHeight =
            static_cast<int>(
                screenHeight * 3 / 4);

        windowX =
            monitorRect.left +
            (static_cast<int>(screenWidth) -
             windowWidth) / 2;

        windowY =
            monitorRect.top +
            (static_cast<int>(screenHeight) -
             windowHeight) / 2;

        g_windowedRect.left =
            windowX;

        g_windowedRect.top =
            windowY;

        g_windowedRect.right =
            windowX + windowWidth;

        g_windowedRect.bottom =
            windowY + windowHeight;

        g_haveWindowedRect = true;
    }


    // --------------------------------------------------------
    // Create window
    // --------------------------------------------------------

    HWND hwnd =
        CreateWindowExW(
            WS_EX_APPWINDOW,
            CLASS_NAME,
            L"LightSpeed",
            windowStyle,
            windowX,
            windowY,
            windowWidth,
            windowHeight,
            nullptr,
            nullptr,
            hInstance,
            nullptr);

    if (!hwnd)
    {
        ReleaseAll();
        CoUninitialize();
        LocalFree(argv);

        return 1;
    }


    // --------------------------------------------------------
    // Direct2D target
    // --------------------------------------------------------

    hr =
        CreateRenderTarget(hwnd);

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);

        ReleaseAll();
        CoUninitialize();
        LocalFree(argv);

        return 1;
    }


    // --------------------------------------------------------
    // First image
    // --------------------------------------------------------

    hr =
        LoadCurrentImage(hwnd);

    if (FAILED(hr))
    {
        wchar_t message[1024];

        wsprintfW(
            message,
            L"Could not load image:\n\n%s\n\n"
            L"Supported formats:\n"
            L"JPEG / JPG / PNG / WebP / GIF / SVG / ICO",
            filename);

        ShowError(
            hwnd,
            message,
            L"LightSpeed");

        DestroyWindow(hwnd);

        ReleaseAll();

        CoUninitialize();

        LocalFree(argv);

        return 1;
    }


    // --------------------------------------------------------
    // Window title
    // --------------------------------------------------------

    UpdateWindowTitle(hwnd);

    // --------------------------------------------------------
    // Show
    // --------------------------------------------------------

    if (g_fullscreen)
    {
        ShowWindow(
            hwnd,
            SW_SHOW);
    }
    else
    {
        ShowWindow(
            hwnd,
            SW_SHOWNORMAL);
    }

    UpdateWindow(hwnd);

    SetForegroundWindow(hwnd);
    SetFocus(hwnd);


    // --------------------------------------------------------
    // Message loop
    // --------------------------------------------------------

    MSG msg{};

    while (GetMessageW(
        &msg,
        nullptr,
        0,
        0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    const int result =
        static_cast<int>(
            msg.wParam);


    // --------------------------------------------------------
    // Save settings
    // --------------------------------------------------------

    SaveWindowSettings(hwnd);


    // --------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------

    ReleaseAll();

    CoUninitialize();

    LocalFree(argv);

    return result;
}
