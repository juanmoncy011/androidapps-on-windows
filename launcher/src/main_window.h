#pragma once
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <apkcore/apk.h>
#include <pkgmgr/package_manager.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
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
    void installSelected();
    void selectAll();

    struct InstalledVersion {
        std::int64_t versionCode = 0;
        std::string versionName;
    };
    using InstalledMap = std::map<std::string, InstalledVersion>;  // package -> installed version

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

    // Package Manager (M2) integration
    struct InstallBatch {
        std::vector<apkcore::ApkInfo> apks;
        std::vector<pkgmgr::InstallResult> results;
    };
    std::vector<int> selectedRows() const;
    void refreshInstalled();
    void startInstall(std::vector<apkcore::ApkInfo> apks, bool allowDowngrade);
    void onInstallDone(InstallBatch batch);
    void uninstallSelected();
    void updateButtons();
    int scale(int v) const { return MulDiv(v, int(dpi_), 96); }

    HINSTANCE inst_{};
    HWND hwnd_{}, list_{}, btnOpen_{}, btnRescan_{}, chkRecursive_{}, btnInstall_{}, btnUninstall_{}, lblFolder_{},
        status_{};
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

    std::unique_ptr<pkgmgr::PackageManager> pm_;  // null if the store could not be opened
    std::wstring pmError_;
    InstalledMap installed_;
    std::thread installer_;
    std::atomic<bool> installCancel_{false};
    bool installing_ = false;
};
