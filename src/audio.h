// audio.h —— Media Foundation 解码 + WASAPI 输出
//
// 设备生命周期和文件生命周期分开：默认设备不变时，切歌只换 SourceReader，
// 不重建 IAudioClient（旧版每首歌都 CoCreateInstance + Initialize，切歌有间隙）。
//
// 输出优先走 32 位浮点：共享模式用设备混音格式（通常是 float32），
// 独占模式（配置 exclusive=1）按源采样率协商，绕过系统混音器。
// 设备被拔掉 / 切换默认端点 / 睡眠唤醒后置 devLost_，由 UI 调 recover() 重建。
#pragma once

#include <windows.h>
#include <mmreg.h>
#include <audioclient.h>
#include <propsys.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace au {

// 类/接口 GUID 直接写死，省得跟 MinGW 的 __uuidof 支持较劲
static const CLSID kCLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIID_IMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIID_IAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIID_IAudioRenderClient = {
    0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const IID kIID_IMMNotificationClient = {
    0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
// 设备是 32 位浮点还是 16 位整数，靠这个子格式判断（不链 ksuser，自己写 GUID）
static const GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// 设备端格式
struct OutFmt {
    int rate = 48000;
    int ch = 2;
    int bits = 32;       // 容器位宽 16 / 24 / 32
    int validBits = 32;  // 有效位（24-in-32 时是 24）
    int blockAlign = 8;  // 字节/帧
    bool isFloat = true;
};

// 一个可选的输出端点
struct DeviceInfo {
    std::wstring id;    // IMMDevice::GetId，空表示跟随系统默认
    std::wstring name;  // 友好名
    bool isDefault = false;
};

// MinGW 不一定带 functiondiscoverykeys_devpkey.h，直接写 PKEY_Device_FriendlyName
static const PROPERTYKEY kPKEY_FriendlyName = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

// 枚举所有在用的渲染端点。第一项固定是「跟随系统默认」，id 为空。
// 调用前需要 COM 已初始化（主线程在 wWinMain 里做了）。
inline std::vector<DeviceInfo> listDevices() {
    std::vector<DeviceInfo> out;
    DeviceInfo def;
    def.name = L"跟随系统默认";
    def.isDefault = true;
    out.push_back(def);

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                kIID_IMMDeviceEnumerator, (void**)&en)) ||
        !en)
        return out;

    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d)) || !d) continue;
            DeviceInfo di;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id)) && id) {
                di.id = id;
                CoTaskMemFree(id);
            }
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kPKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                    di.name = v.pwszVal;
                PropVariantClear(&v);
                ps->Release();
            }
            d->Release();
            if (di.name.empty()) di.name = L"未命名设备";
            out.push_back(di);
        }
        col->Release();
    }
    en->Release();
    return out;
}

// 设备变更通知：回调在系统 COM 线程上，只置原子标志，真正的重建留给 UI tick
class DeviceNotify : public IMMNotificationClient {
public:
    explicit DeviceNotify(std::atomic<bool>* lost) : lost_(lost) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, kIID_IMMNotificationClient) || IsEqualIID(riid, IID_IUnknown)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        lost_->store(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        lost_->store(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && (role == eConsole || role == eMultimedia)) lost_->store(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    std::atomic<bool>* lost_;
};

class Player {
public:
    ~Player() { shutdown(); }

    // 进程启动时调一次：初始化 MF、注册设备通知
    bool init() {
        if (mfStarted_) return true;
        if (FAILED(MFStartup(MF_VERSION))) return false;
        mfStarted_ = true;
        if (SUCCEEDED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                       kIID_IMMDeviceEnumerator, (void**)&enum_)) &&
            enum_) {
            notify_ = new DeviceNotify(&devLost_);
            if (FAILED(enum_->RegisterEndpointNotificationCallback(notify_))) {
                delete notify_;
                notify_ = nullptr;
            }
        }
        return true;
    }

    void shutdown() {
        close();
        releaseDevice();
        if (enum_ && notify_) enum_->UnregisterEndpointNotificationCallback(notify_);
        if (notify_) {
            delete notify_;
            notify_ = nullptr;
        }
        if (enum_) {
            enum_->Release();
            enum_ = nullptr;
        }
        if (mfStarted_) {
            MFShutdown();
            mfStarted_ = false;
        }
    }

    bool open(const std::string& path, std::string* err) {
        close();
        if (!init()) {
            *err = "Media Foundation 初始化失败";
            return false;
        }

        std::wstring wp;
        int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
        wp.assign((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &wp[0], n);

        if (FAILED(MFCreateSourceReaderFromURL(wp.c_str(), nullptr, &reader_))) {
            *err = "打不开这个文件";
            return false;
        }
        reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

        // 源格式：独占模式按它协商，共享模式只用来兜底
        int srcRate = 44100, srcCh = 2;
        IMFMediaType* native = nullptr;
        if (SUCCEEDED(reader_->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                                  &native)) &&
            native) {
            UINT32 v = 0;
            if (SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &v)) && v)
                srcRate = (int)v;
            v = 0;
            if (SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &v)) && v) srcCh = (int)v;
            native->Release();
        }
        if (srcCh < 1 || srcCh > 8) srcCh = 2;
        srcRate_ = srcRate;
        srcCh_ = srcCh;

        OutFmt fmt;
        if (!ensureDevice(srcRate, srcCh, &fmt, err)) {
            close();
            return false;
        }

        // 时长直接问 MF，比解析码流可靠
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(reader_->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,
                                                        MF_PD_DURATION, &var)))
            dur_ = (double)var.hVal.QuadPart / 10000000.0;
        PropVariantClear(&var);

        rate_ = fmt.rate;
        ch_ = fmt.ch;
        ringCap_ = (size_t)rate_ * ch_ * 2;  // 2 秒
        if (ring_.size() < ringCap_) ring_.resize(ringCap_);
        ringHead_ = ringCount_ = 0;
        vizCap_ = 8192;
        if (viz_.size() < vizCap_) viz_.resize(vizCap_);
        vizHead_ = vizCount_ = 0;

        // 解码输出：先按设备格式要（float 设备就要 float，省一次转换），
        // 解码器不给 float 再退 16 位整数。ring 内部统一存 float。
        if (!setDecoderOutput(fmt, err)) {
            close();
            return false;
        }

        eos_ = false;
        stop_ = false;
        playing_ = false;
        pendingResume_ = false;
        posFrames_ = 0;
        seekBase_ = 0;
        seekReq_ = false;

        if (FAILED(ac_->Start())) {
            *err = "音频设备启动失败";
            close();
            return false;
        }
        dec_ = std::thread([this] { decodeLoop(); });
        ren_ = std::thread([this] { renderLoop(); });
        curPath_ = path;
        return true;
    }

    // 只停文件相关的线程和 reader，设备保留（切歌走这里，不重建 COM 对象）
    void close() {
        stop_ = true;
        rbSpace_.notify_all();
        rbData_.notify_all();
        if (dec_.joinable()) dec_.join();
        if (ren_.joinable()) ren_.join();
        if (ac_) ac_->Stop();
        if (reader_) {
            reader_->Release();
            reader_ = nullptr;
        }
        playing_ = false;
        eos_ = false;
        dur_ = 0;
        posFrames_ = 0;
        seekBase_ = 0;
    }

    void play() {
        if (!ac_) return;
        pendingResume_ = false;  // 用户主动播放，恢复正常状态
        if (eos_.load()) {
            // 播完了再按播放：从头来，顺手清掉 eos，免得 tick 又切下一首
            eos_ = false;
            seek(0);
        }
        playing_ = true;
    }
    void pause() {
        playing_ = false;
        pendingResume_ = false;  // 用户主动暂停，别在设备恢复后自己续播
    }
    void toggle() {
        if (playing_.load())
            pause();
        else
            play();
    }
    bool playing() const { return playing_.load(); }

    void seek(double sec) {
        if (!reader_) return;
        if (sec < 0) sec = 0;
        if (dur_ > 0 && sec > dur_) sec = dur_;
        {
            std::lock_guard<std::mutex> lk(rbM_);
            ringHead_ = ringCount_ = 0;
            seekGen_.fetch_add(1);
        }
        {
            std::lock_guard<std::mutex> lk(vizM_);
            vizHead_ = vizCount_ = 0;
        }
        seekTo_ = sec;
        seekReq_ = true;
        rbSpace_.notify_all();
    }

    double pos() const { return seekBase_.load() + (double)posFrames_.load() / (double)rate_; }
    double dur() const { return dur_; }
    int rate() const { return rate_; }
    int channels() const { return ch_; }
    const std::string& currentPath() const { return curPath_; }

    void setVolume(float v) { vol_ = v < 0 ? 0 : (v > 1 ? 1 : v); }
    float volume() const { return vol_.load(); }

    bool ended() const { return eos_.load(); }
    bool drained() {
        std::lock_guard<std::mutex> lk(rbM_);
        return ringCount_ == 0;
    }

    // 设备被拔掉 / 默认端点变了 / 睡眠唤醒：UI 每帧问一次
    bool deviceLost() const { return devLost_.load(); }
    // 独占模式是不是真的生效了（设备不支持时会静默回退共享）
    bool exclusiveActive() const { return exclusiveActive_; }
    void setExclusive(bool on) { exclusive_ = on; }
    bool exclusive() const { return exclusive_.load(); }

    // 指定输出端点；空字符串 = 跟随系统默认。改完要重开当前文件才生效
    void setDevice(const std::wstring& id) { devId_ = id; }
    const std::wstring& deviceId() const { return devId_; }
    // 上次 open 时发现指定设备不在了、自动退回了默认
    bool deviceFallback() const { return devFallback_; }
    void clearFallback() { devFallback_ = false; }

    // 重建设备链路，回到原来的位置和播放状态。
    // 失败会保留 devLost_ 和续播意图，调用方每帧问一次也不会把 UI 卡住。
    bool recover(std::string* err) {
        const double now = std::chrono::duration<double>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (now - lastRecover_ < 2.0) {
            err->clear();  // 节流中，不当作错误上报
            return false;
        }
        lastRecover_ = now;

        if (curPath_.empty()) {
            devLost_ = false;
            return false;
        }
        double at = pos();
        bool was = playing() || pendingResume_;
        std::string p = curPath_;
        close();          // 先停线程，再动 ac_/rc_，否则渲染线程可能还在用
        releaseDevice();
        if (!open(p, err)) {
            pendingResume_ = was;
            devLost_ = true;  // 设备还没回来，过两秒再试
            return false;
        }
        pendingResume_ = false;
        devLost_ = false;
        if (at > 0.3) seek(at);
        if (was) play();
        return true;
    }

    // 取最近的单声道样本给频谱用
    int readMono(float* dst, int maxN) {
        std::lock_guard<std::mutex> lk(vizM_);
        int n = (int)vizCount_;
        if (n > maxN) n = maxN;
        size_t start = (vizHead_ + vizCap_ - n) % vizCap_;
        for (int i = 0; i < n; i++) dst[i] = viz_[(start + i) % vizCap_];
        return n;
    }

private:
    IMMDevice* defaultDevice(std::string* err) {
        if (!enum_) {
            *err = "音频设备枚举失败";
            return nullptr;
        }
        IMMDevice* dev = nullptr;
        if (FAILED(enum_->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev) {
            *err = "没有可用的播放设备";
            return nullptr;
        }
        return dev;
    }

    // 优先用配置指定的端点；它被拔掉或停用了就退回系统默认，并置 devFallback_
    IMMDevice* openDevice(std::string* err) {
        if (enum_ && !devId_.empty()) {
            IMMDevice* d = nullptr;
            if (SUCCEEDED(enum_->GetDevice(devId_.c_str(), &d)) && d) {
                DWORD st = 0;
                if (SUCCEEDED(d->GetState(&st)) && st == DEVICE_STATE_ACTIVE) {
                    curDevId_ = devId_;
                    return d;
                }
                d->Release();
            }
            devFallback_ = true;
            devId_.clear();
            curDevId_.clear();
        }
        return defaultDevice(err);
    }

    // 设备格式没变就复用，变了才重建
    bool ensureDevice(int srcRate, int srcCh, OutFmt* out, std::string* err) {
        if (ac_ && devOk_ && !devLost_.load() && curDevId_ == devId_) {
            if (exclusive_) {
                if (devFmt_.rate == srcRate && devFmt_.ch == srcCh) {
                    *out = devFmt_;
                    return true;
                }
            } else {
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED(ac_->GetMixFormat(&mix)) && mix) {
                    bool same = (mix->nSamplesPerSec == (DWORD)devFmt_.rate &&
                                 mix->nChannels == (WORD)devFmt_.ch &&
                                 mix->wBitsPerSample == (WORD)devFmt_.bits);
                    CoTaskMemFree(mix);
                    if (same) {
                        *out = devFmt_;
                        return true;
                    }
                }
            }
        }
        releaseDevice();
        if (exclusive_) {
            if (createExclusive(srcRate, srcCh, out, err)) {
                exclusiveActive_ = true;
                return true;
            }
            // 设备不支持独占就回退共享，不打断播放
            exclusiveActive_ = false;
            releaseDevice();
        }
        bool ok = createShared(out, err);
        exclusiveActive_ = false;
        return ok;
    }

    bool createShared(OutFmt* out, std::string* err) {
        IMMDevice* dev = openDevice(err);
        if (!dev) return false;
        HRESULT hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac_);
        dev->Release();
        if (FAILED(hr) || !ac_) {
            *err = "音频设备打开失败";
            return false;
        }
        if (FAILED(ac_->GetMixFormat(&mix_)) || !mix_) {
            *err = "拿不到设备混音格式";
            return false;
        }
        evt_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = ac_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             200000, 0, mix_, nullptr);
        if (FAILED(hr)) {
            *err = "音频设备初始化失败";
            return false;
        }
        devFmt_ = fromWaveFormat(mix_);
        return finishDevice(out, err);
    }

    bool createExclusive(int srcRate, int srcCh, OutFmt* out, std::string* err) {
        IMMDevice* dev = openDevice(err);
        if (!dev) return false;
        HRESULT hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac_);
        dev->Release();
        if (FAILED(hr) || !ac_) {
            *err = "音频设备打开失败";
            return false;
        }

        WAVEFORMATEXTENSIBLE wf{};
        WAVEFORMATEX* closest = nullptr;
        const WAVEFORMATEX* use = nullptr;

        // 先要 float32 @ 源采样率，不行再退 PCM16
        makeFormat(srcRate, srcCh, true, &wf);
        hr = ac_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&wf, &closest);
        if (hr == S_OK) {
            use = (const WAVEFORMATEX*)&wf;
            if (closest) {
                CoTaskMemFree(closest);
                closest = nullptr;
            }
        } else if (hr == S_FALSE && closest) {
            use = closest;
        }

        if (!use) {
            makeFormat(srcRate, srcCh, false, &wf);
            hr = ac_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&wf, &closest);
            if (hr == S_OK) {
                use = (const WAVEFORMATEX*)&wf;
                if (closest) {
                    CoTaskMemFree(closest);
                    closest = nullptr;
                }
            } else if (hr == S_FALSE && closest) {
                use = closest;
            }
        }
        if (!use) {
            if (closest) CoTaskMemFree(closest);
            return false;
        }

        evt_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = ac_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             200000, 0, use, nullptr);
        if (FAILED(hr)) {
            if (closest) CoTaskMemFree(closest);
            if (evt_) {
                CloseHandle(evt_);
                evt_ = nullptr;
            }
            return false;
        }
        devFmt_ = fromWaveFormat(use);
        if (closest) CoTaskMemFree(closest);
        return finishDevice(out, err);
    }

    bool finishDevice(OutFmt* out, std::string* err) {
        if (FAILED(ac_->SetEventHandle(evt_))) {
            *err = "音频事件绑定失败";
            return false;
        }
        ac_->GetBufferSize(&bufFrames_);
        if (FAILED(ac_->GetService(kIID_IAudioRenderClient, (void**)&rc_)) || !rc_) {
            *err = "拿不到渲染接口";
            return false;
        }
        devOk_ = true;
        *out = devFmt_;
        return true;
    }

    void releaseDevice() {
        if (ac_) ac_->Stop();
        if (rc_) {
            rc_->Release();
            rc_ = nullptr;
        }
        if (ac_) {
            ac_->Release();
            ac_ = nullptr;
        }
        if (evt_) {
            CloseHandle(evt_);
            evt_ = nullptr;
        }
        if (mix_) {
            CoTaskMemFree(mix_);
            mix_ = nullptr;
        }
        devOk_ = false;
        bufFrames_ = 0;
    }

    static OutFmt fromWaveFormat(const WAVEFORMATEX* f) {
        OutFmt o;
        o.rate = (int)f->nSamplesPerSec;
        o.ch = f->nChannels;
        o.bits = f->wBitsPerSample;
        o.validBits = f->wBitsPerSample;
        o.blockAlign = f->nBlockAlign;
        o.isFloat = (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
            auto* ex = (const WAVEFORMATEXTENSIBLE*)f;
            o.isFloat = IsEqualGUID(ex->SubFormat, kSubtypeFloat) != 0;
            if (ex->Samples.wValidBitsPerSample) o.validBits = ex->Samples.wValidBitsPerSample;
        }
        return o;
    }

    static void makeFormat(int rate, int ch, bool flt, WAVEFORMATEXTENSIBLE* f) {
        memset(f, 0, sizeof(*f));
        f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f->Format.nChannels = (WORD)ch;
        f->Format.nSamplesPerSec = (DWORD)rate;
        f->Format.wBitsPerSample = (WORD)(flt ? 32 : 16);
        f->Format.nBlockAlign = (WORD)(ch * (flt ? 4 : 2));
        f->Format.nAvgBytesPerSec = rate * f->Format.nBlockAlign;
        f->Format.cbSize = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
        f->Samples.wValidBitsPerSample = f->Format.wBitsPerSample;
        f->dwChannelMask = (ch == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                     : (ch == 1 ? SPEAKER_FRONT_CENTER : 0);
        f->SubFormat = flt ? kSubtypeFloat : kSubtypePcm;
    }

    bool setDecoderOutput(const OutFmt& dev, std::string* err) {
        if (trySetDecoder(dev)) return true;
        OutFmt pcm = dev;
        pcm.isFloat = false;
        pcm.bits = 16;
        pcm.validBits = 16;
        pcm.blockAlign = dev.ch * 2;
        if (trySetDecoder(pcm)) return true;
        *err = "这个文件的音频格式解不了";
        return false;
    }

    bool trySetDecoder(const OutFmt& f) {
        IMFMediaType* out = nullptr;
        if (FAILED(MFCreateMediaType(&out))) return false;
        const int bits = f.isFloat ? 32 : 16;
        const int align = f.ch * (bits / 8);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, f.isFloat ? MFAudioFormat_Float : MFAudioFormat_PCM);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, (UINT32)bits);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)f.ch);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)f.rate);
        out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)align);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(f.rate * align));
        HRESULT hr =
            reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, out);
        out->Release();
        if (FAILED(hr)) return false;
        decFloat_ = f.isFloat;
        return true;
    }

    void decodeLoop() {
        while (!stop_.load()) {
            if (seekReq_.exchange(false)) {
                PROPVARIANT v;
                PropVariantInit(&v);
                v.vt = VT_I8;
                v.hVal.QuadPart = (LONGLONG)(seekTo_.load() * 10000000.0);
                reader_->SetCurrentPosition(GUID_NULL, v);
                PropVariantClear(&v);
                seekBase_ = seekTo_.load();
                posFrames_ = 0;
                eos_ = false;
                continue;
            }

            {
                std::unique_lock<std::mutex> lk(rbM_);
                rbSpace_.wait_for(lk, std::chrono::milliseconds(60), [this] {
                    return stop_.load() || ringCount_ < ringCap_ * 3 / 4;
                });
                if (stop_.load()) break;
                if (ringCount_ >= ringCap_ * 3 / 4) continue;
            }

            const uint32_t gen = seekGen_.load();
            DWORD flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                             nullptr, &flags, &ts, &sample);
            if (FAILED(hr)) {
                if (sample) sample->Release();  // 失败路径也别漏掉可能返回的 sample
                eos_ = true;
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (sample) sample->Release();
                eos_ = true;
                rbData_.notify_all();
                break;
            }
            if (!sample) continue;

            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&p, nullptr, &len)) && p && len) {
                    // seek 期间读出来的旧片段直接丢掉，别推进环里
                    if (gen == seekGen_.load()) pushBytes(p, len);
                    buf->Unlock();
                }
                buf->Release();
            }
            sample->Release();
        }
    }

    void pushBytes(const BYTE* p, DWORD bytes) {
        const size_t bps = decFloat_ ? 4 : 2;
        size_t samples = bytes / bps;
        size_t written = 0, head = 0;
        {
            std::lock_guard<std::mutex> lk(rbM_);
            size_t space = ringCap_ - ringCount_;
            size_t n = samples < space ? samples : space;
            size_t tail = (ringHead_ + ringCount_) % ringCap_;
            if (decFloat_) {
                const float* s = (const float*)p;
                for (size_t i = 0; i < n; i++) ring_[(tail + i) % ringCap_] = s[i];
            } else {
                const int16_t* s = (const int16_t*)p;
                for (size_t i = 0; i < n; i++)
                    ring_[(tail + i) % ringCap_] = s[i] * (1.0f / 32768.0f);
            }
            ringCount_ += n;
            written = n;
            head = ringHead_;  // 读指针 = 即将送给声卡的位置
        }
        rbData_.notify_all();

        if (written && ch_ > 0) {
            size_t frames = written / (size_t)ch_;
            std::lock_guard<std::mutex> lk(vizM_);
            // 必须从 ringHead_ 取：那是"下一个要播的样本"，和听到的声音对齐。
            // 换成 tail（最新解码）会领先整整一个缓冲，最多 1.5 秒。
            for (size_t f = 0; f < frames; f++) {
                float m = 0;
                for (int c = 0; c < ch_; c++) m += ring_[(head + f * ch_ + c) % ringCap_];
                viz_[vizHead_] = m / (float)ch_;
                vizHead_ = (vizHead_ + 1) % vizCap_;
                if (vizCount_ < vizCap_) vizCount_++;
            }
        }
    }

    // 从环里取 frames 帧，顺带套音量；返回实际取到的帧数
    size_t popFrames(float* dst, size_t frames) {
        std::lock_guard<std::mutex> lk(rbM_);
        size_t want = frames * (size_t)ch_;
        size_t n = want < ringCount_ ? want : ringCount_;
        n -= n % (size_t)ch_;
        float v = vol_.load();
        for (size_t i = 0; i < n; i++) {
            dst[i] = ring_[(ringHead_ + i) % ringCap_] * v;
        }
        ringHead_ = (ringHead_ + n) % ringCap_;
        ringCount_ -= n;
        return n / (size_t)ch_;
    }

    // 按设备真实格式写缓冲：float32 / int16 / 24-bit packed / 24-in-32 / int32
    void writeDevice(BYTE* data, const float* src, size_t got, size_t want) {
        const size_t samples = got * (size_t)ch_;
        if (devFmt_.isFloat) {
            float* d = (float*)data;
            if (samples) memcpy(d, src, samples * sizeof(float));
            if (got < want) memset(d + samples, 0, (want - got) * (size_t)ch_ * sizeof(float));
            return;
        }
        if (devFmt_.bits == 16) {
            int16_t* d = (int16_t*)data;
            for (size_t i = 0; i < samples; i++) d[i] = (int16_t)(src[i] * 32767.0f);
            if (got < want) memset(d + samples, 0, (want - got) * (size_t)ch_ * sizeof(int16_t));
            return;
        }
        if (devFmt_.bits == 24 && devFmt_.blockAlign == (int)((size_t)ch_ * 3)) {
            BYTE* d = data;
            for (size_t i = 0; i < samples; i++) {
                int32_t s = (int32_t)((double)src[i] * 8388607.0);
                d[i * 3 + 0] = (BYTE)(s & 0xFF);
                d[i * 3 + 1] = (BYTE)((s >> 8) & 0xFF);
                d[i * 3 + 2] = (BYTE)((s >> 16) & 0xFF);
            }
            if (got < want) memset(d + samples * 3, 0, (want - got) * (size_t)ch_ * 3);
            return;
        }
        // 32 位整数容器（含 24-in-32：有效位在高位，低位移空）
        int32_t* d = (int32_t*)data;
        const int vb = devFmt_.validBits > 0 ? devFmt_.validBits : 32;
        const int shift = 32 - vb;
        const double scale = (double)((1LL << (vb - 1)) - 1);
        for (size_t i = 0; i < samples; i++) {
            // 音量/解码浮点可能略微越界，直接乘满量程再转 int32 是 UB
            double x = (double)src[i];
            if (x > 1.0) x = 1.0;
            if (x < -1.0) x = -1.0;
            d[i] = (int32_t)(x * scale) << shift;
        }
        if (got < want) memset(d + samples, 0, (want - got) * (size_t)ch_ * sizeof(int32_t));
    }

    void renderLoop() {
        std::vector<float> tmp;
        while (!stop_.load()) {
            DWORD w = WaitForSingleObject(evt_, 200);
            if (w != WAIT_OBJECT_0) continue;
            if (stop_.load()) break;
            UINT32 pad = 0;
            if (FAILED(ac_->GetCurrentPadding(&pad))) {
                devLost_ = true;  // 设备没了，交给 UI 重建，别把线程静默杀掉
                break;
            }
            UINT32 want = bufFrames_ - pad;
            if (want == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(rc_->GetBuffer(want, &data)) || !data) {
                devLost_ = true;
                break;
            }

            if (!playing_.load()) {
                memset(data, 0, (size_t)want * (size_t)devFmt_.blockAlign);
                rc_->ReleaseBuffer(want, 0);
                continue;
            }

            tmp.resize((size_t)want * (size_t)ch_);
            size_t got = popFrames(tmp.data(), want);
            writeDevice(data, tmp.data(), got, want);
            posFrames_ += (int64_t)got;
            rc_->ReleaseBuffer(want, 0);

            if (got < want) {
                // 欠载：给解码线程一点时间，别空转
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    IMFSourceReader* reader_ = nullptr;
    IAudioClient* ac_ = nullptr;
    IAudioRenderClient* rc_ = nullptr;
    IMMDeviceEnumerator* enum_ = nullptr;
    DeviceNotify* notify_ = nullptr;
    HANDLE evt_ = nullptr;
    UINT32 bufFrames_ = 0;
    WAVEFORMATEX* mix_ = nullptr;
    OutFmt devFmt_;
    bool devOk_ = false;
    bool decFloat_ = false;
    bool mfStarted_ = false;
    bool exclusiveActive_ = false;
    std::atomic<bool> exclusive_{false};
    double dur_ = 0;
    int rate_ = 48000, ch_ = 2;
    int srcRate_ = 44100, srcCh_ = 2;
    std::string curPath_;
    std::wstring devId_, curDevId_;
    bool devFallback_ = false;
    bool pendingResume_ = false;
    double lastRecover_ = 0;

    std::vector<float> ring_;
    size_t ringCap_ = 0, ringHead_ = 0, ringCount_ = 0;
    std::mutex rbM_;
    std::condition_variable rbSpace_, rbData_;
    std::atomic<uint32_t> seekGen_{0};

    std::vector<float> viz_;
    size_t vizCap_ = 0, vizHead_ = 0, vizCount_ = 0;
    std::mutex vizM_;

    std::atomic<bool> playing_{false}, stop_{false}, eos_{false}, seekReq_{false};
    std::atomic<bool> devLost_{false};
    std::atomic<int64_t> posFrames_{0};
    std::atomic<double> seekBase_{0.0}, seekTo_{0.0};
    std::atomic<float> vol_{0.7f};

    std::thread dec_, ren_;
};

}  // namespace au
