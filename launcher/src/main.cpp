#include "main_window.h"

#include <objbase.h>
#include <shellapi.h>

// Usage: AowLauncher.exe [folder]   (defaults to Downloads)
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    std::filesystem::path initial;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1) initial = argv[1];
        LocalFree(argv);
    }

    int exitCode = 0;
    {
        MainWindow window;
        if (!window.create(instance, showCommand, initial)) {
            CoUninitialize();
            return 1;
        }
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_F5) { window.startScan(); continue; }
                if (msg.wParam == 'O' && GetKeyState(VK_CONTROL) < 0) { window.chooseFolder(); continue; }
                if (msg.wParam == 'I' && GetKeyState(VK_CONTROL) < 0) { window.installSelected(); continue; }
                if (msg.wParam == 'L' && GetKeyState(VK_CONTROL) < 0) { window.launchSelected(); continue; }
                if (msg.wParam == 'A' && GetKeyState(VK_CONTROL) < 0) { window.selectAll(); continue; }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = int(msg.wParam);
    }
    CoUninitialize();
    return exitCode;
}
