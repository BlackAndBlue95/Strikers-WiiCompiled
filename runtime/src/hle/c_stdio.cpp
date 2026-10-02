#include "hle_stubs.h"
#include "hle/guest_printf.h"
#include "memory.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

// Hardware boundary used by the translated MetroWerks stdio implementation.
// The original routine forwards completed FILE-buffer writes to UART/EXI or TRK.
// Keep __FileWrite (0x803833B4), __fwrite, and fwide translated so their buffering,
// orientation, short-write, and return-value semantics remain guest-owned.
extern "C" uint32_t ConsoleWrite_HLE_80389730(
    uint32_t handle, uint32_t bufferAddr, uint32_t lengthPtr, uint32_t refCon)
{
    (void)handle;
    (void)refCon;

    if (!lengthPtr) {
        return 1;
    }

    try {
        const uint32_t length = Memory::Read32(lengthPtr);
        if (length == 0) {
            return 0;
        }

        const uint8_t* data = Memory::GetPointer(bufferAddr, length);
        if (!data) {
            Memory::Write32(lengthPtr, 0);
            return 1;
        }

        return 0;
    } catch (const Memory::AccessViolation&) {
        try {
            Memory::Write32(lengthPtr, 0);
        } catch (const Memory::AccessViolation&) {
        }
        return 1;
    }
}

PPC_NATIVE_OVERRIDE(80389730, ConsoleWrite_HLE_80389730, uint32_t,
         (uint32_t handle, uint32_t bufferAddr, uint32_t lengthPtr, uint32_t refCon),
         (handle, bufferAddr, lengthPtr, refCon));



// --------------------------------------------------------------------------
// Shared guest printf formatter used by CPU-context stdio and OSReport paths.
// --------------------------------------------------------------------------
std::string RuntimeHle::FormatGuestPrintf(const std::string& fmt,
                                          size_t& outChars,
                                          const PrintfNext32& next32,
                                          const PrintfNext64& next64,
                                          const PrintfNextDouble& nextDouble,
                                          const PrintfReadString& readString)
{
    std::ostringstream out;
    outChars = 0;

    auto appendText = [&](const std::string& text) {
        out << text;
        outChars += text.size();
    };

    auto appendChar = [&](char ch) {
        out.put(ch);
        ++outChars;
    };

    size_t i = 0;
    while (i < fmt.size()) {
        if (fmt[i] != '%') {
            appendChar(fmt[i]);
            ++i;
            continue;
        }

        // Handle escaped %%
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            appendChar('%');
            i += 2;
            continue;
        }

        size_t cur = i + 1;
        std::string flags;
        while (cur < fmt.size() && std::strchr("-+ #0", fmt[cur])) {
            flags.push_back(fmt[cur]);
            ++cur;
        }

        bool widthFromArg = false;
        int width = -1;
        if (cur < fmt.size() && fmt[cur] == '*') {
            widthFromArg = true;
            ++cur;
        } else {
            int parsed = 0;
            bool seen = false;
            while (cur < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[cur]))) {
                seen = true;
                parsed = (parsed * 10) + (fmt[cur] - '0');
                ++cur;
            }
            if (seen) {
                width = parsed;
            }
        }

        bool precisionFromArg = false;
        int precision = -1;
        if (cur < fmt.size() && fmt[cur] == '.') {
            ++cur;
            if (cur < fmt.size() && fmt[cur] == '*') {
                precisionFromArg = true;
                ++cur;
            } else {
                int parsed = 0;
                bool seen = false;
                while (cur < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[cur]))) {
                    seen = true;
                    parsed = (parsed * 10) + (fmt[cur] - '0');
                    ++cur;
                }
                precision = seen ? parsed : 0; // "%.s" -> precision 0
            }
        }

        std::string length;
        if (cur < fmt.size()) {
            if (fmt[cur] == 'h') {
                length.push_back('h');
                ++cur;
                if (cur < fmt.size() && fmt[cur] == 'h') {
                    length.push_back('h');
                    ++cur;
                }
            } else if (fmt[cur] == 'l') {
                length.push_back('l');
                ++cur;
                if (cur < fmt.size() && fmt[cur] == 'l') {
                    length.push_back('l');
                    ++cur;
                }
            } else if (fmt[cur] == 'z' || fmt[cur] == 't' || fmt[cur] == 'j' || fmt[cur] == 'q' || fmt[cur] == 'L') {
                length.push_back(fmt[cur]);
                ++cur;
            }
        }

        if (widthFromArg) {
            width = static_cast<int32_t>(next32());
            if (width < 0) {
                flags.push_back('-');
                width = -width;
            }
        }
        if (precisionFromArg) {
            precision = static_cast<int32_t>(next32());
            if (precision < 0) {
                precision = -1; // Negative precision is treated as if it's omitted.
            }
        }

        if (cur >= fmt.size()) {
            appendText(fmt.substr(i));
            break;
        }

        char spec = fmt[cur];
        ++cur;

        // The guest is ILP32: long, size_t and ptrdiff_t are 32 bits, long long and intmax_t 64, and
        // long double is double. Each host conversion is rebuilt with the host type that holds what
        // was consumed, never the guest's length modifier (a host %ld reads 64 bits).
        const bool guest64 = length == "ll" || length == "j" || length == "q";
        auto hostPiece = [&](const char* hostLength, char hostSpec) {
            std::string piece = "%" + flags;
            if (width >= 0) piece += std::to_string(width);
            if (precision >= 0) piece += "." + std::to_string(precision);
            return piece + hostLength + hostSpec;
        };
        auto appendWithSnprintf = [&](const std::string& piece, auto value) {
            int needed = std::snprintf(nullptr, 0, piece.c_str(), value);
            if (needed <= 0) {
                return;
            }
            std::string buf(static_cast<size_t>(needed) + 1, '\0');
            std::snprintf(buf.data(), buf.size(), piece.c_str(), value);
            buf.resize(static_cast<size_t>(needed));
            appendText(buf);
        };
        // Guest wchar_t is a big-endian UTF-16 unit; the log is UTF-8.
        auto appendUtf8 = [](std::string& text, uint32_t codePoint) {
            if (codePoint < 0x80) {
                text.push_back(static_cast<char>(codePoint));
            } else if (codePoint < 0x800) {
                text.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            } else {
                text.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
                text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
        };
        const char* shortLength = length == "h" ? "h" : length == "hh" ? "hh" : "";

        switch (spec) {
            case 's': {
                uint32_t ptr = next32();
                if (length == "l") {
                    std::string text;
                    try {
                        for (uint32_t at = ptr; ptr != 0 && text.size() < 4096; at += 2) {
                            const uint16_t unit = Memory::Read16(at);
                            if (unit == 0) break;
                            appendUtf8(text, unit);
                        }
                    } catch (const Memory::AccessViolation&) {
                    }
                    appendWithSnprintf(hostPiece("", 's'), text.c_str());
                } else {
                    std::string guest = readString(ptr);
                    appendWithSnprintf(hostPiece("", 's'), guest.c_str());
                }
                break;
            }
            case 'c': {
                if (length == "l") {
                    std::string text;
                    appendUtf8(text, next32() & 0xFFFF);
                    precision = -1;
                    appendWithSnprintf(hostPiece("", 's'), text.c_str());
                } else {
                    char ch = static_cast<char>(next32() & 0xFF);
                    appendWithSnprintf(hostPiece("", 'c'), ch);
                }
                break;
            }
            case 'p': {
                uint32_t ptr = next32();
                appendWithSnprintf(hostPiece("", 'p'), reinterpret_cast<void*>(static_cast<uintptr_t>(ptr)));
                break;
            }
            case 'd':
            case 'i': {
                if (guest64) {
                    appendWithSnprintf(hostPiece("ll", spec), static_cast<long long>(static_cast<int64_t>(next64())));
                } else {
                    appendWithSnprintf(hostPiece(shortLength, spec), static_cast<int>(static_cast<int32_t>(next32())));
                }
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                if (guest64) {
                    appendWithSnprintf(hostPiece("ll", spec), static_cast<unsigned long long>(next64()));
                } else {
                    appendWithSnprintf(hostPiece(shortLength, spec), static_cast<unsigned>(next32()));
                }
                break;
            }
            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G':
            case 'a': case 'A': {
                double v = nextDouble();
                appendWithSnprintf(hostPiece("", spec), v);
                break;
            }
            case 'n': {
                // The count goes out at the width the modifier names, never past it.
                uint32_t ptr = next32();
                if (ptr != 0) {
                    if (length == "hh") Memory::Write8(ptr, static_cast<uint8_t>(outChars));
                    else if (length == "h") Memory::Write16(ptr, static_cast<uint16_t>(outChars));
                    else if (guest64) Memory::Write64(ptr, static_cast<uint64_t>(outChars));
                    else Memory::Write32(ptr, static_cast<uint32_t>(outChars));
                }
                break;
            }
            default: {
                // Unknown specifier, copy literally so we don't drop information.
                appendText(fmt.substr(i, cur - i));
                break;
            }
        }

        i = cur;
    }

    return out.str();
}

// 0x803D6F64 is WUD_DEBUGPrint in the PAL executable. Its retail body only
// performs the compiler-generated variadic prologue and returns; it does not
// format or emit text. Keep it on the ordinary translated path so its guest
// memory effects remain exact without inventing expensive host-side logging.
