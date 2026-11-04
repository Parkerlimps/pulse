// audio.h —— Media Foundation 解码 + WASAPI 输出
//
// 解码线程：IMFSourceReader 把文件解成 16bit PCM（格式由 MF 自动重采样到设备混音格式）
// 渲染线程：WASAPI 事件驱动，按设备混音格式填缓冲
// 中间用一条 2 秒的环形缓冲隔开，两边互不等待
#pragma once

#include <windows.h>
#include <audioclient.h>
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
// 设备是 32 位浮点还是 16 位整数，靠这个子格式判断（不链 ksuser，自己写 GUID）
static const GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const IID kIID_IAudioRenderClient = {
    0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

class Player {
public:
    ~Player() { close(); }

    bool open(const std::string& path, std::string* err) {
        close();
        if (!mfStarted_) {
            if (FAILED(MFStartup(MF_VERSION))) {
                *err = "Media Foundation 初始化失败";
                return false;
            }
            mfStarted_ = true;
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

        if (!initAudioDevice(err)) {
            close();
            return false;
        }
        if (!setDecoderOutput(err)) {
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

        ringCap_ = (size_t)rate_ * ch_ * 2;  // 2 秒
        ring_.assign(ringCap_, 0.0f);
        ringHead_ = ringCount_ = 0;
        vizCap_ = 8192;
        viz_.assign(vizCap_, 0.0f);
        vizCount_ = 0;

        eos_ = false;
        stop_ = false;
        playing_ = false;
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
        return true;
    }

    void close() {
        stop_ = true;
        rbSpace_.notify_all();
        rbData_.notify_all();
        if (dec_.joinable()) dec_.join();
        if (ren_.joinable()) ren_.join();
        if (ac_) ac_->Stop();
        if (rc_) { rc_->Release(); rc_ = nullptr; }
        if (ac_) { ac_->Release(); ac_ = nullptr; }
        if (evt_) { CloseHandle(evt_); evt_ = nullptr; }
        if (mix_) { CoTaskMemFree(mix_); mix_ = nullptr; }
        if (reader_) { reader_->Release(); reader_ = nullptr; }
        playing_ = false;
        eos_ = false;
        dur_ = 0;
    }

    void play() {
        if (!ac_) return;
        playing_ = true;
        eos_ = false;
    }
    void pause() { playing_ = false; }
    void toggle() { playing_ = !playing_.load(); }
    bool playing() const { return playing_.load(); }

    void seek(double sec) {
        if (!reader_) return;
        if (sec < 0) sec = 0;
        if (dur_ > 0 && sec > dur_) sec = dur_;
        {
            std::lock_guard<std::mutex> lk(rbM_);
            ringHead_ = ringCount_ = 0;
        }
        seekTo_ = sec;
        seekReq_ = true;
        rbSpace_.notify_all();
    }

    double pos() const { return seekBase_.load() + (double)posFrames_.load() / (double)rate_; }
    double dur() const { return dur_; }
    int rate() const { return rate_; }
    int channels() const { return ch_; }

    void setVolume(float v) { vol_ = v < 0 ? 0 : (v > 1 ? 1 : v); }
    float volume() const { return vol_.load(); }

    bool ended() const { return eos_.load(); }
    bool drained() {
        std::lock_guard<std::mutex> lk(rbM_);
        return ringCount_ == 0;
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
    bool initAudioDevice(std::string* err) {
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                    kIID_IMMDeviceEnumerator, (void**)&en)) ||
            !en) {
            *err = "找不到音频设备";
            return false;
        }
        IMMDevice* dev = nullptr;
        HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        en->Release();
        if (FAILED(hr) || !dev) {
            *err = "没有可用的播放设备";
            return false;
        }
        hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac_);
        dev->Release();
        if (FAILED(hr) || !ac_) {
            *err = "音频设备打开失败";
            return false;
        }
        if (FAILED(ac_->GetMixFormat(&mix_)) || !mix_) {
            *err = "拿不到设备混音格式";
            return false;
        }
        ch_ = mix_->nChannels;
        rate_ = (int)mix_->nSamplesPerSec;
        mixFloat_ = (mix_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (mix_->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ex = (WAVEFORMATEXTENSIBLE*)mix_;
            mixFloat_ = IsEqualGUID(ex->SubFormat, kSubtypeFloat) ? true : false;
        }

        evt_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = ac_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             200000, 0, mix_, nullptr);
        if (FAILED(hr)) {
            *err = "音频设备初始化失败";
            return false;
        }
        ac_->SetEventHandle(evt_);
        ac_->GetBufferSize(&bufFrames_);
        if (FAILED(ac_->GetService(kIID_IAudioRenderClient, (void**)&rc_)) || !rc_) {
            *err = "拿不到渲染接口";
            return false;
        }
        return true;
    }

    // 让 MF 直接输出设备格式的 PCM16，重采样交给它自己（Audio Resampler DSP）
    bool setDecoderOutput(std::string* err) {
        IMFMediaType* out = nullptr;
        if (FAILED(MFCreateMediaType(&out))) {
            *err = "创建媒体类型失败";
            return false;
        }
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch_);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate_);
        out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch_ * 2);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate_ * ch_ * 2);
        HRESULT hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                  nullptr, out);
        out->Release();
        if (FAILED(hr)) {
            *err = "这个文件的音频格式解不了";
            return false;
        }
        return true;
    }

    void decodeLoop() {
        std::vector<int16_t> tmp;
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

            DWORD flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                             nullptr, &flags, &ts, &sample);
            if (FAILED(hr)) {
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
                    pushPCM((const int16_t*)p, (int)(len / 2));
                    buf->Unlock();
                }
                buf->Release();
            }
            sample->Release();
        }
    }

    void pushPCM(const int16_t* p, int samples) {
        const float k = 1.0f / 32768.0f;
        size_t written = 0;
        {
            std::lock_guard<std::mutex> lk(rbM_);
            size_t space = ringCap_ - ringCount_;
            size_t n = (samples < (int)space) ? (size_t)samples : space;
            size_t tail = (ringHead_ + ringCount_) % ringCap_;
            for (size_t i = 0; i < n; i++) {
                ring_[(tail + i) % ringCap_] = p[i] * k;
            }
            ringCount_ += n;
            written = n;
        }
        rbData_.notify_all();

        if (written && ch_ > 0) {
            size_t frames = written / (size_t)ch_;
            std::lock_guard<std::mutex> lk(vizM_);
            for (size_t f = 0; f < frames; f++) {
                float m = 0;
                for (int c = 0; c < ch_; c++) m += p[f * ch_ + c];
                viz_[vizHead_] = m / (ch_ * 32768.0f);
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

    void renderLoop() {
        std::vector<float> tmp;
        while (!stop_.load()) {
            DWORD w = WaitForSingleObject(evt_, 200);
            if (w != WAIT_OBJECT_0) continue;
            UINT32 pad = 0;
            if (FAILED(ac_->GetCurrentPadding(&pad))) break;
            UINT32 want = bufFrames_ - pad;
            if (want == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(rc_->GetBuffer(want, &data)) || !data) continue;

            if (!playing_.load()) {
                memset(data, 0, (size_t)want * mix_->nBlockAlign);
                rc_->ReleaseBuffer(want, 0);
                continue;
            }

            tmp.resize((size_t)want * ch_);
            size_t got = popFrames(tmp.data(), want);

            if (mixFloat_) {
                float* d = (float*)data;
                if (got) memcpy(d, tmp.data(), got * ch_ * sizeof(float));
                if (got < want)
                    memset(d + got * ch_, 0, (want - got) * ch_ * sizeof(float));
            } else {
                int16_t* d = (int16_t*)data;
                for (size_t i = 0; i < got * ch_; i++) {
                    float v = tmp[i];
                    d[i] = (int16_t)(v * 32767.0f);
                }
                if (got < want) memset(d + got * ch_, 0, (want - got) * ch_ * 2);
            }
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
    HANDLE evt_ = nullptr;
    UINT32 bufFrames_ = 0;
    WAVEFORMATEX* mix_ = nullptr;
    bool mixFloat_ = true;
    int rate_ = 48000, ch_ = 2;
    bool mfStarted_ = false;
    double dur_ = 0;

    std::vector<float> ring_;
    size_t ringCap_ = 0, ringHead_ = 0, ringCount_ = 0;
    std::mutex rbM_;
    std::condition_variable rbSpace_, rbData_;

    std::vector<float> viz_;
    size_t vizCap_ = 0, vizHead_ = 0, vizCount_ = 0;
    std::mutex vizM_;

    std::atomic<bool> playing_{false}, stop_{false}, eos_{false}, seekReq_{false};
    std::atomic<int64_t> posFrames_{0};
    std::atomic<double> seekBase_{0.0}, seekTo_{0.0};
    std::atomic<float> vol_{0.7f};

    std::thread dec_, ren_;
};

}  // namespace au
