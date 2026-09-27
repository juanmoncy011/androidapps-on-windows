#pragma once
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <apkcore/apk.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

class MainWindow {
public:
    MainWindow() = default;
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool create(HINSTANCE instance, int showCommand, std::filesystem::path initialFolder);
    void startScan();
    void chooseFolder();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void onCreate();
    void createFont();
    void layout();
    LRESULT onNotify(NMHDR* nm);
    void onDrop(HDROP drop);
    void onScanDone(std::vector<apkcore::ApkInfo> apps);
    void setFolder(const std::filesystem::path& folder);
    void stopWorker();
    void buildIcons();
    void applySort();
    void sortBy(int column);
    void updateSortArrow();
    void populate();
    void showDetails(const apkcore::ApkInfo& app);
    void setStatus(const std::wstring& text);
    int scale(int v) const { return MulDiv(v, int(dpi_), 96); }

    HINSTANCE inst_{};
    HWND hwnd_{}, list_{}, btnOpen_{}, btnRescan_{}, chkRecursive_{}, lblFolder_{}, status_{};
    HFONT font_{};
    HIMAGELIST images_{};
    UINT dpi_ = 96;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;

    std::filesystem::path folder_;
    std::vector<apkcore::ApkInfo> apps_;
    std::vector<int> iconIndex_;
    int sortColumn_ = 0;
    bool sortAscending_ = true;

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::chrono::steady_clock::time_point scanStart_;
};
