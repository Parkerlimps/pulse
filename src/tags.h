// tags.h —— 读音频元数据：ID3v2/v1、FLAC STREAMINFO / VORBIS_COMMENT / PICTURE
//
// 自己解析，不引入 taglib：需要的字段就标题、歌手、专辑、时长、内嵌封面这几个，
// 依赖一个 3MB 的库不划算。封面只记偏移，用时按区间从原文件读，不常驻内存。
#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "util.h"

namespace tags {

struct Meta {
    std::string title, artist, album;
    long long durationMs = 0;
    long long coverOff = 0;
    long long coverLen = 0;
};

inline uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint32_t be24(const unsigned char* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
inline uint32_t le32(const unsigned char* p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint32_t ss32(const unsigned char* p) {  // ID3 的 7 位一节长度
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

// ID3 的编码字节 → UTF-8。0 标称 Latin-1，但国内 mp3 里九成是 GBK，
// 所以按系统 ANSI 码页解更靠谱
inline std::string decodeText(const unsigned char* p, size_t n, int enc) {
    if (n == 0) return "";
    std::string out;
    if (enc == 3) {
        out.assign((const char*)p, n);
    } else if (enc == 1 || enc == 2) {
        std::wstring w;
        bool be = (enc == 2);
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if (p[0] == 0xFF && p[1] == 0xFE) { i = 2; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
        }
        for (; i + 1 < n; i += 2) {
            wchar_t ch = be ? (wchar_t)((p[i] << 8) | p[i + 1]) : (wchar_t)(p[i] | (p[i + 1] << 8));
            if (ch == 0) break;
            w += ch;
        }
        out = util::narrow(w);
    } else {
        int len = MultiByteToWideChar(CP_ACP, 0, (const char*)p, (int)n, nullptr, 0);
        std::wstring w((size_t)len, L'\0');
        MultiByteToWideChar(CP_ACP, 0, (const char*)p, (int)n, &w[0], len);
        size_t z = w.find(L'\0');
        if (z != std::wstring::npos) w.resize(z);
        out = util::narrow(w);
    }
    // 去掉尾部填充的 NUL
    while (!out.empty() && (out.back() == '\0' || out.back() == ' ')) out.pop_back();
    return out;
}

// ---------------- MP3 时长 ----------------

inline long long mp3Duration(const std::vector<unsigned char>& b, long long fileSize,
                             size_t audioStart) {
    static const int br1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int br2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int sr1[] = {44100, 48000, 32000, 0};
    static const int sr2[] = {22050, 24000, 16000, 0};
    static const int sr25[] = {11025, 12000, 8000, 0};

    for (size_t i = audioStart; i + 4 < b.size() && i < audioStart + 200000; i++) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
        int ver = (b[i + 1] >> 3) & 3;  // 3=MPEG1 2=MPEG2 0=MPEG2.5
        int layer = (b[i + 1] >> 1) & 3;
        int bri = (b[i + 2] >> 4) & 15;
        int sri = (b[i + 2] >> 2) & 3;
        if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) continue;

        int bitrate = (ver == 3 ? br1 : br2)[bri] * 1000;
        int sr = (ver == 3 ? sr1 : (ver == 2 ? sr2 : sr25))[sri];
        if (!bitrate || !sr) continue;

        int pad = (b[i + 2] >> 1) & 1;
        int mono = (b[i + 3] >> 6) == 3;
        size_t frameLen = (size_t)((ver == 3 ? 144 : 72) * bitrate / sr) + pad;

        // 有 Xing/Info 头就用总帧数，VBR 才算得准
        size_t xoff = i + 4 + (mono ? 17 : 32);
        if (xoff + 12 < b.size() && (memcmp(&b[xoff], "Xing", 4) == 0 ||
                                     memcmp(&b[xoff], "Info", 4) == 0)) {
            uint32_t flags = be32(&b[xoff + 4]);
            if (flags & 1) {
                uint32_t frames = be32(&b[xoff + 8]);
                return (long long)frames * (ver == 3 ? 1152 : 576) * 1000 / sr;
            }
        }
        if (frameLen == 0) frameLen = 1;
        long long audioBytes = fileSize - (long long)i;
        if (audioBytes <= 0) return 0;
        return audioBytes * 8 * 1000 / bitrate;
    }
    return 0;
}

// ---------------- 解析入口 ----------------

inline Meta read(const std::string& path) {
    Meta m;
    std::wstring wp = util::widen(path);
    HANDLE f = CreateFileW(wp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return m;

    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    long long fileSize = sz.QuadPart;

    // 先读文件头判断 ID3v2 的标签区有多长：封面动辄几 MB，死读 2 MB 会在
    // 大 APIC 处断掉，后面的标题就读不到了。上限 16 MB，读到的都是标签区。
    unsigned char hdr[10] = {};
    DWORD got = 0;
    size_t head = (size_t)std::min<long long>(fileSize, 10);
    ReadFile(f, hdr, (DWORD)head, &got, nullptr);
    size_t want = 2 << 20;
    if (got == 10 && memcmp(hdr, "ID3", 3) == 0) {
        size_t tagSize = ss32(&hdr[6]);
        want = 10 + tagSize;
        if (want > (16u << 20)) want = 16u << 20;
    }
    want = (size_t)std::min<long long>((long long)want, fileSize);
    std::vector<unsigned char> b(want);
    SetFilePointer(f, 0, nullptr, FILE_BEGIN);
    got = 0;
    ReadFile(f, b.data(), (DWORD)b.size(), &got, nullptr);
    b.resize(got);
    CloseHandle(f);
    if (b.size() < 16) return m;

    size_t audioStart = 0;

    if (memcmp(b.data(), "ID3", 3) == 0) {
        int ver = b[3];
        uint32_t tagSize = ss32(&b[6]);
        audioStart = 10 + tagSize;
        size_t pos = 10;

        while (pos + 10 <= b.size() && pos < 10 + tagSize) {
            if (b[pos] == 0) break;  // 填充区
            std::string id((const char*)&b[pos], ver >= 4 ? 4 : 4);
            uint32_t fsize = ver >= 4 ? ss32(&b[pos + 4]) : be32(&b[pos + 4]);
            size_t data = pos + 10;
            if (fsize == 0 || data + fsize > b.size()) break;

            const unsigned char* p = &b[data];
            if (id == "TIT2") m.title = decodeText(p + 1, fsize - 1, p[0]);
            else if (id == "TPE1") m.artist = decodeText(p + 1, fsize - 1, p[0]);
            else if (id == "TALB") m.album = decodeText(p + 1, fsize - 1, p[0]);
            else if (id == "APIC" && m.coverLen == 0) {
                int enc = p[0];
                size_t i = 1;
                while (i < fsize && p[i]) i++;  // mime
                i++;                            // NUL
                i++;                            // 图片类型
                if (enc == 1 || enc == 2) {     // 描述是 UTF-16，按双字节找结束
                    while (i + 1 < fsize && (p[i] || p[i + 1])) i += 2;
                    i += 2;
                } else {
                    while (i < fsize && p[i]) i++;
                    i++;
                }
                if (i < fsize) {
                    m.coverOff = (long long)data + (long long)i;
                    m.coverLen = (long long)fsize - (long long)i;
                }
            }
            pos = data + fsize;
        }
    } else if (memcmp(b.data(), "fLaC", 4) == 0) {
        size_t pos = 4;
        while (pos + 4 <= b.size()) {
            int type = b[pos] & 0x7F;
            bool last = (b[pos] & 0x80) != 0;
            uint32_t len = be24(&b[pos + 1]);
            size_t data = pos + 4;
            if (data + len > b.size()) break;

            if (type == 0 && len >= 18) {  // STREAMINFO
                const unsigned char* p = &b[data];
                uint64_t packed = 0;
                for (int i = 10; i < 18; i++) packed = (packed << 8) | p[i];
                int sr = (int)((packed >> 44) & 0xFFFFF);
                uint64_t total = packed & 0xFFFFFFFFFULL;
                if (sr) m.durationMs = (long long)(total * 1000ULL / sr);
            } else if (type == 4) {  // VORBIS_COMMENT，长度全是小端
                const unsigned char* p = &b[data];
                size_t i = 0;
                if (i + 4 <= len) {
                    uint32_t vlen = le32(p + i);
                    i += 4 + vlen;
                }
                if (i + 4 <= len) {
                    uint32_t cnt = le32(p + i);
                    i += 4;
                    for (uint32_t k = 0; k < cnt && i + 4 <= len; k++) {
                        uint32_t l = le32(p + i);
                        i += 4;
                        if (i + l > len) break;
                        std::string kv((const char*)p + i, l);
                        i += l;
                        size_t eq = kv.find('=');
                        if (eq == std::string::npos) continue;
                        std::string key = kv.substr(0, eq);
                        for (auto& c : key) c = (char)toupper((unsigned char)c);
                        std::string val = kv.substr(eq + 1);
                        if (key == "TITLE") m.title = val;
                        else if (key == "ARTIST") m.artist = val;
                        else if (key == "ALBUM") m.album = val;
                    }
                }
            } else if (type == 6 && m.coverLen == 0) {  // PICTURE
                const unsigned char* p = &b[data];
                size_t i = 4;  // 图片类型
                if (i + 4 <= len) {
                    uint32_t ml = be32(p + i);
                    i += 4 + ml;
                }
                if (i + 4 <= len) {
                    uint32_t dl = be32(p + i);
                    i += 4 + dl;
                }
                i += 16;  // 宽高色深色数
                if (i + 4 <= len) {
                    uint32_t bl = be32(p + i);
                    i += 4;
                    if (i + bl <= len) {
                        m.coverOff = (long long)data + (long long)i;
                        m.coverLen = bl;
                    }
                }
            }
            pos = data + len;
            if (last) break;
        }
    }

    if (m.durationMs == 0 && b.size() > 4 && b[0] != 'f') m.durationMs = mp3Duration(b, fileSize, audioStart);

    // ID3v1 兜底
    if (m.title.empty() && fileSize > 128) {
        // 只在前面的 ID3v2 里没找到时才去看文件尾部
        HANDLE g = CreateFileW(wp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g != INVALID_HANDLE_VALUE) {
            unsigned char v1[128];
            LARGE_INTEGER off{};
            off.QuadPart = fileSize - 128;
            SetFilePointerEx(g, off, nullptr, FILE_BEGIN);
            DWORD n = 0;
            if (ReadFile(g, v1, 128, &n, nullptr) && n == 128 && memcmp(v1, "TAG", 3) == 0) {
                auto field = [&](int at, int len) {
                    std::string s((const char*)v1 + at, len);
                    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
                    return s;
                };
                std::string t = field(3, 30), a = field(33, 30), al = field(63, 30);
                if (m.title.empty()) m.title = decodeText((const unsigned char*)t.data(), t.size(), 0);
                if (m.artist.empty()) m.artist = decodeText((const unsigned char*)a.data(), a.size(), 0);
                if (m.album.empty()) m.album = decodeText((const unsigned char*)al.data(), al.size(), 0);
            }
            CloseHandle(g);
        }
    }
    return m;
}

}  // namespace tags
