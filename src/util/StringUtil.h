#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace oscore {

// 轻量字符串工具，保持 header-only，避免为了简单命令解析引入额外依赖。
inline std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline std::string trim(const std::string& value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();

    if (first >= last) {
        return {};
    }

    return std::string(first, last);
}

// 估算字符串在控制台中的显示宽度（等宽字体）。
// ASCII 可打印字符宽度=1，CJK 范围宽度≈2，控制字符宽度=0。
inline int displayWidth(const std::string& text) {
    int width = 0;
    for (std::size_t i = 0; i < text.size(); ) {
        const auto uc = static_cast<unsigned char>(text[i]);
        if (uc <= 0x1F || uc == 0x7F) {
            // 控制字符不计宽度
            ++i;
            continue;
        }
        if (uc <= 0x7E) {
            // ASCII 可打印
            width += 1;
            ++i;
            continue;
        }
        // UTF-8 多字节序列：首字节高位决定后续字节数
        int bytes = 1;
        if ((uc & 0xE0) == 0xC0) bytes = 2;
        else if ((uc & 0xF0) == 0xE0) bytes = 3;
        else if ((uc & 0xF8) == 0xF0) bytes = 4;
        // 制表符 U+2500-U+257F（├ └ │ 等）: 3 字节 UTF-8，显示宽度为 1
        if (bytes == 3 && uc == 0xE2 && i + 2 < text.size() &&
            (static_cast<unsigned char>(text[i + 1]) == 0x94 ||
             static_cast<unsigned char>(text[i + 1]) == 0x95)) {
            width += 1;
        } else if (bytes >= 3) {
            // CJK 统一汉字及常见全角符号范围近似宽度=2
            width += 2;
        } else if (bytes == 2) {
            // 两字节序列宽度保守估计为 1（拉丁扩展等）
            width += 1;
        } else {
            width += 1; // 无效字节，保守处理
        }
        i += static_cast<std::size_t>(bytes);
    }
    return width;
}

// 使用空格将字符串补齐到指定的显示宽度
inline std::string padRightDisplayWidth(const std::string& text, int targetWidth) {
    const int current = displayWidth(text);
    if (current >= targetWidth) {
        return text + ' ';
    }
    return text + std::string(static_cast<std::size_t>(targetWidth - current), ' ');
}

} // namespace oscore
