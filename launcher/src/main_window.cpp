#include "main_window.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <memory>
#include <numeric>
#include <sstream>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
constexpr UINT WM_APP_SCAN_DONE = WM_APP + 2;
constexpr UINT WM_APP_INSTALL_PROGRESS = WM_APP + 3;
constexpr UINT WM_APP_INSTALL_DONE = WM_APP + 4;

enum ControlId : int { IDC_OPEN = 101, IDC_RESCAN, IDC_RECURSIVE, IDC_FOLDER, IDC_LIST, IDC_STATUS, IDC_INSTALL, IDC_UNINSTALL };
enum Column : int { ColApp, ColPackage, ColVersion, ColStatus, ColMinSdk, ColTargetSdk, ColAbis, ColSize, ColFile, ColCount };

struct ColumnDef { const wchar_t* title; int width; int format; };
constexpr ColumnDef kColumns[ColCount] = {
    {L"App", 220, LVCFMT_LEFT},     {L"Package", 250, LVCFMT_LEFT},     {L"Version", 130, LVCFMT_LEFT},
    {L"Status", 190, LVCFMT_LEFT},
    {L"Min SDK", 70, LVCFMT_RIGHT}, {L"Target SDK", 85, LVCFMT_RIGHT}, {L"ABIs", 150, LVCFMT_LEFT},
    {L"Size", 80, LVCFMT_RIGHT},    {L"File", 240, LVCFMT_LEFT},
};

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring formatSize(std::uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; ++u; }
    wchar_t buf[32];
    swprintf_s(buf, u == 0 ? L"%.0f %s" : L"%.1f %s", v, units[u]);
    return buf;
}

std::wstring sdkText(int sdk) { return sdk > 0 ? std::to_wstring(sdk) : std::wstring(L"\u2014"); }

using InstalledMap = MainWindow::InstalledMap;

std::wstring cellText(const apkcore::ApkInfo& a, int col, const InstalledMap& installed) {
    switch (col) {
    case ColApp: return widen(a.displayName());
    case ColPackage: return a.ok() ? widen(a.packageName) : L"Error: " + widen(a.error);
    case ColVersion: {
        if (!a.ok()) return {};
        std::wstring v = widen(a.versionName);
        if (a.versionCode) {
            if (!v.empty()) v += L' ';
            v += L"(" + std::to_wstring(a.versionCode) + L")";
        }
        return v;
    }
    case ColStatus: {
        if (!a.ok()) return {};
        const auto it = installed.find(a.packageName);
        if (it == installed.end()) return L"Not installed";
        const auto& v = it->second;
        if (v.versionCode == a.versionCode) return L"Installed";
        const std::wstring ver = widen(v.versionName.empty() ? std::to_string(v.versionCode) : v.versionName);
        return (a.versionCode > v.versionCode ? L"Update available (installed " : L"Older (installed ") + ver + L")";
    }
    case ColMinSdk: return a.ok() ? sdkText(a.minSdk) : std::wstring();
    case ColTargetSdk: return a.ok() ? sdkText(a.targetSdk) : std::wstring();
    case ColAbis: {
        if (!a.ok()) return {};
        if (a.nativeAbis.empty()) return L"none";
        std::wstring s;
        for (const auto& abi : a.nativeAbis) { if (!s.empty()) s += L", "; s += widen(abi); }
        return s;
    }
    case ColSize: return formatSize(a.fileSize);
    case ColFile: return a.path.filename().wstring();
    default: return {};
    }
}

int compareApps(const apkcore::ApkInfo& a, const apkcore::ApkInfo& b, int col, const InstalledMap& installed) {
    auto num = [](auto x, auto y) { return x < y ? -1 : (x > y ? 1 : 0); };
    switch (col) {
    case ColMinSdk: return num(a.minSdk, b.minSdk);
    case ColTargetSdk: return num(a.targetSdk, b.targetSdk);
    case ColSize: return num(a.fileSize, b.fileSize);
    case ColVersion: if (int c = num(a.versionCode, b.versionCode)) return c; break;
    default: break;
    }
    const std::wstring x = cellText(a, col, installed), y = cellText(b, col, installed);
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                           x.c_str(), int(x.size()), y.c_str(), int(y.size()), nullptr, nullptr, 0) - CSTR_EQUAL;
}

// Decode PNG/WebP bytes into a premultiplied 32-bit DIB of size x size.
HBITMAP decodeIcon(IWICImagingFactory* factory, const std::vector<std::uint8_t>& data, int size) {
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return nullptr;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), DWORD(data.size())))) return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom))) return nullptr;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler))) return nullptr;
    if (FAILED(scaler->Initialize(converter.Get(), UINT(size), UINT(size), WICBitmapInterpolationModeFant))) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;
    if (FAILED(scaler->CopyPixels(nullptr, UINT(size * 4), UINT(size * size * 4), static_cast<BYTE*>(bits)))) {
        DeleteObject(bmp);
        return nullptr;
    }
    return bmp;
}

} // namespace

// ---------------------------------------------------------------------------

MainWindow::~MainWindow() {
    stopWorker();
    installCancel_ = true;
    if (installer_.joinable()) installer_.join();
}

bool MainWindow::create(HINSTANCE instance, int showCommand, fs::path initialFolder) {
    inst_ = instance;

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AowLauncherWindow";
    RegisterClassExW(&wc);

    if (initialFolder.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) initialFolder = p;
        CoTaskMemFree(p);
    }
    folder_ = initialFolder;

    try {
        pm_ = std::make_unique<pkgmgr::PackageManager>();
    } catch (const std::exception& e) {
        pmError_ = L"Package Manager unavailable: " + widen(e.what());
    }

    const int sysDpi = int(GetDpiForSystem());
    HWND h = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Android Apps on Windows \u2014 Launcher",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(1200, sysDpi, 96), MulDiv(700, sysDpi, 96),
                             nullptr, nullptr, instance, this);
    if (!h) return false;
    ShowWindow(h, showCommand);
    UpdateWindow(h);
    startScan();
    return true;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        onCreate();
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {scale(640), scale(360)};
        return 0;
    }

    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        createFont();
        const RECT* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OPEN: chooseFolder(); return 0;
        case IDC_RESCAN: startScan(); return 0;
        case IDC_RECURSIVE: if (HIWORD(wp) == BN_CLICKED) startScan(); return 0;
        case IDC_INSTALL: installSelected(); return 0;
        case IDC_UNINSTALL: uninstallSelected(); return 0;
        }
        break;

    case WM_NOTIFY:
        return onNotify(reinterpret_cast<NMHDR*>(lp));

    case WM_DROPFILES:
        onDrop(reinterpret_cast<HDROP>(wp));
        return 0;

    case WM_APP_PROGRESS:
        setStatus(L"Scanning\u2026 " + std::to_wstring(wp) + L" / " + std::to_wstring(lp));
        return 0;

    case WM_APP_SCAN_DONE: {
        std::unique_ptr<std::vector<apkcore::ApkInfo>> result(reinterpret_cast<std::vector<apkcore::ApkInfo>*>(lp));
        if (wp == 0) onScanDone(std::move(*result));  // wp==1 -> cancelled/stale scan
        return 0;
    }

    case WM_APP_INSTALL_PROGRESS:
        setStatus(L"Installing\u2026 " + std::to_wstring(wp + 1) + L" / " + std::to_wstring(lp));
        return 0;

    case WM_APP_INSTALL_DONE: {
        std::unique_ptr<InstallBatch> batch(reinterpret_cast<InstallBatch*>(lp));
        onInstallDone(std::move(*batch));
        return 0;
    }

    case WM_DESTROY: {
        stopWorker();
        installCancel_ = true;
        if (installer_.joinable()) installer_.join();
        MSG m;
        while (PeekMessageW(&m, hwnd_, WM_APP_SCAN_DONE, WM_APP_SCAN_DONE, PM_REMOVE))
            delete reinterpret_cast<std::vector<apkcore::ApkInfo>*>(m.lParam);
        while (PeekMessageW(&m, hwnd_, WM_APP_INSTALL_DONE, WM_APP_INSTALL_DONE, PM_REMOVE))
            delete reinterpret_cast<InstallBatch*>(m.lParam);
        if (images_) { ImageList_Destroy(images_); images_ = nullptr; }
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        PostQuitMessage(0);
        return 0;
    }

    case WM_NCDESTROY: {
        HWND h = hwnd_;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        return DefWindowProcW(h, msg, wp, lp);
    }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void MainWindow::onCreate() {
    dpi_ = GetDpiForWindow(hwnd_);
    createFont();

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return c;
    };
    btnOpen_ = make(WC_BUTTONW, L"Open Folder\u2026", WS_TABSTOP | BS_PUSHBUTTON, IDC_OPEN);
    btnRescan_ = make(WC_BUTTONW, L"Rescan (F5)", WS_TABSTOP | BS_PUSHBUTTON, IDC_RESCAN);
    chkRecursive_ = make(WC_BUTTONW, L"Include subfolders", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_RECURSIVE);
    btnInstall_ = make(WC_BUTTONW, L"Install (Ctrl+I)", WS_TABSTOP | BS_PUSHBUTTON, IDC_INSTALL);
    btnUninstall_ = make(WC_BUTTONW, L"Uninstall", WS_TABSTOP | BS_PUSHBUTTON, IDC_UNINSTALL);
    lblFolder_ = make(WC_STATICW, L"", SS_LEFT | SS_PATHELLIPSIS | SS_CENTERIMAGE, IDC_FOLDER);
    list_ = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS, IDC_LIST);
    status_ = make(STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, IDC_STATUS);

    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(list_, L"Explorer", nullptr);
    for (int i = 0; i < ColCount; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = kColumns[i].format;
        c.cx = scale(kColumns[i].width);
        c.pszText = const_cast<LPWSTR>(kColumns[i].title);
        ListView_InsertColumn(list_, i, &c);
    }
    updateSortArrow();

    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    SetWindowTextW(lblFolder_, folder_.empty() ? L"No folder selected" : folder_.c_str());
    updateButtons();
}

void MainWindow::createFont() {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(9, int(dpi_), 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT f = CreateFontIndirectW(&lf);
    for (HWND c : {btnOpen_, btnRescan_, chkRecursive_, btnInstall_, btnUninstall_, lblFolder_, list_, status_})
        if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    if (font_) DeleteObject(font_);
    font_ = f;
}

void MainWindow::layout() {
    if (!list_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(status_, &sr);
    const int statusH = sr.bottom - sr.top;

    const int pad = scale(8), btnH = scale(28);
    int x = pad;
    auto place = [&](HWND h, int w) { MoveWindow(h, x, pad, w, btnH, TRUE); x += w + pad; };
    place(btnOpen_, scale(115));
    place(btnRescan_, scale(105));
    place(chkRecursive_, scale(140));
    place(btnInstall_, scale(115));
    place(btnUninstall_, scale(90));
    MoveWindow(lblFolder_, x, pad, std::max(0, int(rc.right) - x - pad), btnH, TRUE);

    const int top = pad * 2 + btnH;
    MoveWindow(list_, 0, top, rc.right, std::max(0, int(rc.bottom) - top - statusH), TRUE);
}

void MainWindow::setStatus(const std::wstring& text) {
    if (status_) SetWindowTextW(status_, text.c_str());
}

void MainWindow::setFolder(const fs::path& folder) {
    folder_ = folder;
    SetWindowTextW(lblFolder_, folder_.c_str());
    startScan();
}

void MainWindow::chooseFolder() {
    if (!hwnd_) return;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose a folder containing APK files");
    if (!folder_.empty()) {
        ComPtr<IShellItem> start;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder_.c_str(), nullptr, IID_PPV_ARGS(&start))))
            dlg->SetFolder(start.Get());
    }
    if (FAILED(dlg->Show(hwnd_))) return;  // cancelled
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        setFolder(path);
        CoTaskMemFree(path);
    }
}

void MainWindow::onDrop(HDROP drop) {
    const UINT len = DragQueryFileW(drop, 0, nullptr, 0);
    std::wstring p(len, L'\0');
    DragQueryFileW(drop, 0, p.data(), len + 1);
    DragFinish(drop);

    const fs::path path(p);
    std::error_code ec;
    if (fs::is_directory(path, ec)) setFolder(path);
    else if (path.has_parent_path()) setFolder(path.parent_path());
}

void MainWindow::stopWorker() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void MainWindow::startScan() {
    if (!hwnd_) return;
    if (folder_.empty()) { setStatus(L"Choose a folder that contains .apk files."); return; }

    stopWorker();
    cancel_ = false;
    const bool recursive = SendMessageW(chkRecursive_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    scanStart_ = std::chrono::steady_clock::now();
    setStatus(L"Scanning " + folder_.wstring() + L"\u2026");
    EnableWindow(btnRescan_, FALSE);

    HWND h = hwnd_;
    const fs::path folder = folder_;
    worker_ = std::thread([this, h, folder, recursive] {
        auto progress = [this, h](std::size_t done, std::size_t total) {
            PostMessageW(h, WM_APP_PROGRESS, done, static_cast<LPARAM>(total));
            return !cancel_.load();
        };
        auto result = std::make_unique<std::vector<apkcore::ApkInfo>>(
            apkcore::scanFolder(folder, apkcore::ScanOptions{recursive}, progress));
        const WPARAM cancelled = cancel_.load() ? 1 : 0;
        if (PostMessageW(h, WM_APP_SCAN_DONE, cancelled, reinterpret_cast<LPARAM>(result.get())))
            result.release();  // ownership passes to the UI thread
    });
}

void MainWindow::onScanDone(std::vector<apkcore::ApkInfo> apps) {
    apps_ = std::move(apps);
    refreshInstalled();
    buildIcons();
    applySort();
    populate();
    EnableWindow(btnRescan_, TRUE);
    updateButtons();

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - scanStart_).count();
    const auto failed = std::count_if(apps_.begin(), apps_.end(), [](const auto& a) { return !a.ok(); });
    wchar_t buf[256];
    if (apps_.empty())
        swprintf_s(buf, L"No .apk files found (%.1fs). Drop a folder here or use Open Folder.", secs);
    else
        swprintf_s(buf, L"%zu APK(s) found, %zu failed to parse \u2014 %.1fs. Double-click an app for details.",
                   apps_.size(), std::size_t(failed), secs);
    std::wstring text = buf;
    if (!pmError_.empty()) text += L"  |  " + pmError_;
    else text += L"  |  " + std::to_wstring(installed_.size()) + L" app(s) installed";
    setStatus(text);
}

void MainWindow::buildIcons() {
    const int size = scale(24);
    HIMAGELIST il = ImageList_Create(size, size, ILC_COLOR32, int(apps_.size()) + 1, 8);
    ImageList_AddIcon(il, LoadIconW(nullptr, IDI_APPLICATION));  // index 0 = fallback icon

    iconIndex_.assign(apps_.size(), 0);
    for (std::size_t i = 0; i < apps_.size(); ++i) {
        if (apps_[i].iconData.empty() || !wic_) continue;
        if (HBITMAP bmp = decodeIcon(wic_.Get(), apps_[i].iconData, size)) {
            const int idx = ImageList_Add(il, bmp, nullptr);
            DeleteObject(bmp);
            if (idx >= 0) iconIndex_[i] = idx;
        }
    }
    if (HIMAGELIST old = ListView_SetImageList(list_, il, LVSIL_SMALL)) ImageList_Destroy(old);
    images_ = il;
}

void MainWindow::applySort() {
    std::vector<std::size_t> order(apps_.size());
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const int c = compareApps(apps_[a], apps_[b], sortColumn_, installed_);
        return sortAscending_ ? c < 0 : c > 0;
    });
    std::vector<apkcore::ApkInfo> sorted;
    std::vector<int> icons;
    sorted.reserve(order.size());
    icons.reserve(order.size());
    for (std::size_t i : order) {
        sorted.push_back(std::move(apps_[i]));
        icons.push_back(iconIndex_[i]);
    }
    apps_ = std::move(sorted);
    iconIndex_ = std::move(icons);
}

void MainWindow::sortBy(int column) {
    if (column == sortColumn_) sortAscending_ = !sortAscending_;
    else { sortColumn_ = column; sortAscending_ = true; }
    applySort();
    populate();
    updateSortArrow();
}

void MainWindow::updateSortArrow() {
    HWND header = ListView_GetHeader(list_);
    for (int i = 0; i < ColCount; ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(header, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == sortColumn_) hi.fmt |= sortAscending_ ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, i, &hi);
    }
}

void MainWindow::populate() {
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    for (int i = 0; i < int(apps_.size()); ++i) {
        std::wstring name = cellText(apps_[std::size_t(i)], ColApp, installed_);
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        it.iItem = i;
        it.pszText = name.data();
        it.iImage = iconIndex_[std::size_t(i)];
        it.lParam = i;
        const int row = ListView_InsertItem(list_, &it);
        for (int c = 1; c < ColCount; ++c) {
            std::wstring t = cellText(apps_[std::size_t(i)], c, installed_);
            ListView_SetItemText(list_, row, c, t.data());
        }
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
}

LRESULT MainWindow::onNotify(NMHDR* nm) {
    if (nm->hwndFrom != list_) return 0;
    switch (nm->code) {
    case LVN_ITEMACTIVATE: {  // double-click or Enter
        const auto* ia = reinterpret_cast<NMITEMACTIVATE*>(nm);
        if (ia->iItem >= 0 && ia->iItem < int(apps_.size())) showDetails(apps_[std::size_t(ia->iItem)]);
        break;
    }
    case LVN_COLUMNCLICK:
        sortBy(reinterpret_cast<NMLISTVIEW*>(nm)->iSubItem);
        break;
    case LVN_ITEMCHANGED:
        if (reinterpret_cast<NMLISTVIEW*>(nm)->uChanged & LVIF_STATE) updateButtons();
        break;
    }
    return 0;
}

void MainWindow::showDetails(const apkcore::ApkInfo& a) {
    std::wostringstream s;
    s << L"File: " << a.path.wstring() << L"\nSize: " << formatSize(a.fileSize) << L"\n\n";
    if (!a.ok()) {
        s << L"Could not parse this APK:\n" << widen(a.error);
    } else {
        s << L"App name: " << widen(a.displayName())
          << L"\nPackage: " << widen(a.packageName)
          << L"\nVersion: " << cellText(a, ColVersion, installed_)
          << L"\nMin SDK: " << sdkText(a.minSdk) << L"    Target SDK: " << sdkText(a.targetSdk)
          << L"\nNative ABIs: " << cellText(a, ColAbis, installed_)
          << L"\nLaunch activity: "
          << (a.launchActivity.empty() ? std::wstring(L"(none \u2014 not launchable)") : widen(a.launchActivity));
        if (!a.splitName.empty()) s << L"\nSplit APK: " << widen(a.splitName);
        if (!a.iconPath.empty()) s << L"\nIcon: " << widen(a.iconPath);
        s << L"\nStatus: " << cellText(a, ColStatus, installed_);
        if (pm_ && installed_.count(a.packageName))
            if (const auto p = pm_->find(a.packageName)) s << L"\nInstalled copy: " << p->apkPath.wstring();

        s << L"\n\nPermissions (" << a.permissions.size() << L"):";
        const std::size_t shown = std::min<std::size_t>(a.permissions.size(), 20);
        for (std::size_t i = 0; i < shown; ++i) s << L"\n  \u2022 " << widen(a.permissions[i]);
        if (a.permissions.size() > shown) s << L"\n  \u2026 and " << (a.permissions.size() - shown) << L" more";
    }
    MessageBoxW(hwnd_, s.str().c_str(), widen(a.displayName()).c_str(),
                MB_OK | (a.ok() ? MB_ICONINFORMATION : MB_ICONWARNING));
}

// ---- Package Manager (M2) ---------------------------------------------------

std::vector<int> MainWindow::selectedRows() const {
    std::vector<int> rows;
    for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list_, i, LVNI_SELECTED))
        if (i < int(apps_.size())) rows.push_back(i);
    return rows;
}

void MainWindow::selectAll() {
    ListView_SetItemState(list_, -1, LVIS_SELECTED, LVIS_SELECTED);
}

void MainWindow::refreshInstalled() {
    installed_.clear();
    if (!pm_) return;
    for (const auto& p : pm_->list()) installed_[p.packageName] = {p.versionCode, p.versionName};
}

void MainWindow::updateButtons() {
    bool canInstall = false, canUninstall = false;
    if (pm_ && !installing_) {
        for (int i : selectedRows()) {
            const auto& a = apps_[std::size_t(i)];
            canInstall |= a.ok();
            canUninstall |= a.ok() && installed_.count(a.packageName) > 0;
        }
    }
    EnableWindow(btnInstall_, canInstall);
    EnableWindow(btnUninstall_, canUninstall);
}

void MainWindow::installSelected() {
    if (!pm_ || installing_) return;
    std::vector<apkcore::ApkInfo> todo;
    for (int i : selectedRows())
        if (apps_[std::size_t(i)].ok()) todo.push_back(apps_[std::size_t(i)]);
    if (!todo.empty()) startInstall(std::move(todo), false);
}

void MainWindow::startInstall(std::vector<apkcore::ApkInfo> apks, bool allowDowngrade) {
    if (installer_.joinable()) installer_.join();
    installing_ = true;
    installCancel_ = false;
    updateButtons();

    HWND h = hwnd_;
    installer_ = std::thread([this, h, apks = std::move(apks), allowDowngrade]() mutable {
        auto batch = std::make_unique<InstallBatch>();
        for (std::size_t i = 0; i < apks.size() && !installCancel_; ++i) {
            PostMessageW(h, WM_APP_INSTALL_PROGRESS, i, static_cast<LPARAM>(apks.size()));
            batch->results.push_back(pm_->install(apks[i], pkgmgr::InstallOptions{allowDowngrade}));
        }
        apks.resize(batch->results.size());
        batch->apks = std::move(apks);
        if (PostMessageW(h, WM_APP_INSTALL_DONE, 0, reinterpret_cast<LPARAM>(batch.get())))
            batch.release();  // ownership passes to the UI thread
    });
}

void MainWindow::onInstallDone(InstallBatch batch) {
    if (installer_.joinable()) installer_.join();
    installing_ = false;

    std::size_t added = 0, updated = 0, unchanged = 0;
    std::wstring errors;
    std::vector<apkcore::ApkInfo> refused;
    for (std::size_t i = 0; i < batch.results.size(); ++i) {
        const auto& r = batch.results[i];
        switch (r.status) {
        case pkgmgr::InstallStatus::Installed: ++added; break;
        case pkgmgr::InstallStatus::Updated:
        case pkgmgr::InstallStatus::Reinstalled: ++updated; break;
        case pkgmgr::InstallStatus::AlreadyInstalled: ++unchanged; break;
        case pkgmgr::InstallStatus::DowngradeRefused: refused.push_back(std::move(batch.apks[i])); break;
        default:
            errors += L"• " + widen(batch.apks[i].displayName()) + L": " + widen(pkgmgr::toString(r.status)) +
                      (r.message.empty() ? L"" : L" — " + widen(r.message)) + L"\n";
        }
    }

    // Row order is unchanged, so the selection survives the repopulate.
    const std::vector<int> selection = selectedRows();
    refreshInstalled();
    populate();
    for (int i : selection) ListView_SetItemState(list_, i, LVIS_SELECTED, LVIS_SELECTED);
    updateButtons();

    wchar_t buf[160];
    swprintf_s(buf, L"Install finished: %zu new, %zu updated, %zu already installed, %zu failed  |  %zu app(s) installed",
               added, updated, unchanged, batch.results.size() - added - updated - unchanged - refused.size(),
               installed_.size());
    setStatus(buf);

    if (!errors.empty())
        MessageBoxW(hwnd_, (L"Some APKs could not be installed:\n\n" + errors).c_str(), L"Install", MB_OK | MB_ICONWARNING);
    if (!refused.empty()) {
        std::wstring q = L"A newer version is already installed for:\n\n";
        for (const auto& a : refused) q += L"• " + widen(a.displayName()) + L" (" + widen(a.versionName) + L")\n";
        q += L"\nReplace it with this older version?";
        if (MessageBoxW(hwnd_, q.c_str(), L"Downgrade", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
            startInstall(std::move(refused), true);
    }
}

void MainWindow::uninstallSelected() {
    if (!pm_ || installing_) return;
    std::vector<std::string> packages;
    std::wstring names;
    for (int i : selectedRows()) {
        const auto& a = apps_[std::size_t(i)];
        if (!a.ok() || !installed_.count(a.packageName)) continue;
        if (std::find(packages.begin(), packages.end(), a.packageName) != packages.end()) continue;
        packages.push_back(a.packageName);
        names += L"• " + widen(a.displayName()) + L"\n";
    }
    if (packages.empty()) return;
    if (MessageBoxW(hwnd_, (L"Uninstall these apps?\n\n" + names).c_str(), L"Uninstall",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;

    std::wstring errors;
    for (const auto& pkg : packages) {
        std::string err;
        if (!pm_->uninstall(pkg, &err)) errors += L"• " + widen(pkg) + L": " + widen(err) + L"\n";
    }
    const std::vector<int> selection = selectedRows();
    refreshInstalled();
    populate();
    for (int i : selection) ListView_SetItemState(list_, i, LVIS_SELECTED, LVIS_SELECTED);
    updateButtons();
    setStatus(L"Uninstalled " + std::to_wstring(packages.size()) + L" app(s)  |  " +
              std::to_wstring(installed_.size()) + L" app(s) installed");
    if (!errors.empty())
        MessageBoxW(hwnd_, (L"Some apps could not be uninstalled:\n\n" + errors).c_str(), L"Uninstall",
                    MB_OK | MB_ICONWARNING);
}
