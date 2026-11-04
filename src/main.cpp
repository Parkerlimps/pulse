// Pulse —— 本地音乐播放器（C++ / Win32 + Direct2D + Media Foundation + WASAPI）
//
// 单进程，没有 WebView。解码交给 Media Foundation（系统自带，mp3/aac/flac/m4a/wav/wma 都支持），
// 输出走 WASAPI 共享模式，界面用 Direct2D 自绘，频谱是对解码出来的 PCM 做 FFT。
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "audio.h"
#include "library.h"
#include "resource.h"
#include "spectrum.h"
#include "ui.h"
#include "util.h"

namespace {

constexpr UINT_PTR TIMER_TICK = 1;

struct App {
    ui::View view;
    au::Player audio;
    dsp::Spectrum spec;
    std::vector<lib::Track> tracks;
    std::vector<std::string> folders;
    std::vector<std::string> excluded;  // 手动从曲库移除的文件（不删磁盘，可恢复）
    std::string dataDir, cfgPath, status, lastPath;
    int idx = -1;
    int mode = 0;
    float vol = 0.7f;
    bool mini = false;
    bool playing = false;
    int coverIdx = -2;
    HWND hwnd = nullptr;
    RECT saveRect{};
    LONG_PTR saveStyle = 0, saveExStyle = 0;
};

App g;

// ---------------- 配置 ----------------

void loadConfig() {
    g.cfgPath = g.dataDir + "\\config.txt";
    FILE* f = _wfopen(util::widen(g.cfgPath).c_str(), L"rb");
    if (!f) {
        std::string m;
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &p))) {
            m = util::narrow(p);
            CoTaskMemFree(p);
        }
        if (!m.empty() && util::dirExists(m)) g.folders.push_back(m);
        return;
    }
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        if (k == "volume") g.vol = (float)atof(v.c_str());
        else if (k == "last") g.idx = atoi(v.c_str());
        else if (k == "lastpath") g.lastPath = util::normalizePath(v);
        else if (k == "mode") g.mode = atoi(v.c_str());
        else if (k == "folder" && !v.empty()) {
            std::string d = util::normalizePath(v);
            bool dup = false;
            for (auto& x : g.folders)
                if (_stricmp(x.c_str(), d.c_str()) == 0) dup = true;
            if (!dup) g.folders.push_back(d);
        }
        else if (k == "exclude" && !v.empty()) {
            std::string d = util::normalizePath(v);
            bool dup = false;
            for (auto& x : g.excluded)
                if (_stricmp(x.c_str(), d.c_str()) == 0) dup = true;
            if (!dup) g.excluded.push_back(d);
        }
    }
    fclose(f);
    if (g.vol < 0 || g.vol > 1) g.vol = 0.7f;
}

void saveConfig() {
    std::string s;
    char buf[64];
    snprintf(buf, sizeof(buf), "volume=%.2f\n", g.vol);
    s += buf;
    snprintf(buf, sizeof(buf), "last=%d\n", g.idx);
    s += buf;
    snprintf(buf, sizeof(buf), "mode=%d\n", g.mode);
    s += buf;
    // 记住文件路径而不只是下标：移除一首歌之后，下标会指向别首
    std::string lp = (g.idx >= 0 && g.idx < (int)g.tracks.size())
                         ? util::normalizePath(g.tracks[g.idx].path)
                         : g.lastPath;
    if (!lp.empty()) s += "lastpath=" + lp + "\n";
    for (auto& d : g.folders) s += "folder=" + d + "\n";
    for (auto& d : g.excluded) s += "exclude=" + d + "\n";
    FILE* f = _wfopen(util::widen(g.cfgPath).c_str(), L"wb");
    if (f) {
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    }
}

// ---------------- 曲库 / 播放 ----------------

// Windows 路径大小写不敏感；统一分隔符后再比，避免同一文件被重复记录
bool samePath(const std::string& a, const std::string& b) {
    return _stricmp(util::normalizePath(a).c_str(), util::normalizePath(b).c_str()) == 0;
}

bool isExcluded(const std::string& path) {
    for (auto& e : g.excluded)
        if (samePath(e, path)) return true;
    return false;
}

void stopCurrent() {
    g.audio.close();
    g.playing = false;
    g.status.clear();
}

void rescan() {
    // 记住正在播的文件，扫完按路径找回下标——移除一个目录会让后面的下标整体前移
    bool hadCur = g.idx >= 0 && g.idx < (int)g.tracks.size();
    std::string cur = hadCur ? g.tracks[g.idx].path : "";

    auto all = lib::scan(g.folders);
    g.tracks.clear();
    g.tracks.reserve(all.size());
    for (auto& t : all)
        if (!isExcluded(t.path)) g.tracks.push_back(std::move(t));

    if (hadCur) {
        int k = -1;
        for (int i = 0; i < (int)g.tracks.size(); i++)
            if (samePath(cur, g.tracks[i].path)) { k = i; break; }
        if (k >= 0) {
            g.idx = k;
        } else {
            // 当前曲目被移走了：停播，选中相邻的一首但不自动播放
            stopCurrent();
            g.idx = g.tracks.empty() ? -1 : std::min(g.idx, (int)g.tracks.size() - 1);
        }
    } else if (g.idx >= (int)g.tracks.size()) {
        g.idx = (int)g.tracks.size() - 1;
    }
}

std::string readCover(const lib::Track& t) {
    if (t.coverLen <= 0 || t.coverLen > 8 * 1024 * 1024) return "";
    FILE* f = _wfopen(util::widen(t.path).c_str(), L"rb");
    if (!f) return "";
    std::string out((size_t)t.coverLen, '\0');
    _fseeki64(f, t.coverOff, SEEK_SET);
    size_t got = fread(&out[0], 1, (size_t)t.coverLen, f);
    fclose(f);
    if (got != (size_t)t.coverLen) return "";
    return out;
}

void loadCoverFor(int i) {
    if (i == g.coverIdx) return;
    g.coverIdx = i;
    if (i < 0 || i >= (int)g.tracks.size()) {
        g.view.clearCover();
        return;
    }
    auto bytes = readCover(g.tracks[i]);
    if (!bytes.empty() && g.view.setCover(bytes)) return;
    g.view.clearCover();
}

void refreshCover() {
    g.coverIdx = -2;  // 强制重载：曲库变了，原来的下标可能已经指向另一首
    loadCoverFor(g.idx);
}

void playAt(int i, bool autoplay) {
    if (i < 0 || i >= (int)g.tracks.size()) return;
    g.idx = i;
    g.status.clear();
    std::string err;
    if (!g.audio.open(g.tracks[i].path, &err)) {
        g.status = err;
        g.playing = false;
        g.coverIdx = -2;
        loadCoverFor(i);
        return;
    }
    g.audio.setVolume(g.vol);
    if (autoplay) {
        g.audio.play();
        g.playing = true;
    } else {
        g.playing = false;
    }
    g.coverIdx = -2;
    loadCoverFor(i);
    g.view.keepVisible(i, (int)g.tracks.size());
    saveConfig();
}

void nextTrack(bool fromEnd) {
    int n = (int)g.tracks.size();
    if (n == 0) return;
    if (fromEnd && g.mode == 1) {
        g.audio.seek(0);
        g.audio.play();
        g.playing = true;
        return;
    }
    int k;
    if (g.mode == 2 && n > 1) {
        do { k = rand() % n; } while (k == g.idx);
    } else {
        k = g.idx + 1;
        if (k >= n) k = 0;
    }
    playAt(k, true);
}

void prevTrack() {
    int n = (int)g.tracks.size();
    if (n == 0) return;
    if (g.audio.pos() > 3.0) {
        g.audio.seek(0);
        return;
    }
    int k = g.idx - 1;
    if (k < 0) k = n - 1;
    playAt(k, true);
}

void togglePlay() {
    if (g.idx < 0) {
        if (!g.tracks.empty()) playAt(0, true);
        return;
    }
    if (g.audio.dur() <= 0) {
        playAt(g.idx, true);
        return;
    }
    if (g.audio.playing()) {
        g.audio.pause();
        g.playing = false;
    } else {
        g.audio.play();
        g.playing = true;
    }
}

// 文件夹选择框必须跑在 STA 线程上：
// 本进程是 MTA（WASAPI 的 COM 对象要跨线程用，不能改成 STA），
// 而 SHBrowseForFolder / IFileOpenDialog 依赖 STA，在 MTA 里会直接卡死且不弹窗。
static const CLSID kCLSID_FileOpenDialog = {
    0xDC1C5A9C, 0xE88A, 0x4DDE, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
static const IID kIID_IFileOpenDialog = {
    0xD57C7288, 0xD4AD, 0x4768, {0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60}};

struct PickJob {
    HWND owner = nullptr;
    std::atomic<bool> done{false};
    std::wstring path;
};
PickJob* g_pick = nullptr;

void startPickFolder() {
    if (g_pick) return;
    g_pick = new PickJob();
    g_pick->owner = g.hwnd;
    PickJob* job = g_pick;
    HANDLE th = CreateThread(
        nullptr, 0,
        [](LPVOID p) -> DWORD {
            auto* j = (PickJob*)p;
            if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
                IFileOpenDialog* dlg = nullptr;
                if (SUCCEEDED(CoCreateInstance(kCLSID_FileOpenDialog, nullptr,
                                               CLSCTX_INPROC_SERVER, kIID_IFileOpenDialog,
                                               (void**)&dlg)) &&
                    dlg) {
                    DWORD opts = 0;
                    dlg->GetOptions(&opts);
                    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                    FOS_PATHMUSTEXIST);
                    dlg->SetTitle(L"选择音乐文件夹");
                    if (SUCCEEDED(dlg->Show(j->owner))) {
                        IShellItem* item = nullptr;
                        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                            PWSTR path = nullptr;
                            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                                j->path = path;
                                CoTaskMemFree(path);
                            }
                            item->Release();
                        }
                    }
                    dlg->Release();
                }
                CoUninitialize();
            }
            j->done = true;
            return 0;
        },
        job, 0, nullptr);
    if (th) CloseHandle(th);
}

// 加入曲库目录（去重）。选择器和命令行参数走同一条路
bool applyFolder(const std::string& raw) {
    std::string d = util::normalizePath(raw);
    if (d.empty() || !util::dirExists(d)) return false;
    for (auto& x : g.folders)
        if (_stricmp(x.c_str(), d.c_str()) == 0) return false;
    g.folders.push_back(d);
    saveConfig();
    rescan();
    if (g.idx < 0 && !g.tracks.empty()) g.idx = 0;
    refreshCover();
    InvalidateRect(g.hwnd, nullptr, FALSE);
    return true;
}

// 在 tick 里收结果，不阻塞 UI 线程
void pollPickFolder() {
    if (!g_pick || !g_pick->done.load()) return;
    std::wstring path;
    {
        PickJob* job = g_pick;
        g_pick = nullptr;
        path = job->path;
        delete job;
    }
    if (path.empty()) return;
    applyFolder(util::narrow(path));
}

// ---------------- 移除 / 恢复 ----------------
// 只改配置和内存里的曲库，绝不删磁盘上的音频文件；移错了随时能加回来。

void removeFolder(int i) {
    if (i < 0 || i >= (int)g.folders.size()) return;
    g.folders.erase(g.folders.begin() + i);
    rescan();
    refreshCover();
    saveConfig();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void removeTrack(int row) {
    if (row < 0 || row >= (int)g.tracks.size()) return;
    std::string p = util::normalizePath(g.tracks[row].path);
    if (!isExcluded(p)) g.excluded.push_back(p);
    bool wasCur = (row == g.idx);
    g.tracks.erase(g.tracks.begin() + row);
    if (row < g.idx) {
        g.idx--;  // 前面的行没了，当前曲目整体前移
    } else if (wasCur) {
        stopCurrent();  // 正在播的那首被移除：停下，选中相邻一首但不自动播放
        if (g.idx >= (int)g.tracks.size()) g.idx = (int)g.tracks.size() - 1;
    }
    refreshCover();
    saveConfig();
    g.view.keepVisible(g.idx < 0 ? 0 : g.idx, (int)g.tracks.size());
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void restoreTrack(int i) {
    if (i < 0 || i >= (int)g.excluded.size()) return;
    g.excluded.erase(g.excluded.begin() + i);
    rescan();
    refreshCover();
    saveConfig();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void clearExcluded() {
    if (g.excluded.empty()) return;
    g.excluded.clear();
    rescan();
    refreshCover();
    saveConfig();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ---------------- 迷你模式 ----------------

int dpiOf() {
    HDC dc = GetDC(nullptr);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d;
}

void resizeViewToClient(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    g.view.resize(rc.right, rc.bottom, (float)GetDpiForWindow(hwnd));
}

void enterMini(HWND hwnd) {
    if (g.mini) return;
    GetWindowRect(hwnd, &g.saveRect);
    g.saveStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    g.saveExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    int dpi = dpiOf();
    int w = MulDiv((int)ui::MINI_W, dpi, 96), h = MulDiv((int)ui::MINI_H, dpi, 96);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    ShowWindow(hwnd, SW_RESTORE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN);
    // 不能用 WS_EX_TOOLWINDOW：它会把窗口从任务栏和 Alt+Tab 里摘掉，而迷你条
    // 一旦被别的窗口挡住或失焦，键盘 Esc/M 也收不到，程序就真的找不回来了。
    // APPWINDOW 让任务栏按钮在迷你态也一直保留，随时能把小条叫回前台。
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_APPWINDOW);
    SetWindowPos(hwnd, HWND_TOPMOST, sw - w - MulDiv(28, dpi, 96), sh - h - MulDiv(72, dpi, 96),
                 w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    g.mini = true;
    resizeViewToClient(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void exitMini(HWND hwnd) {
    if (!g.mini) return;
    SetWindowLongPtrW(hwnd, GWL_STYLE, g.saveStyle);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, g.saveExStyle);
    SetWindowPos(hwnd, HWND_NOTOPMOST, g.saveRect.left, g.saveRect.top,
                 g.saveRect.right - g.saveRect.left, g.saveRect.bottom - g.saveRect.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    g.mini = false;
    resizeViewToClient(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
    SetForegroundWindow(hwnd);
}

// ---------------- 每帧 ----------------

ui::Model buildModel() {
    ui::Model m;
    m.tracks = &g.tracks;
    m.idx = g.idx;
    m.mode = g.mode;
    m.playing = g.playing;
    m.mini = g.mini;
    m.pos = g.audio.pos();
    m.dur = g.audio.dur();
    m.vol = g.vol;
    m.spec = &g.spec;
    m.status = g.status;
    m.folders = &g.folders;
    m.excluded = &g.excluded;
    return m;
}

void applyPending() {
    float r = 0;
    if (g.view.takeSeekRatio(&r)) {
        double d = g.audio.dur();
        if (d > 0) g.audio.seek(d * r);
    }
    if (g.view.takeVolume(&r)) {
        g.vol = r;
        g.audio.setVolume(r);
        saveConfig();
    }
}

void handleActions() {
    for (auto& a : g.view.takeActions()) {
        switch (a.act) {
            case ui::A_PREV: prevTrack(); break;
            case ui::A_PLAY: togglePlay(); break;
            case ui::A_NEXT: nextTrack(false); break;
            case ui::A_MODE:
                g.mode = (g.mode + 1) % 3;
                saveConfig();
                break;
            case ui::A_MINI: enterMini(g.hwnd); break;
            case ui::A_RESTORE: exitMini(g.hwnd); break;
            case ui::A_CLOSE: PostMessageW(g.hwnd, WM_CLOSE, 0, 0); break;
            case ui::A_ADD: startPickFolder(); break;
            case ui::A_RESCAN: rescan(); refreshCover(); break;
            case ui::A_ROW: playAt(a.row, true); break;
            case ui::A_REMOVE_FOLDER: removeFolder(a.row); break;
            case ui::A_REMOVE_TRACK: removeTrack(a.row); break;
            case ui::A_RESTORE_TRACK: restoreTrack(a.row); break;
            case ui::A_CLEAR_EXCLUDED: clearExcluded(); break;
            case ui::A_DRAG:
                ReleaseCapture();
                SendMessageW(g.hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                break;
            default: break;
        }
    }
}

void tick() {
    static float mono[4096];
    static double last = 0;
    double now = util::nowSec();
    float dt = last > 0 ? (float)(now - last) : 0.033f;
    if (dt > 0.2f) dt = 0.2f;
    last = now;

    if (g.playing) {
        int n = g.audio.readMono(mono, 4096);
        if (n > 0) g.spec.analyze(mono, n, g.audio.rate(), dt);
        else g.spec.decay(dt);
    } else {
        g.spec.decay(dt);
    }

    // 一首放完：等缓冲也空了再切，免得尾巴被掐掉
    if (g.playing && g.audio.ended() && g.audio.drained()) nextTrack(true);

    applyPending();
    handleActions();
    pollPickFolder();

    bool animating = g.playing;
    if (!animating) {
        for (int i = 0; i < dsp::kBars; i++)
            if (g.spec.levels[i] > 0.004f) {
                animating = true;
                break;
            }
    }
    if (animating) InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ---------------- 窗口 ----------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                resizeViewToClient(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_DPICHANGED: {
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            resizeViewToClient(hwnd);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            g.view.paint(buildModel());
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            g.view.mouseMove(p);
            applyPending();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            g.view.mouseDown(p);
            applyPending();
            handleActions();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP:
            g.view.mouseUp();
            applyPending();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            g.view.wheel(GET_WHEEL_DELTA_WPARAM(wp));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_MINIMIZE) {
                enterMini(hwnd);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            switch (wp) {
                case VK_SPACE: togglePlay(); break;
                case 'M': g.mini ? exitMini(hwnd) : enterMini(hwnd); break;
                case VK_ESCAPE:
                    if (g.mini) exitMini(hwnd);
                    break;
                case VK_LEFT: g.audio.seek(g.audio.pos() - 5); break;
                case VK_RIGHT: g.audio.seek(g.audio.pos() + 5); break;
                case VK_UP:
                    g.vol = g.vol + 0.05f > 1.0f ? 1.0f : g.vol + 0.05f;
                    g.audio.setVolume(g.vol);
                    saveConfig();
                    break;
                case VK_DOWN:
                    g.vol = g.vol - 0.05f < 0 ? 0 : g.vol - 0.05f;
                    g.audio.setVolume(g.vol);
                    saveConfig();
                    break;
                case VK_NEXT: nextTrack(false); break;
                case VK_PRIOR: prevTrack(); break;
                default: break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_TICK) tick();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            int dpi = dpiOf();
            mm->ptMinTrackSize.x = MulDiv(780, dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(520, dpi, 96);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // COM 用 MTA：解码、WASAPI、WIC、D2D 分布在不同线程，MTA 省掉封送
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g.dataDir = util::localAppData() + "\\Pulse";
    util::makeDirs(g.dataDir);
    loadConfig();

    // 支持把文件夹拖到 exe 上（等价于「添加文件夹」）
    int argc = 0;
    bool dirty = false;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; i++) {
            std::string d = util::normalizePath(util::narrow(argv[i]));
            if (util::dirExists(d)) {
                bool dup = false;
                for (auto& x : g.folders)
                    if (_stricmp(x.c_str(), d.c_str()) == 0) dup = true;
                if (!dup) {
                    g.folders.push_back(d);
                    dirty = true;
                }
            }
        }
        LocalFree(argv);
        if (dirty) saveConfig();
    }
    rescan();
    g.spec.init();
    if (!g.lastPath.empty()) {
        for (int i = 0; i < (int)g.tracks.size(); i++)
            if (samePath(g.lastPath, g.tracks[i].path)) { g.idx = i; break; }
    }
    if (g.idx >= (int)g.tracks.size()) g.idx = -1;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(11, 12, 16));
    wc.lpszClassName = L"PulseWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    int dpi = dpiOf();
    int w = MulDiv(940, dpi, 96), h = MulDiv(640, dpi, 96);
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(0, L"PulseWnd", L"Pulse", WS_OVERLAPPEDWINDOW, (sx - w) / 2,
                                std::max(0, (sy - h) / 2 - 40), w, h, nullptr, nullptr, hInst,
                                nullptr);
    if (!hwnd) return 1;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    COLORREF cap = RGB(11, 12, 16), txt = RGB(242, 244, 248), bd = RGB(24, 26, 33);
    DwmSetWindowAttribute(hwnd, 35, &cap, sizeof(cap));
    DwmSetWindowAttribute(hwnd, 36, &txt, sizeof(txt));
    DwmSetWindowAttribute(hwnd, 34, &bd, sizeof(bd));
    int corner = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));

    if (!g.view.init(hwnd)) {
        MessageBoxW(hwnd, L"Direct2D 初始化失败", L"Pulse", MB_ICONERROR);
        return 1;
    }
    resizeViewToClient(hwnd);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (g.idx >= 0 && g.idx < (int)g.tracks.size()) {
        loadCoverFor(g.idx);
        g.view.keepVisible(g.idx, (int)g.tracks.size());
    }
    g.audio.setVolume(g.vol);
    SetTimer(hwnd, TIMER_TICK, 16, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(hwnd, TIMER_TICK);
    g.audio.close();
    saveConfig();
    CoUninitialize();
    return 0;
}
