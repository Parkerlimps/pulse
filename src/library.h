// library.h —— 扫描音乐目录、建曲库
#pragma once

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "tags.h"
#include "util.h"

namespace lib {

struct Track {
    std::string path;
    std::string title, artist, album;
    long long durationMs = 0;
    long long coverOff = 0, coverLen = 0;
    std::string fmt;  // mp3 / flac / m4a ...
};

inline std::string extOf(const std::wstring& name) {
    size_t p = name.find_last_of(L'.');
    if (p == std::wstring::npos) return "";
    std::wstring e = name.substr(p + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return util::narrow(e);
}

inline bool isAudioExt(const std::string& e) {
    // 只收 Media Foundation 能解的（ape / dsd 系统不带解码器）
    return e == "mp3" || e == "flac" || e == "m4a" || e == "wav" || e == "aac" || e == "wma";
}

inline void walk(const std::wstring& dir, std::vector<std::wstring>& out, int depth) {
    if (depth > 8) return;
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = dir + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk(full, out, depth + 1);
        } else if (isAudioExt(extOf(fd.cFileName))) {
            out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

inline std::string stem(const std::string& path) {
    size_t a = path.find_last_of("\\/");
    std::string name = a == std::string::npos ? path : path.substr(a + 1);
    size_t d = name.find_last_of('.');
    return d == std::string::npos ? name : name.substr(0, d);
}

// 扫多个目录 → 曲库。同一个文件出现多次只留一条。
inline std::vector<Track> scan(const std::vector<std::string>& dirs) {
    std::vector<std::wstring> files;
    for (auto& d : dirs) {
        if (d.empty()) continue;
        if (!util::dirExists(d)) continue;
        walk(util::widen(d), files, 0);
    }
    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    files.erase(std::unique(files.begin(),
                            files.end(), [](const std::wstring& a, const std::wstring& b) {
                                return _wcsicmp(a.c_str(), b.c_str()) == 0;
                            }),
                files.end());

    std::vector<Track> out;
    out.reserve(files.size());
    for (auto& f : files) {
        Track t;
        t.path = util::narrow(f);
        t.fmt = extOf(f);
        auto m = tags::read(t.path);
        t.title = m.title;
        t.artist = m.artist;
        t.album = m.album;
        t.durationMs = m.durationMs;
        t.coverOff = m.coverOff;
        t.coverLen = m.coverLen;
        if (t.title.empty()) t.title = stem(t.path);
        out.push_back(t);
    }
    return out;
}

}  // namespace lib
