// ui.h —— Direct2D 自绘界面：布局、绘制、命中测试
//
// 只负责画和收集点击，不碰播放状态：main 每帧把 Model 递进来，
// 事件里收集到的动作由 takeActions() 取走。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "library.h"
#include "spectrum.h"

namespace ui {

enum Act {
    A_NONE = 0,
    A_PREV, A_PLAY, A_NEXT, A_MODE, A_MINI, A_RESTORE, A_CLOSE,
    A_ADD, A_RESCAN, A_ROW, A_DRAG,
    A_REMOVE_FOLDER, A_REMOVE_TRACK, A_RESTORE_TRACK, A_CLEAR_EXCLUDED,
    A_DEV_PANEL, A_DEV_SELECT,
};

// 命中 id 分段：曲库行数不固定，删除按钮用远离 100+i 的段，避免大曲库撞号
constexpr int ID_TRACK_DEL = 100000;
constexpr int ID_FOLDER_DEL = 200000;
constexpr int ID_RESTORE = 300000;
constexpr int ID_DEV = 400000;

struct Action {
    int act = A_NONE;
    int row = -1;
};

struct Model {
    const std::vector<lib::Track>* tracks = nullptr;
    int idx = -1;      // 当前曲目在 tracks 里的下标
    int mode = 0;      // 0 顺序 1 单曲 2 随机
    bool playing = false;
    bool mini = false;
    double pos = 0, dur = 0;
    float vol = 0.7f;
    const dsp::Spectrum* spec = nullptr;
    std::string status;  // 出错时显示在副标题位置
    const std::vector<std::string>* folders = nullptr;   // 已添加的目录
    const std::vector<std::string>* excluded = nullptr;  // 已移除的曲目路径
    const std::vector<std::string>* devices = nullptr;   // 可选输出端点
    int devIndex = 0;
    std::string deviceName;                              // 当前端点短名
};

static const D2D1_COLOR_F C_BG = D2D1::ColorF(0.043f, 0.047f, 0.063f);
static const D2D1_COLOR_F C_LINE = D2D1::ColorF(0.137f, 0.149f, 0.184f);
static const D2D1_COLOR_F C_TEXT = D2D1::ColorF(0.949f, 0.957f, 0.973f);
static const D2D1_COLOR_F C_MUTED = D2D1::ColorF(0.541f, 0.565f, 0.627f);
static const D2D1_COLOR_F C_FAINT = D2D1::ColorF(0.365f, 0.388f, 0.451f);
static const D2D1_COLOR_F C_ACCENT = D2D1::ColorF(1.0f, 0.369f, 0.478f);
static const D2D1_COLOR_F C_ACCENT2 = D2D1::ColorF(1.0f, 0.729f, 0.478f);

constexpr float COVER = 188.0f;
constexpr float ROW_H = 46.0f;
constexpr float NOW_H = 286.0f;
constexpr float MINI_W = 340.0f;
constexpr float MINI_H = 124.0f;

inline std::wstring w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring o((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &o[0], n);
    return o;
}

inline std::wstring fileName(const std::string& p) {
    size_t a = p.find_last_of("\\/");
    return w(a == std::string::npos ? p : p.substr(a + 1));
}

class View {
public:
    bool init(HWND hwnd);
    void resize(UINT w, UINT h, float dpi);
    void paint(const Model& m);

    void mouseMove(POINT p);
    void mouseDown(POINT p);
    void mouseUp();
    void wheel(int delta);

    bool takeSeekRatio(float* out) {
        if (pendingSeekRatio_ < 0) return false;
        *out = pendingSeekRatio_;
        pendingSeekRatio_ = -1;
        return true;
    }
    bool takeVolume(float* out) {
        if (pendingVol_ < 0) return false;
        *out = pendingVol_;
        pendingVol_ = -1;
        return true;
    }
    std::vector<Action> takeActions() { return std::move(actions_); }
    void clearActions() { actions_.clear(); }

    bool setCover(const std::string& bytes);
    void clearCover();
    void keepVisible(int row, int totalRows);
    int scroll() const { return (int)scroll_; }
    bool closeDevicePanel() {
        if (!devPanel_) return false;
        devPanel_ = false;
        return true;
    }

private:
    int hitTest(POINT p) const;
    void addHot(int id, const D2D1_RECT_F& r) { hots_.push_back({id, r}); }
    void createRT();
    bool buildIcons();

    void fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c);
    void fillRound(const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c);
    void strokeRound(const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c, float sw);
    void circle(float cx, float cy, float rad, const D2D1_COLOR_F& c);
    void text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r,
              const D2D1_COLOR_F& c, DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING,
              float padLeft = 0);
    void icon(ID2D1PathGeometry* g, float x, float y, float size, const D2D1_COLOR_F& c);
    void spectrum(const dsp::Spectrum& sp, const D2D1_RECT_F& r, int bars, bool playing);
    void drawCover(const D2D1_RECT_F& r, float rad, bool has);
    void paintFull(const Model& m);
    void paintMini(const Model& m);
    void paintTrackList(const Model& m, float top, float bottom);
    void paintFolderPanel(const Model& m, float top, float bottom);
    void paintDevicePanel(const Model& m);

    HWND hwnd_ = nullptr;
    float dpi_ = 96.0f, W_ = 940, H_ = 640;

    ID2D1Factory* fac_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    IWICImagingFactory* wic_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1LinearGradientBrush* grad_ = nullptr;
    ID2D1Bitmap* cover_ = nullptr;
    ID2D1RoundedRectangleGeometry* coverClip_ = nullptr;
    ID2D1Layer* layer_ = nullptr;

    IDWriteTextFormat *fTitle_ = nullptr, *fSub_ = nullptr, *fRow_ = nullptr, *fRowSub_ = nullptr,
                      *fSmall_ = nullptr, *fBtn_ = nullptr, *fTime_ = nullptr, *fBig_ = nullptr;

    ID2D1PathGeometry *gPlay_ = nullptr, *gPause_ = nullptr, *gPrev_ = nullptr, *gNext_ = nullptr;

    int hover_ = 0, active_ = 0;
    bool dragSeek_ = false, dragVol_ = false;
    float pendingSeekRatio_ = -1;
    float pendingVol_ = -1;
    float scroll_ = 0;
    float panelScroll_ = 0;
    bool panel_ = false;     // 文件夹管理面板是否展开
    bool devPanel_ = false;  // 输出设备浮层是否展开
    int lastTotal_ = 0;
    std::vector<Action> actions_;
    std::vector<std::pair<int, D2D1_RECT_F>> hots_;
};

// ---------------- 初始化 ----------------

inline bool View::init(HWND hwnd) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &fac_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&dw_)))
        return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, (void**)&wic_)))
        return false;

    auto mk = [&](IDWriteTextFormat** f, float size, DWRITE_FONT_WEIGHT wt) {
        if (FAILED(dw_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, wt, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", f)))
            return false;
        (*f)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject* ell = nullptr;
        dw_->CreateEllipsisTrimmingSign(*f, &ell);
        (*f)->SetTrimming(&tr, ell);
        if (ell) ell->Release();
        (*f)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return true;
    };
    if (!mk(&fTitle_, 21.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) return false;
    if (!mk(&fSub_, 12.5f, DWRITE_FONT_WEIGHT_NORMAL)) return false;
    if (!mk(&fRow_, 13.0f, DWRITE_FONT_WEIGHT_NORMAL)) return false;
    if (!mk(&fRowSub_, 11.5f, DWRITE_FONT_WEIGHT_NORMAL)) return false;
    if (!mk(&fSmall_, 11.5f, DWRITE_FONT_WEIGHT_NORMAL)) return false;
    if (!mk(&fBtn_, 12.5f, DWRITE_FONT_WEIGHT_NORMAL)) return false;
    if (!mk(&fTime_, 11.0f, DWRITE_FONT_WEIGHT_NORMAL)) return false;
    if (!mk(&fBig_, 54.0f, DWRITE_FONT_WEIGHT_NORMAL)) return false;
    if (!buildIcons()) return false;
    createRT();
    return rt_ != nullptr;
}

// 图标是 24x24 的逻辑坐标，绘制时按尺寸缩放
inline bool View::buildIcons() {
    auto poly = [&](std::initializer_list<D2D1_POINT_2F> pts, ID2D1PathGeometry** out) {
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(fac_->CreatePathGeometry(out))) return false;
        (*out)->Open(&sink);
        sink->BeginFigure(*pts.begin(), D2D1_FIGURE_BEGIN_FILLED);
        bool first = true;
        for (auto& p : pts) {
            if (first) { first = false; continue; }
            sink->AddLine(p);
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();
        return true;
    };
    if (!poly({{8.f, 5.f}, {8.f, 19.f}, {19.f, 12.f}}, &gPlay_)) return false;
    if (!poly({{5.f, 5.f}, {5.f, 19.f}, {15.f, 12.f}}, &gNext_)) return false;

    // 暂停：两根竖条
    if (FAILED(fac_->CreatePathGeometry(&gPause_))) return false;
    {
        ID2D1GeometrySink* sink = nullptr;
        gPause_->Open(&sink);
        sink->BeginFigure(D2D1::Point2F(7.5f, 5.f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(10.9f, 5.f));
        sink->AddLine(D2D1::Point2F(10.9f, 19.f));
        sink->AddLine(D2D1::Point2F(7.5f, 19.f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->BeginFigure(D2D1::Point2F(13.1f, 5.f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(16.5f, 5.f));
        sink->AddLine(D2D1::Point2F(16.5f, 19.f));
        sink->AddLine(D2D1::Point2F(13.1f, 19.f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();
    }
    // 上一首：竖条 + 三角（另一个方向）
    if (FAILED(fac_->CreatePathGeometry(&gPrev_))) return false;
    {
        ID2D1GeometrySink* sink = nullptr;
        gPrev_->Open(&sink);
        sink->BeginFigure(D2D1::Point2F(6.f, 5.f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(8.6f, 5.f));
        sink->AddLine(D2D1::Point2F(8.6f, 19.f));
        sink->AddLine(D2D1::Point2F(6.f, 19.f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->BeginFigure(D2D1::Point2F(19.f, 5.f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(19.f, 19.f));
        sink->AddLine(D2D1::Point2F(9.5f, 12.f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();
    }
    return true;
}

inline void View::createRT() {
    if (rt_) { rt_->Release(); rt_ = nullptr; }
    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
    auto props = D2D1::RenderTargetProperties();
    props.dpiX = dpi_;
    props.dpiY = dpi_;
    fac_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, size), &rt_);
    if (!rt_) return;
    rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt_->CreateSolidColorBrush(C_TEXT, &brush_);

    D2D1_GRADIENT_STOP stops[3] = {
        {0.0f, C_ACCENT}, {0.5f, D2D1::ColorF(1.0f, 0.541f, 0.42f)}, {1.0f, C_ACCENT2}};
    ID2D1GradientStopCollection* col = nullptr;
    rt_->CreateGradientStopCollection(stops, 3, &col);
    if (col) {
        rt_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(900, 0)), col,
            &grad_);
        col->Release();
    }
    if (coverClip_) { coverClip_->Release(); coverClip_ = nullptr; }
    fac_->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(D2D1::RectF(0, 0, COVER, COVER), 14.f, 14.f), &coverClip_);
    if (layer_) { layer_->Release(); layer_ = nullptr; }
    rt_->CreateLayer(nullptr, &layer_);
    W_ = (float)(rc.right - rc.left) * 96.0f / dpi_;
    H_ = (float)(rc.bottom - rc.top) * 96.0f / dpi_;
}

inline void View::resize(UINT w, UINT h, float dpi) {
    dpi_ = dpi;
    W_ = (float)w * 96.0f / dpi;
    H_ = (float)h * 96.0f / dpi;
    if (rt_) {
        // 渲染目标是按 DIP 画的，必须跟窗口 DPI 对齐，否则内容只会占左上角那一块
        rt_->SetDpi(dpi_, dpi_);
        rt_->Resize(D2D1::SizeU(w, h));
    } else {
        createRT();
    }
}

// ---------------- 小工具 ----------------

inline void View::fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    brush_->SetColor(c);
    rt_->FillRectangle(r, brush_);
}

inline void View::fillRound(const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c) {
    brush_->SetColor(c);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_);
}

inline void View::strokeRound(const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c, float sw) {
    brush_->SetColor(c);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_, sw);
}

inline void View::circle(float cx, float cy, float rad, const D2D1_COLOR_F& c) {
    brush_->SetColor(c);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), brush_);
}

inline void View::text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& c, DWRITE_TEXT_ALIGNMENT a, float) {
    if (s.empty()) return;
    f->SetTextAlignment(a);
    brush_->SetColor(c);
    rt_->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, brush_,
                   D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

inline void View::icon(ID2D1PathGeometry* g, float x, float y, float size,
                       const D2D1_COLOR_F& c) {
    if (!g) return;
    brush_->SetColor(c);
    float k = size / 24.0f;
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(x, y));
    rt_->FillGeometry(g, brush_);
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
}

inline void View::drawCover(const D2D1_RECT_F& r, float rad, bool has) {
    if (has && cover_ && coverClip_) {
        // 圆角遮罩裁切
        auto rr = D2D1::RoundedRect(r, rad, rad);
        ID2D1RoundedRectangleGeometry* g = nullptr;
        fac_->CreateRoundedRectangleGeometry(rr, &g);
        if (g) {
            rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g), layer_);
            rt_->DrawBitmap(cover_, r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt_->PopLayer();
            g->Release();
        } else {
            rt_->DrawBitmap(cover_, r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        strokeRound(r, rad, D2D1::ColorF(1, 1, 1, 0.07f), 1.0f);
    } else {
        fillRound(r, rad, D2D1::ColorF(0.10f, 0.11f, 0.145f));
        text(L"\u266A", fBig_, r, D2D1::ColorF(1, 1, 1, 0.18f), DWRITE_TEXT_ALIGNMENT_CENTER);
        strokeRound(r, rad, D2D1::ColorF(1, 1, 1, 0.05f), 1.0f);
    }
}

inline void View::spectrum(const dsp::Spectrum& sp, const D2D1_RECT_F& r, int bars, bool playing) {
    float w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    grad_->SetStartPoint(D2D1::Point2F(r.left, 0));
    grad_->SetEndPoint(D2D1::Point2F(r.right, 0));

    if (!playing) {
        // 没在播就只留一条中性底线，别用彩色渐变——那看着像进度条的重影
        fill(D2D1::RectF(r.left, r.bottom - 2.5f, r.right, r.bottom - 0.5f),
             D2D1::ColorF(1, 1, 1, 0.06f));
        return;
    }

    float gap = bars > 36 ? 3.0f : 2.5f;
    float bw = (w - gap * (bars - 1)) / bars;
    if (bw < 2.0f) bw = 2.0f;
    float maxH = h - 5.0f;
    float base = r.bottom - 2.0f;

    for (int i = 0; i < bars; i++) {
        float lv = sp.levels[i];
        float bh = lv < 0.03f ? 1.2f : (lv * maxH > 2.5f ? lv * maxH : 2.5f);
        float x = r.left + i * (bw + gap);
        grad_->SetOpacity(lv < 0.03f ? 0.16f : 0.55f + 0.45f * (lv > 0.42f ? 1.0f : lv * 2.4f));
        float rad = bw * 0.5f < 3.0f ? bw * 0.5f : 3.0f;
        rt_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x, base - bh, x + bw, base), rad, rad), grad_);

        if (lv > 0.1f && sp.peaks[i] - lv > 0.05f) {
            float py = base - (sp.peaks[i] * maxH > 2.5f ? sp.peaks[i] * maxH : 2.5f) - 4.0f;
            if (py > r.top) {
                grad_->SetOpacity(0.38f);
                rt_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(x, py, x + bw, py + 2.0f), 1.0f, 1.0f), grad_);
            }
        }
    }
    grad_->SetOpacity(1.0f);
}

// ---------------- 绘制 ----------------

inline void View::paint(const Model& m) {
    if (!rt_) createRT();
    if (!rt_) return;
    hots_.clear();
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(C_BG);

    if (m.mini) paintMini(m);
    else paintFull(m);

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        if (brush_) { brush_->Release(); brush_ = nullptr; }
        if (grad_) { grad_->Release(); grad_ = nullptr; }
        if (layer_) { layer_->Release(); layer_ = nullptr; }
        clearCover();
        rt_->Release();
        rt_ = nullptr;
        createRT();
    }
}

inline std::wstring fmtTime(double sec) {
    if (sec < 0 || !(sec == sec)) sec = 0;
    int s = (int)sec;
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d:%02d", s / 60, s % 60);
    return buf;
}

inline void View::paintFull(const Model& m) {
    const auto& tracks = *m.tracks;
    bool has = m.idx >= 0 && m.idx < (int)tracks.size();
    const lib::Track* cur = has ? &tracks[m.idx] : nullptr;

    // 封面
    D2D1_RECT_F coverR = D2D1::RectF(26, 24, 26 + COVER, 24 + COVER);
    drawCover(coverR, 14.0f, has);

    // 标题 / 副标题
    float mx = 26 + COVER + 28;
    D2D1_RECT_F titleR = D2D1::RectF(mx, 26, W_ - 26, 58);
    text(cur ? w(cur->title) : L"还没有在播放", fTitle_, titleR, C_TEXT);
    std::wstring sub = cur ? (w(cur->artist).empty() ? L"未知歌手" : w(cur->artist)) : L"从下面的曲库里选一首";
    if (cur && !cur->album.empty()) sub += L" · " + w(cur->album);
    // 错误信息优先占副标题位：原来画在 H_-3 到 H_+12，基本在窗口外，等于没提示
    text(m.status.empty() ? sub : w(m.status), fSub_, D2D1::RectF(mx, 60, W_ - 26, 82),
         m.status.empty() ? C_MUTED : C_ACCENT);

    // 频谱
    if (m.spec) spectrum(*m.spec, D2D1::RectF(mx, 96, W_ - 26, 160), 52, m.playing);

    // 进度
    double dur = m.dur > 0 ? m.dur : 0;
    double pos = m.pos < dur ? m.pos : dur;
    float ratio = dur > 0 ? (float)(pos / dur) : 0;
    D2D1_RECT_F barR = D2D1::RectF(mx + 42, 181, W_ - 26 - 42, 185);
    fillRound(barR, 2.0f, D2D1::ColorF(1, 1, 1, 0.08f));
    if (ratio > 0.001f) {
        D2D1_RECT_F fr = barR;
        fr.right = barR.left + (barR.right - barR.left) * ratio;
        grad_->SetStartPoint(D2D1::Point2F(barR.left, 0));
        grad_->SetEndPoint(D2D1::Point2F(barR.right, 0));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(fr, 2.0f, 2.0f), grad_);
    }
    if (hover_ == 7 || dragSeek_) circle(barR.left + (barR.right - barR.left) * ratio, 183, 4.5f,
                                        D2D1::ColorF(1, 1, 1));
    addHot(7, D2D1::RectF(barR.left, 172, barR.right, 194));
    text(fmtTime(pos), fTime_, D2D1::RectF(mx, 176, mx + 38, 192), C_FAINT);
    text(fmtTime(dur), fTime_, D2D1::RectF(W_ - 26 - 38, 176, W_ - 26, 192), C_FAINT,
         DWRITE_TEXT_ALIGNMENT_TRAILING);

    // 左：上一首 / 播放 / 下一首
    struct Btn { int id; float x, y, s; ID2D1PathGeometry* g; };
    float cy = 210;
    D2D1_RECT_F pv = D2D1::RectF(mx, cy, mx + 34, cy + 34);
    D2D1_RECT_F pb = D2D1::RectF(mx + 46, cy - 6, mx + 46 + 46, cy + 40);
    D2D1_RECT_F nx = D2D1::RectF(mx + 104, cy, mx + 138, cy + 34);

    if (hover_ == 1) fillRound(pv, 9, D2D1::ColorF(1, 1, 1, 0.05f));
    if (hover_ == 3) fillRound(nx, 9, D2D1::ColorF(1, 1, 1, 0.05f));
    icon(gPrev_, pv.left + 5, pv.top + 5, 24, C_MUTED);
    icon(gNext_, nx.left + 5, nx.top + 5, 24, C_MUTED);

    // 播放键：渐变圆
    grad_->SetStartPoint(D2D1::Point2F(pb.left, pb.top));
    grad_->SetEndPoint(D2D1::Point2F(pb.right, pb.bottom));
    grad_->SetOpacity(0.96f);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((pb.left + pb.right) / 2, (pb.top + pb.bottom) / 2),
                                   23.0f, 23.0f),
                     grad_);
    grad_->SetOpacity(1.0f);
    if (m.playing) icon(gPause_, pb.left + 11, pb.top + 11, 24, D2D1::ColorF(1, 1, 1));
    else icon(gPlay_, pb.left + 11, pb.top + 11, 24, D2D1::ColorF(1, 1, 1));

    addHot(1, pv);
    addHot(2, pb);
    addHot(3, nx);

    // 右：模式 / 音量 / 迷你
    float rx = W_ - 26;
    D2D1_RECT_F miniB = D2D1::RectF(rx - 56, cy + 3, rx, cy + 31);
    if (hover_ == 6) fillRound(miniB, 8, D2D1::ColorF(1, 1, 1, 0.06f));
    text(L"迷你", fBtn_, miniB, hover_ == 6 ? C_TEXT : C_MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
    addHot(6, miniB);
    rx -= 56 + 16;

    D2D1_RECT_F volR = D2D1::RectF(rx - 76, cy + 15, rx, cy + 19);
    fillRound(volR, 2.0f, D2D1::ColorF(1, 1, 1, 0.08f));
    {
        D2D1_RECT_F fr = volR;
        fr.right = volR.left + (volR.right - volR.left) * (m.vol < 0 ? 0 : m.vol);
        fillRound(fr, 2.0f, D2D1::ColorF(1, 1, 1, 0.55f));
        if (hover_ == 5 || dragVol_)
            circle(fr.right, cy + 17, 4.0f, D2D1::ColorF(1, 1, 1));
    }
    addHot(5, D2D1::RectF(volR.left, volR.top - 8, volR.right, volR.bottom + 8));
    rx -= 76 + 20;

    D2D1_RECT_F modeB = D2D1::RectF(rx - 54, cy + 3, rx, cy + 31);
    if (hover_ == 4) fillRound(modeB, 8, D2D1::ColorF(1, 1, 1, 0.06f));
    const wchar_t* labels[3] = {L"顺序", L"单曲", L"随机"};
    text(labels[m.mode % 3], fBtn_, modeB, hover_ == 4 ? C_TEXT : C_MUTED,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    addHot(4, modeB);

    // 输出端点：点开浮层选设备
    rx -= 54 + 16;
    D2D1_RECT_F devB = D2D1::RectF(rx - 148, cy + 3, rx, cy + 31);
    if (hover_ == 14 || devPanel_) fillRound(devB, 8, D2D1::ColorF(1, 1, 1, 0.06f));
    text(w(m.deviceName), fBtn_, devB, (hover_ == 14 || devPanel_) ? C_TEXT : C_MUTED,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    addHot(14, devB);

    // 分隔线
    fill(D2D1::RectF(0, NOW_H - 1, W_, NOW_H), C_LINE);

    // 列表头
    int total = (int)tracks.size();
    text(L"曲库 " + std::to_wstring(total) + L" 首", fSmall_, D2D1::RectF(26, 294, 136, 322),
         C_FAINT);
    // 管理文件夹：展开后能移除误加的目录、恢复被移除的曲目
    D2D1_RECT_F manage = D2D1::RectF(140, 296, 236, 324);
    if (hover_ == 13) fillRound(manage, 8, D2D1::ColorF(1, 1, 1, 0.05f));
    strokeRound(manage, 8,
                hover_ == 13 ? D2D1::ColorF(1, 0.369f, 0.478f, 0.45f) : C_LINE, 1.0f);
    text(panel_ ? L"返回曲库" : L"管理文件夹", fBtn_, manage,
         hover_ == 13 ? C_ACCENT : C_MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
    addHot(13, manage);
    D2D1_RECT_F rescan = D2D1::RectF(W_ - 26 - 76, 296, W_ - 26, 324);
    D2D1_RECT_F add = D2D1::RectF(rescan.left - 8 - 92, 296, rescan.left - 8, 324);
    if (hover_ == 9) fillRound(rescan, 8, D2D1::ColorF(1, 1, 1, 0.05f));
    if (hover_ == 8) fillRound(add, 8, D2D1::ColorF(1, 1, 1, 0.05f));
    strokeRound(rescan, 8, hover_ == 9 ? D2D1::ColorF(1, 0.369f, 0.478f, 0.45f) : C_LINE, 1.0f);
    strokeRound(add, 8, hover_ == 8 ? D2D1::ColorF(1, 0.369f, 0.478f, 0.45f) : C_LINE, 1.0f);
    text(L"重新扫描", fBtn_, rescan, hover_ == 9 ? C_ACCENT : C_MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
    text(L"添加文件夹", fBtn_, add, hover_ == 8 ? C_ACCENT : C_MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
    addHot(9, rescan);
    addHot(8, add);

    // 列表 / 管理面板
    float listTop = 332, listBottom = H_ - 12;
    if (panel_) paintFolderPanel(m, listTop, listBottom);
    else paintTrackList(m, listTop, listBottom);

    // 浮层必须最后画，命中测试又是从后往前找，这样才盖得住下面的控件
    paintDevicePanel(m);
}

inline void View::paintTrackList(const Model& m, float listTop, float listBottom) {
    const auto& tracks = *m.tracks;
    int total = (int)tracks.size();
    float viewH = listBottom - listTop;
    float contentH = total * ROW_H;
    float maxScroll = contentH - viewH;
    if (maxScroll < 0) maxScroll = 0;
    if (scroll_ > maxScroll) scroll_ = maxScroll;
    if (scroll_ < 0) scroll_ = 0;

    rt_->PushAxisAlignedClip(D2D1::RectF(16, listTop, W_ - 16, listBottom),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    int first = (int)(scroll_ / ROW_H);
    int last = std::min(total - 1, first + (int)(viewH / ROW_H) + 1);
    for (int i = first; i <= last; i++) {
        if (i < 0 || i >= total) continue;
        const auto& t = tracks[i];
        float y = listTop + i * ROW_H - scroll_;
        D2D1_RECT_F row = D2D1::RectF(16, y, W_ - 16, y + ROW_H - 2);
        bool on = (i == m.idx);
        bool hotRow = (hover_ == 100 + i || hover_ == ID_TRACK_DEL + i);
        if (on) fillRound(row, 10, D2D1::ColorF(1.0f, 0.369f, 0.478f, 0.09f));
        else if (hotRow) fillRound(row, 10, D2D1::ColorF(1, 1, 1, 0.035f));
        if (on) fillRound(D2D1::RectF(18, y + 14, 21, y + 30), 1.5f, C_ACCENT);

        wchar_t num[8];
        swprintf(num, 8, L"%02d", i + 1);
        text(num, fSmall_, D2D1::RectF(28, y, 58, y + ROW_H - 2), on ? C_ACCENT : C_FAINT,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        text(w(t.title), fRow_, D2D1::RectF(68, y + 3, W_ - 340, y + 25), C_TEXT);
        text(w(t.artist.empty() ? "未知歌手" : t.artist), fRowSub_,
             D2D1::RectF(68, y + 23, W_ - 340, y + 43), C_MUTED);
        text(w(t.album), fRowSub_, D2D1::RectF(W_ - 320, y, W_ - 140, y + ROW_H - 2), C_FAINT);
        text(fmtTime(t.durationMs / 1000.0), fRowSub_,
             D2D1::RectF(W_ - 136, y, W_ - 66, y + ROW_H - 2), C_FAINT,
             DWRITE_TEXT_ALIGNMENT_TRAILING);

        // 命中测试从后往前找，删除按钮要登记在整行之后才能盖住整行
        addHot(100 + i, row);
        if (hotRow) {
            D2D1_RECT_F del = D2D1::RectF(W_ - 58, y + 12, W_ - 30, y + 34);
            bool hotDel = (hover_ == ID_TRACK_DEL + i);
            if (hotDel) fillRound(del, 6, D2D1::ColorF(1.0f, 0.369f, 0.478f, 0.18f));
            text(L"\u00D7", fRow_, del, hotDel ? C_ACCENT : C_MUTED,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            addHot(ID_TRACK_DEL + i, del);
        }
    }
    rt_->PopAxisAlignedClip();

    if (total == 0) {
        text(L"曲库是空的", fSub_,
             D2D1::RectF(0, listTop + viewH / 2 - 34, W_, listTop + viewH / 2 - 8), C_MUTED,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        text(L"点右上角「添加文件夹」，把放音乐的目录加进来", fSmall_,
             D2D1::RectF(0, listTop + viewH / 2 - 6, W_, listTop + viewH / 2 + 20), C_FAINT,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    if (maxScroll > 0) {
        float thumbH = viewH * viewH / contentH;
        if (thumbH < 28) thumbH = 28;
        float t = scroll_ / maxScroll;
        float ty = listTop + (viewH - thumbH) * t;
        fillRound(D2D1::RectF(W_ - 9, ty, W_ - 5, ty + thumbH), 2.0f,
                  D2D1::ColorF(0.23f, 0.25f, 0.30f));
    }
}

inline void View::paintFolderPanel(const Model& m, float top, float bottom) {
    const auto& folders = *m.folders;
    const auto& excluded = *m.excluded;
    float L = 16, R = W_ - 16;
    float contentTop = top + 40, contentBottom = bottom - 10;
    float viewH = contentBottom - contentTop;
    if (viewH < 40) return;

    fillRound(D2D1::RectF(L, top, R, bottom), 10, D2D1::ColorF(0.055f, 0.060f, 0.080f));
    strokeRound(D2D1::RectF(L, top, R, bottom), 10, C_LINE, 1.0f);

    // 顶栏固定，不随内容滚动
    text(L"文件夹管理", fRow_, D2D1::RectF(32, top + 6, 260, top + 34), C_TEXT);
    if (!excluded.empty()) {
        D2D1_RECT_F all = D2D1::RectF(R - 118, top + 7, R - 12, top + 33);
        bool hot = (hover_ == 16);
        if (hot) fillRound(all, 7, D2D1::ColorF(1, 1, 1, 0.06f));
        strokeRound(all, 7, hot ? C_ACCENT : C_LINE, 1.0f);
        text(L"全部恢复 " + std::to_wstring(excluded.size()) + L" 首", fRowSub_, all,
             hot ? C_ACCENT : C_MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
        addHot(16, all);
    }

    int nf = (int)folders.size(), ne = (int)excluded.size();
    float totalH = 28 + (nf ? nf : 1) * 34 + 34 + (ne ? ne : 1) * 30 + 44;
    float maxS = totalH - viewH;
    if (maxS < 0) maxS = 0;
    if (panelScroll_ > maxS) panelScroll_ = maxS;
    if (panelScroll_ < 0) panelScroll_ = 0;

    rt_->PushAxisAlignedClip(D2D1::RectF(L + 1, contentTop, R - 1, contentBottom),
                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = contentTop + 4 - panelScroll_;
    auto visible = [&](float h) { return y + h > contentTop && y < contentBottom; };

    text(L"已添加的文件夹  " + std::to_wstring(nf) + L" 个", fRowSub_,
         D2D1::RectF(32, y, R - 90, y + 24), C_FAINT);
    y += 28;
    if (nf == 0) {
        text(L"还没有添加目录，点右上角「添加文件夹」", fRowSub_,
             D2D1::RectF(32, y, R - 90, y + 30), C_FAINT);
        y += 34;
    }
    for (int i = 0; i < nf; i++, y += 34) {
        if (!visible(34)) continue;
        D2D1_RECT_F row = D2D1::RectF(28, y, R - 12, y + 30);
        bool hot = (hover_ == ID_FOLDER_DEL + i);
        if (hot) fillRound(row, 7, D2D1::ColorF(1, 1, 1, 0.035f));
        text(w(folders[i]), fRowSub_, D2D1::RectF(36, y, R - 76, y + 30), C_TEXT);
        D2D1_RECT_F del = D2D1::RectF(R - 62, y + 3, R - 24, y + 27);
        if (hot) fillRound(del, 6, D2D1::ColorF(1.0f, 0.369f, 0.478f, 0.18f));
        text(L"\u00D7", fRow_, del, hot ? C_ACCENT : C_MUTED,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        addHot(ID_FOLDER_DEL + i, del);
    }
    y += 6;

    text(L"已从曲库移除  " + std::to_wstring(ne) + L" 首（文件仍在磁盘上）", fRowSub_,
         D2D1::RectF(32, y, R - 90, y + 26), C_FAINT);
    y += 34;
    if (ne == 0) {
        text(L"没有移除过曲目。在曲库里把鼠标移到某一行，点右侧的 × 即可移除。", fRowSub_,
             D2D1::RectF(32, y, R - 24, y + 30), C_FAINT);
        y += 30;
    }
    for (int i = 0; i < ne; i++, y += 30) {
        if (!visible(30)) continue;
        D2D1_RECT_F row = D2D1::RectF(28, y, R - 12, y + 28);
        bool hot = (hover_ == ID_RESTORE + i);
        if (hot) fillRound(row, 7, D2D1::ColorF(1, 1, 1, 0.035f));
        text(fileName(excluded[i]), fRowSub_, D2D1::RectF(36, y, R - 100, y + 28), C_MUTED);
        D2D1_RECT_F rb = D2D1::RectF(R - 86, y + 2, R - 22, y + 26);
        if (hot) fillRound(rb, 6, D2D1::ColorF(1, 1, 1, 0.08f));
        strokeRound(rb, 6, hot ? C_ACCENT : C_LINE, 1.0f);
        text(L"恢复", fRowSub_, rb, hot ? C_ACCENT : C_MUTED,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        addHot(ID_RESTORE + i, rb);
    }
    y += 10;
    text(L"移除只影响曲库，不会删除磁盘上的文件；移错的目录可以再用「添加文件夹」加回来。",
         fRowSub_, D2D1::RectF(32, y, R - 24, y + 26), C_FAINT);
    rt_->PopAxisAlignedClip();

    if (maxS > 0) {
        float thumbH = viewH * viewH / totalH;
        if (thumbH < 28) thumbH = 28;
        float t = panelScroll_ / maxS;
        float ty = contentTop + (viewH - thumbH) * t;
        fillRound(D2D1::RectF(R - 6, ty, R - 3, ty + thumbH), 1.5f,
                  D2D1::ColorF(0.23f, 0.25f, 0.30f));
    }
}

inline void View::paintDevicePanel(const Model& m) {
    if (!devPanel_ || !m.devices) return;
    const auto& devs = *m.devices;
    const float rowH = 32.0f;
    int n = (int)devs.size();
    int maxRows = (int)((H_ - 16.0f - 248.0f) / rowH);
    if (maxRows < 1) maxRows = 1;
    if (n > maxRows) n = maxRows;

    const float pw = 300.0f;
    const float ph = 14.0f + n * rowH;
    const float px = W_ - 26.0f - pw;
    const float py = 248.0f;

    fillRound(D2D1::RectF(px, py, px + pw, py + ph), 12, D2D1::ColorF(0.075f, 0.082f, 0.105f));
    strokeRound(D2D1::RectF(px, py, px + pw, py + ph), 12, C_LINE, 1.0f);

    for (int i = 0; i < n; i++) {
        float y = py + 7.0f + i * rowH;
        D2D1_RECT_F row = D2D1::RectF(px + 6, y, px + pw - 6, y + rowH - 2);
        bool on = (i == m.devIndex);
        bool hot = (hover_ == ID_DEV + i);
        if (on) fillRound(row, 7, D2D1::ColorF(1.0f, 0.369f, 0.478f, 0.12f));
        else if (hot) fillRound(row, 7, D2D1::ColorF(1, 1, 1, 0.04f));
        text(w(devs[i]), fRowSub_, D2D1::RectF(px + 18, y, px + pw - 36, y + rowH - 2),
             on ? C_ACCENT : C_TEXT);
        if (on)
            text(L"\u2713", fRow_, D2D1::RectF(px + pw - 34, y, px + pw - 12, y + rowH - 2),
                 C_ACCENT, DWRITE_TEXT_ALIGNMENT_CENTER);
        addHot(ID_DEV + i, row);
    }
}

inline void View::paintMini(const Model& m) {
    const auto& tracks = *m.tracks;
    bool has = m.idx >= 0 && m.idx < (int)tracks.size();
    const lib::Track* cur = has ? &tracks[m.idx] : nullptr;

    fill(D2D1::RectF(0, 0, MINI_W, MINI_H), C_BG);

    // 拖动区必须最先登记：命中测试是从后往前找的，放最后会把所有按钮都盖住
    addHot(12, D2D1::RectF(0, 0, MINI_W, MINI_H));

    drawCover(D2D1::RectF(10, 10, 62, 62), 9.0f, has);

    text(cur ? w(cur->title) : L"—", fRow_, D2D1::RectF(72, 12, 208, 34), C_TEXT);
    text(cur ? (cur->artist.empty() ? L"未知歌手" : w(cur->artist)) : L"未播放", fRowSub_,
         D2D1::RectF(72, 34, 208, 54), C_MUTED);

    D2D1_RECT_F pv = D2D1::RectF(214, 14, 240, 40);
    D2D1_RECT_F pb = D2D1::RectF(244, 12, 274, 42);
    D2D1_RECT_F nx = D2D1::RectF(278, 14, 304, 40);
    icon(gPrev_, pv.left + 3, pv.top + 3, 20, hover_ == 1 ? C_TEXT : C_MUTED);
    icon(gNext_, nx.left + 3, nx.top + 3, 20, hover_ == 3 ? C_TEXT : C_MUTED);
    grad_->SetStartPoint(D2D1::Point2F(pb.left, pb.top));
    grad_->SetEndPoint(D2D1::Point2F(pb.right, pb.bottom));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(259, 27), 15.0f, 15.0f), grad_);
    if (m.playing) icon(gPause_, pb.left + 8, pb.top + 8, 15, D2D1::ColorF(1, 1, 1));
    else icon(gPlay_, pb.left + 8, pb.top + 8, 15, D2D1::ColorF(1, 1, 1));
    addHot(1, pv);
    addHot(2, pb);
    addHot(3, nx);

    // 右上两个小按钮
    D2D1_RECT_F restore = D2D1::RectF(318, 8, 332, 22);
    D2D1_RECT_F close = D2D1::RectF(318, 26, 332, 40);
    if (hover_ == 10) fillRound(restore, 4, D2D1::ColorF(1, 1, 1, 0.08f));
    if (hover_ == 11) fillRound(close, 4, D2D1::ColorF(1, 1, 1, 0.08f));
    // 还原：两根斜线拼出的箭头
    brush_->SetColor(C_FAINT);
    rt_->DrawLine(D2D1::Point2F(324, 19), D2D1::Point2F(330, 13), brush_, 1.4f);
    rt_->DrawLine(D2D1::Point2F(326, 12.5f), D2D1::Point2F(330.5f, 12.5f), brush_, 1.4f);
    rt_->DrawLine(D2D1::Point2F(330.5f, 12.5f), D2D1::Point2F(330.5f, 17), brush_, 1.4f);
    rt_->DrawLine(D2D1::Point2F(322, 30), D2D1::Point2F(328, 36), brush_, 1.4f);
    rt_->DrawLine(D2D1::Point2F(328, 30), D2D1::Point2F(322, 36), brush_, 1.4f);
    addHot(10, restore);
    addHot(11, close);

    // 小窗也要能调音量：迷你态虽然保留了任务栏按钮（APPWINDOW），但经常失焦，
    // ↑↓ 不一定送得到这里，所以给一条可点的滑条
    text(L"音量", fRowSub_, D2D1::RectF(72, 56, 108, 76), C_FAINT);
    D2D1_RECT_F volR = D2D1::RectF(112, 66, 300, 70);
    fillRound(volR, 2.0f, D2D1::ColorF(1, 1, 1, 0.08f));
    D2D1_RECT_F vf = volR;
    vf.right = volR.left + (volR.right - volR.left) * (m.vol < 0 ? 0 : m.vol);
    fillRound(vf, 2.0f, D2D1::ColorF(1, 1, 1, 0.55f));
    if (hover_ == 5 || dragVol_) circle(vf.right, 68, 3.5f, D2D1::ColorF(1, 1, 1));
    // 命中区上下放宽，左右与滑条对齐，拖动比例才准；登记在拖动区(12)之后才盖得住它
    addHot(5, D2D1::RectF(112, 56, 300, 78));

    if (m.spec) spectrum(*m.spec, D2D1::RectF(10, 80, MINI_W - 10, 108), 22, m.playing);
}

// ---------------- 交互 ----------------

inline int View::hitTest(POINT p) const {
    float x = p.x * 96.0f / dpi_, y = p.y * 96.0f / dpi_;
    for (int i = (int)hots_.size() - 1; i >= 0; i--) {
        const auto& h = hots_[i];
        if (x >= h.second.left && x <= h.second.right && y >= h.second.top &&
            y <= h.second.bottom)
            return h.first;
    }
    return 0;
}

inline void View::mouseMove(POINT p) {
    hover_ = hitTest(p);
    float x = p.x * 96.0f / dpi_;
    if (dragSeek_ || dragVol_) {
        int want = dragSeek_ ? 7 : 5;
        for (auto& hh : hots_) {
            if (hh.first != want) continue;
            float f = (x - hh.second.left) / (hh.second.right - hh.second.left);
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            if (dragSeek_) pendingSeekRatio_ = f;
            else pendingVol_ = f;
            break;
        }
    }
}

inline void View::mouseDown(POINT p) {
    int id = hitTest(p);
    active_ = id;
    hover_ = id;
    float x = p.x * 96.0f / dpi_;

    // 设备浮层开着时，点别处只关闭它，不触发底下的控件
    if (devPanel_ && id != 14 && !(id >= ID_DEV && id < ID_DEV + 1000)) {
        devPanel_ = false;
        return;
    }
    if (id == 14) {
        devPanel_ = !devPanel_;
        if (devPanel_) actions_.push_back({A_DEV_PANEL, -1});
        return;
    }
    if (id >= ID_DEV) {
        actions_.push_back({A_DEV_SELECT, id - ID_DEV});
        devPanel_ = false;
        return;
    }

    if (id == 7 || id == 5) {
        for (auto& hh : hots_) {
            if (hh.first != id) continue;
            float f = (x - hh.second.left) / (hh.second.right - hh.second.left);
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            if (id == 7) { dragSeek_ = true; pendingSeekRatio_ = f; }
            else { dragVol_ = true; pendingVol_ = f; }
            break;
        }
        return;
    }
    if (id == 12) { actions_.push_back({A_DRAG, -1}); return; }
    // 从大段到小段判断，互不重叠
    if (id >= ID_RESTORE) { actions_.push_back({A_RESTORE_TRACK, id - ID_RESTORE}); return; }
    if (id >= ID_FOLDER_DEL) { actions_.push_back({A_REMOVE_FOLDER, id - ID_FOLDER_DEL}); return; }
    if (id >= ID_TRACK_DEL) { actions_.push_back({A_REMOVE_TRACK, id - ID_TRACK_DEL}); return; }
    if (id >= 100) { actions_.push_back({A_ROW, id - 100}); return; }
    switch (id) {
        case 1: actions_.push_back({A_PREV, -1}); break;
        case 2: actions_.push_back({A_PLAY, -1}); break;
        case 3: actions_.push_back({A_NEXT, -1}); break;
        case 4: actions_.push_back({A_MODE, -1}); break;
        case 6: actions_.push_back({A_MINI, -1}); break;
        case 8: actions_.push_back({A_ADD, -1}); break;
        case 9: actions_.push_back({A_RESCAN, -1}); break;
        case 10: actions_.push_back({A_RESTORE, -1}); break;
        case 11: actions_.push_back({A_CLOSE, -1}); break;
        case 13:  // 管理面板开关，纯 UI 状态，不用交给 main
            panel_ = !panel_;
            panelScroll_ = 0;
            break;
        case 16: actions_.push_back({A_CLEAR_EXCLUDED, -1}); break;
        default: break;
    }
}

inline void View::mouseUp() {
    dragSeek_ = false;
    dragVol_ = false;
    active_ = 0;
}

inline void View::wheel(int delta) {
    if (devPanel_) return;  // 浮层开着时别滚到底下的曲库
    float& s = panel_ ? panelScroll_ : scroll_;
    s -= delta / 120.0f * 58.0f;
    if (s < 0) s = 0;
}

inline void View::keepVisible(int row, int totalRows) {
    float listTop = 332, listBottom = H_ - 12;
    float viewH = listBottom - listTop;
    float y = row * ROW_H;
    if (y < scroll_) scroll_ = y;
    if (y + ROW_H > scroll_ + viewH) scroll_ = y + ROW_H - viewH;
    if (scroll_ < 0) scroll_ = 0;
    (void)totalRows;
}

// ---------------- 封面 ----------------

inline bool View::setCover(const std::string& bytes) {
    clearCover();
    if (!wic_ || !rt_ || bytes.empty()) return false;

    IStream* stream = SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size());
    if (!stream) return false;

    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(
            wic_->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        dec && SUCCEEDED(dec->GetFrame(0, &frame)) && frame &&
        SUCCEEDED(wic_->CreateFormatConverter(&conv)) && conv &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        ok = SUCCEEDED(rt_->CreateBitmapFromWicBitmap(conv, nullptr, &cover_)) && cover_;
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    stream->Release();
    return ok;
}

inline void View::clearCover() {
    if (cover_) {
        cover_->Release();
        cover_ = nullptr;
    }
}

}  // namespace ui
