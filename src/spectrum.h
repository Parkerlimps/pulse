// spectrum.h —— 频谱分析：FFT + 对数分频 + 自动增益
//
// 视觉参数和之前 WebAudio 那一版对齐（minDecibels -88 / maxDecibels -12），
// 这样换成原生实现后柱子的观感不会变。
#pragma once

#include <cmath>
#include <cstring>
#include <vector>

namespace dsp {

constexpr int kFftSize = 2048;  // 2 的幂
constexpr int kBars = 52;

struct Spectrum {
    std::vector<float> win, re, im;
    float levels[kBars] = {0};
    float peaks[kBars] = {0};
    float gain = 1.0f;

    void init() {
        win.resize(kFftSize);
        re.resize(kFftSize);
        im.resize(kFftSize);
        for (int i = 0; i < kFftSize; i++)
            win[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / (kFftSize - 1));
    }

    // 暂停时让柱子自己落下去
    void decay(float dt) {
        float k = 1.0f - expf(-dt * 9.0f);
        for (int i = 0; i < kBars; i++) {
            levels[i] += (0.0f - levels[i]) * k;
            peaks[i] = (peaks[i] > 0.02f) ? peaks[i] - dt * 0.42f : 0.0f;
        }
    }

    // mono: 单声道混合样本；n 实际点数；sampleRate 设备采样率
    void analyze(const float* mono, int n, int sampleRate, float dt) {
        if (n < kFftSize) {
            decay(dt);
            return;
        }
        const float* src = mono + (n - kFftSize);
        for (int i = 0; i < kFftSize; i++) {
            re[i] = src[i] * win[i];
            im[i] = 0.0f;
        }
        fft(re, im, kFftSize);

        const int half = kFftSize / 2;
        float raw[kBars];
        const float nyq = sampleRate * 0.5f;
        const float maxHz = nyq < 9000.0f ? nyq : 9000.0f;
        const float low = 40.0f;

        for (int b = 0; b < kBars; b++) {
            float f0 = low * powf(maxHz / low, (float)b / kBars);
            float f1 = low * powf(maxHz / low, (float)(b + 1) / kBars);
            int a = (int)(f0 / nyq * half);
            int c = (int)ceilf(f1 / nyq * half);
            if (a < 0) a = 0;
            if (a > half - 1) a = half - 1;
            if (c <= a) c = a + 1;
            if (c > half) c = half;

            float m = 0.0f;
            for (int k = a; k < c; k++) {
                float mag = sqrtf(re[k] * re[k] + im[k] * im[k]) * 2.0f / kFftSize;
                if (mag > m) m = mag;
            }
            // 转 dB，再映射到 0..1（和 AnalyserNode 的 min/maxDecibels 一致）
            float db = 20.0f * log10f(m + 1e-9f);
            float t = (db + 88.0f) / 76.0f;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            float v = powf(t, 1.7f);
            v *= 0.45f + 0.55f * ((float)b / (kBars - 1));  // 压低频、抬高频
            raw[b] = v;
        }

        // 自动增益：拿本帧 88 分位当基准，慢跟随
        float sorted[kBars];
        memcpy(sorted, raw, sizeof(raw));
        for (int i = 1; i < kBars; i++) {  // 插入排序，52 个元素不值得上快排
            float x = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > x) {
                sorted[j + 1] = sorted[j];
                j--;
            }
            sorted[j + 1] = x;
        }
        float p = sorted[(int)(kBars * 0.88f)];
        float target = 0.8f / (p > 0.055f ? p : 0.055f);
        if (target > 4.0f) target = 4.0f;
        if (target < 0.9f) target = 0.9f;
        gain += (target - gain) * 0.07f;

        float rise = 1.0f - expf(-dt * 26.0f);
        float fall = 1.0f - expf(-dt * 5.0f);
        for (int i = 0; i < kBars; i++) {
            float v = raw[i] * gain;
            if (v > 1.0f) v = 1.0f;
            levels[i] += (v - levels[i]) * (v > levels[i] ? rise : fall);
            if (levels[i] > peaks[i]) peaks[i] = levels[i];
            else peaks[i] -= dt * 0.42f;
            if (peaks[i] < 0) peaks[i] = 0;
        }
    }

private:
    static void fft(std::vector<float>& re, std::vector<float>& im, int n) {
        // 位反转置换
        for (int i = 1, j = 0; i < n; i++) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                float t = re[i]; re[i] = re[j]; re[j] = t;
                t = im[i]; im[i] = im[j]; im[j] = t;
            }
        }
        for (int len = 2; len <= n; len <<= 1) {
            float ang = -2.0f * 3.14159265358979f / len;
            float wr = cosf(ang), wi = sinf(ang);
            for (int i = 0; i < n; i += len) {
                float cr = 1.0f, ci = 0.0f;
                for (int k = 0; k < len / 2; k++) {
                    int a = i + k, b = i + k + len / 2;
                    float xr = re[b] * cr - im[b] * ci;
                    float xi = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - xr; im[b] = im[a] - xi;
                    re[a] += xr;        im[a] += xi;
                    float ncr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = ncr;
                }
            }
        }
    }
};

}  // namespace dsp
