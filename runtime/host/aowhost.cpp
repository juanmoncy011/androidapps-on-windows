// aowhost - stand-in app process for the simulated ("host") runtime backend.
//
// Without an Android runtime the Runtime Manager still needs a real process per
// app to launch, track and stop. aowhost is that process: on Windows it opens a
// window showing the app's icon and details; elsewhere it just runs until stopped.
//
//   aowhost --package P [--label L] [--version V] [--apk PATH] [--activity A] [--icon PNG]
//           [--exit-after-ms N] [--exit-code C]     (the last two simulate an exit/crash)
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#else
#include <csignal>
#include <thread>
#endif

namespace {

struct Args {
    std::map<std::string, std::string> values;
    std::string get(const std::string& k) const {
        auto it = values.find(k);
        return it == values.end() ? std::string() : it->second;
    }
    int getInt(const std::string& k, int fallback) const {
        const std::string v = get(k);
        return v.empty() ? fallback : std::atoi(v.c_str());
    }
};

} // namespace

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

struct State {
    Args args;
    HBITMAP icon = nullptr;
    int iconSize = 0;
    HFONT titleFont = nullptr, textFont = nullptr;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    int exitCode = 0;
};
State g;

constexpr UINT_PTR kTickTimer = 1, kExitTimer = 2;

// PNG/WebP/JPEG -> premultiplied 32-bit DIB (for AlphaBlend).
HBITMAP loadImage(const std::wstring& path, int size) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(f->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(conv.Get(), UINT(size), UINT(size), WICBitmapInterpolationModeFant)))
        return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && FAILED(scaler->CopyPixels(nullptr, UINT(size * 4), UINT(size * size * 4), static_cast<BYTE*>(bits)))) {
        DeleteObject(bmp);
        bmp = nullptr;
    }
    return bmp;
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const UINT dpi = GetDpiForWindow(hwnd);
    auto px = [dpi](int v) { return MulDiv(v, int(dpi), 96); };

    HBRUSH bg = CreateSolidBrush(RGB(0xF4, 0xF6, 0xF8));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    int y = px(24);
    if (g.icon) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g.icon);
        const int d = px(96);
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(dc, (rc.right - d) / 2, y, d, d, mem, 0, 0, g.iconSize, g.iconSize, bf);
        SelectObject(mem, old);
        DeleteDC(mem);
        y += d + px(12);
    }

    auto line = [&](HFONT font, COLORREF color, const std::wstring& text, int height) {
        SelectObject(dc, font);
        SetTextColor(dc, color);
        RECT r{px(16), y, rc.right - px(16), y + height};
        DrawTextW(dc, text.c_str(), -1, &r, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += height;
    };
    const std::string label = g.args.get("label").empty() ? g.args.get("package") : g.args.get("label");
    line(g.titleFont, RGB(0x20, 0x21, 0x24), widen(label), px(34));
    line(g.textFont, RGB(0x5F, 0x63, 0x68), widen(g.args.get("package") + "  " + g.args.get("version")), px(22));
    if (!g.args.get("activity").empty()) line(g.textFont, RGB(0x5F, 0x63, 0x68), widen(g.args.get("activity")), px(22));

    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g.start).count();
    wchar_t status[128];
    swprintf_s(status, L"Running in the simulated runtime — PID %lu — up %lld:%02lld",
               GetCurrentProcessId(), secs / 60, secs % 60);
    y += px(12);
    line(g.textFont, RGB(0x18, 0x80, 0x38), status, px(22));
    line(g.textFont, RGB(0x80, 0x86, 0x8B), L"A real Android runtime (adb backend) would show the app here.", px(22));
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: paint(hwnd); return 0;
    case WM_TIMER:
        if (wp == kExitTimer) { DestroyWindow(hwnd); return 0; }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DESTROY: PostQuitMessage(g.exitCode); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HFONT makeFont(int points, int weight, UINT dpi) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(points, int(dpi), 72);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; i += 2) {
        std::string key = narrow(argv[i]);
        if (key.rfind("--", 0) == 0) g.args.values[key.substr(2)] = narrow(argv[i + 1]);
    }
    if (argv) LocalFree(argv);
    if (g.args.get("package").empty()) {
        MessageBoxW(nullptr, L"aowhost is started by the Runtime Manager (aowrt / AowLauncher).", L"aowhost", MB_OK);
        return 2;
    }
    g.exitCode = g.args.getInt("exit-code", 0);

    const UINT dpi = GetDpiForSystem();
    g.iconSize = MulDiv(96, int(dpi), 96);
    if (!g.args.get("icon").empty()) g.icon = loadImage(widen(g.args.get("icon")), g.iconSize);
    g.titleFont = makeFont(16, FW_SEMIBOLD, dpi);
    g.textFont = makeFont(10, FW_NORMAL, dpi);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"AowHostWindow";
    RegisterClassExW(&wc);

    const std::string label = g.args.get("label").empty() ? g.args.get("package") : g.args.get("label");
    const std::wstring title = widen(label) + L" — Android (simulated)";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(460, int(dpi), 96), MulDiv(400, int(dpi), 96), nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, showCommand);
    SetTimer(hwnd, kTickTimer, 1000, nullptr);
    if (const int ms = g.args.getInt("exit-after-ms", 0); ms > 0) SetTimer(hwnd, kExitTimer, UINT(ms), nullptr);

    MSG msg;
    int code = g.exitCode;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) code = int(msg.wParam);
    if (g.icon) DeleteObject(g.icon);
    DeleteObject(g.titleFont);
    DeleteObject(g.textFont);
    CoUninitialize();
    return code;
}

#else  // POSIX: headless stand-in -----------------------------------------------

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
} // namespace

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        if (key.rfind("--", 0) == 0) args.values[key.substr(2)] = argv[i + 1];
    }
    if (args.get("package").empty()) return 2;
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    const int exitAfter = args.getInt("exit-after-ms", 0);
    const auto start = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (exitAfter > 0 && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(exitAfter))
            return args.getInt("exit-code", 0);
    }
    return 0;
}

#endif
