// md2pdf.cpp - markdown -> PDF via headless Chrome (CDP over WebSocket)
// Port of the PHP project (ConvertCommand.php + PdfEngine.php).
// Markdown -> HTML: md4c (https://github.com/mity/md4c)
// Usage: md2pdf <markdown-file> [--output=<path>] [--header=<html>] [--footer=<html>] [--no-header] [--no-footer]

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "md4c/src/md4c-html.h" // extern "C" guarded

namespace fs = std::filesystem;

// ---------------------------------------------------------------- utilities

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static fs::path W(const std::wstring& w) { return fs::path(w); }

static std::string readWholeFileUtf8(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) s.erase(0, 3); // strip UTF-8 BOM
    return s;
}

static bool writeBytes(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    f.close();
    return f.good();
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

static std::string base64Encode(const unsigned char* d, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        if (i + 1 < n) o += T[(v >> 6) & 63]; else o += '=';
        if (i + 2 < n) o += T[v & 63]; else o += '=';
    }
    return o;
}

static std::string base64Decode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string o;
    o.reserve(s.size() / 4 * 3 + 3);
    int buf = 0, bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o += (char)((buf >> bits) & 0xFF);
        }
    }
    return o;
}

static uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- console output (same colors as PHP)

static void out(const std::string& s, const char* color) {
    std::fwrite(color, 1, strlen(color), stdout);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fwrite("\033[0m\n", 1, 5, stdout);
    std::fflush(stdout);
}
#define OUT_INFO(m) out(m, "\033[36m")
#define OUT_OK(m)   out(m, "\033[32m")
#define OUT_ERR(m)  out(m, "\033[31m")
#define OUT_WARN(m) out(m, "\033[33m")

// ---------------------------------------------------------------- markdown -> HTML (md4c)

namespace {
struct MdOut { std::string s; };
void mdAppendCb(const MD_CHAR* text, MD_SIZE size, void* userdata) {
    static_cast<MdOut*>(userdata)->s.append(text, size);
}
}

static std::string mdToHtml(const std::string& srcIn) {
    MdOut o;
    int rc = md_html(srcIn.data(), (MD_SIZE)srcIn.size(), &mdAppendCb, &o,
                     MD_DIALECT_GITHUB, MD_HTML_FLAG_SKIP_UTF8_BOM);
    if (rc != 0) throw std::runtime_error("md4c: markdown parse error");
    return o.s;
}

// ---------------------------------------------------------------- heading anchors

// md4c implements CommonMark plus the GitHub extensions, and neither of those
// defines heading anchors. `{#id}` comes from PHP Markdown Extra / kramdown /
// Pandoc, so md4c passes it through as literal text; and the slugged ids that make
// `[x](#some-heading)` work on github.com are generated by GitHub's renderer, not
// by the spec, so md4c emits a bare `<h2>`. Either way every in-document link in
// the PDF pointed at nothing. Add the ids here, after parsing.

static std::string stripTags(const std::string& h) {
    std::string o;
    bool inTag = false;
    for (char c : h) {
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag) o += c;
    }
    return o;
}

// towlower() only maps ASCII under the default "C" locale, which left 'Š' upper
// case while 'č' came through fine -- enough to break the one link. Map the whole
// string through the invariant locale instead, which is Unicode-aware and does not
// depend on what the machine is configured for.
static std::wstring toLowerW(const std::wstring& w) {
    if (w.empty()) return w;
    std::wstring o(w.size(), L'\0');
    int k = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.data(), (int)w.size(),
                          &o[0], (int)o.size(), nullptr, nullptr, 0);
    if (k <= 0) return w;
    o.resize((size_t)k);
    return o;
}

// GitHub-style slug: lowercase, spaces to '-', punctuation dropped. Letters and
// digits are kept whatever the script, so Slovenian headings keep their sumniki
// ("Šumniki v naslovu" -> "šumniki-v-naslovu").
static std::string slugify(const std::string& text) {
    std::wstring w = toLowerW(utf8ToWide(text)), o;
    for (wchar_t c : w) {
        if (iswalnum((wint_t)c)) o += c;
        else if (c == L'_') o += c;
        else if (c == L' ' || c == L'-' || c == L'\t') o += L'-';
    }
    std::wstring r;                         // collapse '-' runs, trim the ends
    for (wchar_t c : o) {
        if (c == L'-' && (r.empty() || r.back() == L'-')) continue;
        r += c;
    }
    while (!r.empty() && r.back() == L'-') r.pop_back();
    return wideToUtf8(r);
}

// Give every <h1>..<h6> an id. An explicit trailing {#id} wins and is removed from
// the visible text; otherwise the id is slugged from the heading. Duplicates get a
// -1, -2 suffix, as on GitHub. Headings that already carry attributes (raw HTML in
// the markdown) are left untouched.
static std::string addHeadingAnchors(const std::string& html) {
    std::string o;
    o.reserve(html.size() + 128);
    std::map<std::string, int> used;
    size_t last = 0;
    for (size_t pos = 0; (pos = html.find("<h", pos)) != std::string::npos;) {
        if (pos + 3 >= html.size() || html[pos + 2] < '1' || html[pos + 2] > '6' ||
            html[pos + 3] != '>') {
            pos += 2;
            continue;
        }
        const char level = html[pos + 2];
        const std::string closeTag = std::string("</h") + level + ">";
        const size_t innerBegin = pos + 4;
        const size_t innerEnd = html.find(closeTag, innerBegin);
        if (innerEnd == std::string::npos) { pos += 2; continue; }

        std::string inner = html.substr(innerBegin, innerEnd - innerBegin);
        std::string id;

        // explicit "{#id}" at the very end of the heading text
        if (!inner.empty() && inner.back() == '}') {
            size_t b = inner.find_last_of('{');
            if (b != std::string::npos && b + 2 < inner.size() && inner[b + 1] == '#') {
                std::string cand = inner.substr(b + 2, inner.size() - b - 3);
                if (!cand.empty() && cand.find_first_of(" \t<>\"'{}") == std::string::npos) {
                    id = cand;
                    inner = trim(inner.substr(0, b));
                }
            }
        }
        if (id.empty()) id = slugify(stripTags(inner));
        if (id.empty()) { pos = innerEnd + closeTag.size(); continue; }

        int& seen = used[id];
        const std::string finalId = seen ? id + "-" + std::to_string(seen) : id;
        seen++;

        o += html.substr(last, pos - last);
        o += "<h";
        o += level;
        o += " id=\"" + finalId + "\">" + inner + closeTag;
        last = innerEnd + closeTag.size();
        pos = last;
    }
    o += html.substr(last);
    return o;
}

// ---------------------------------------------------------------- media path resolution (mirrors PdfEngine.php)

static std::vector<std::string> missingMedia;

// Percent-encode everything a file:// URL cannot carry literally. Without this a
// path containing a space, '#' or '?' (or any non-ASCII byte) silently truncates
// or fails to load inside Chrome.
static std::string urlEncodePath(const std::string& p) {
    static const char* HEX = "0123456789ABCDEF";
    std::string o;
    o.reserve(p.size());
    for (unsigned char c : p) {
        if (c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~' ||
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            o += (char)c;
        } else {
            o += '%'; o += HEX[c >> 4]; o += HEX[c & 15];
        }
    }
    return o;
}

// Inverse of the above. md4c percent-encodes every link destination it emits, so
// an author's `![](a b.png|50%|center)` reaches us as `a%20b.png%7C50%%7Ccenter`:
// the '|' separators are hidden behind %7C and the path no longer matches a real
// file. Decode before splitting on '|' or touching the disk. A malformed escape
// is left as-is, so a bare '%' (as in the `50%` width spec) survives.
static std::string urlDecode(const std::string& p) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string o;
    o.reserve(p.size());
    for (size_t i = 0; i < p.size(); i++) {
        int h1, h2;
        if (p[i] == '%' && i + 2 < p.size() &&
            (h1 = hex(p[i + 1])) >= 0 && (h2 = hex(p[i + 2])) >= 0) {
            o += (char)(h1 * 16 + h2);
            i += 2;
        } else {
            o += p[i];
        }
    }
    return o;
}

static std::string resolvePath(const std::string& path, const fs::path& sourceDir) {
    // '#foo' is an in-document reference (SVG gradients, filters), never a file
    if (path.empty() || path[0] == '#') return path;
    if (path.rfind("data:", 0) == 0 || path.rfind("http://", 0) == 0 ||
        path.rfind("https://", 0) == 0 || path.rfind("//", 0) == 0 ||
        path.rfind("file:///", 0) == 0) {
        return path;
    }
    if (path.size() >= 3 && std::isalpha((unsigned char)path[0]) && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\\')) {
        std::string u = path;
        std::replace(u.begin(), u.end(), '\\', '/');
        return "file:///" + urlEncodePath(u);
    }
    fs::path p = sourceDir / utf8ToWide(path);
    std::error_code ec;
    fs::path real = fs::weakly_canonical(p, ec);
    if (ec || !fs::exists(real, ec)) {
        missingMedia.push_back(path);
        return path;
    }
    std::string u = wideToUtf8(real.wstring());
    std::replace(u.begin(), u.end(), '\\', '/');
    return "file:///" + urlEncodePath(u);
}

// width spec: 50% or 1.5% or 50%/2  (manual check, MSVC regex overflows on long strings)
static bool isWidthSpec(const std::string& p) {
    size_t i = 0;
    if (i >= p.size() || !isdigit((unsigned char)p[i])) return false;
    while (i < p.size() && isdigit((unsigned char)p[i])) i++;
    if (i < p.size() && p[i] == '.') {
        i++;
        if (i >= p.size() || !isdigit((unsigned char)p[i])) return false;
        while (i < p.size() && isdigit((unsigned char)p[i])) i++;
    }
    if (i >= p.size() || p[i] != '%') return false;
    i++;
    if (i < p.size()) { // optional /N
        if (p[i] != '/') return false;
        i++;
        if (i >= p.size() || !isdigit((unsigned char)p[i])) return false;
        while (i < p.size() && isdigit((unsigned char)p[i])) i++;
    }
    return i == p.size();
}

// manual case-insensitive find
static size_t findCI(const std::string& s, const char* needle, size_t from) {
    const size_t nl = strlen(needle);
    if (nl == 0 || s.size() < nl) return std::string::npos;
    const char n0l = (char)tolower((unsigned char)needle[0]);
    const char n0u = (char)toupper((unsigned char)needle[0]);
    for (size_t i = from; i + nl <= s.size(); i++) {
        if (s[i] != n0l && s[i] != n0u) continue; // cheap reject before the full compare
        size_t k = 1;
        for (; k < nl; k++)
            if (tolower((unsigned char)s[i + k]) != tolower((unsigned char)needle[k])) break;
        if (k == nl) return i;
    }
    return std::string::npos;
}

// mirror of PHP resolveImageTag, but takes positions: tag at `tagStart`, src value [valBegin,valEnd), quote char
static std::string buildImageReplacement(const std::string& html, size_t tagStart,
                                         size_t valBegin, size_t valEnd, char quote,
                                         const fs::path& sourceDir) {
    std::string rawSrc = trim(html.substr(valBegin, valEnd - valBegin));
    // Leave data: URIs alone -- their payload may legitimately contain '%'.
    if (rawSrc.rfind("data:", 0) != 0) rawSrc = urlDecode(rawSrc);
    std::string parsedSrc = rawSrc;
    std::string width, align;

    // pipe syntax: path|50%|center  (same as PHP resolveImageTag)
    if (rawSrc.find('|') != std::string::npos) {
        std::vector<std::string> parts;
        std::istringstream ss(rawSrc);
        std::string part;
        while (std::getline(ss, part, '|')) parts.push_back(part);
        parsedSrc = trim(parts[0]);
        for (size_t i = 1; i < parts.size(); i++) {
            std::string p = trim(parts[i]);
            if (isWidthSpec(p)) width = p;
            else {
                std::string lp = toLower(p);
                if (lp == "left" || lp == "center" || lp == "right" ||
                    lp == "float:left" || lp == "float:right") align = lp;
            }
        }
    }

    std::string src = resolvePath(parsedSrc, sourceDir);
    std::string style;
    if (!width.empty()) style += "width:" + width + ";";
    if (!align.empty()) {
        if (align.rfind("float:", 0) == 0) {
            std::string dir = align.substr(6);
            style += "float:" + dir + ";margin-" + dir + ":0;margin-" +
                     (dir == "left" ? "right" : "left") + ":12px;";
        } else if (align == "center") {
            style += "display:block;margin-left:auto;margin-right:auto;";
        } else if (align == "right") {
            style += "display:block;margin-left:auto;margin-right:0;";
        } else {
            style += "display:block;margin-left:0;margin-right:auto;";
        }
    }

    // everything from <img up to src value start + resolved value + quote (+style)
    std::string r = html.substr(tagStart, valBegin - tagStart) + src + quote;
    if (!style.empty()) r += " style=\"" + style + "\"";
    return r;
}

// manual scan, no std::regex (MSVC regex overflows its stack on long base64 data URIs)
static std::string resolveMediaPaths(const std::string& html, const fs::path& sourceDir) {
    std::string h;
    h.reserve(html.size());
    size_t last = 0;
    for (size_t pos = 0; (pos = findCI(html, "<img", pos)) != std::string::npos;) {
        size_t tagEnd = html.find('>', pos);
        if (tagEnd == std::string::npos) break;
        // find src= inside the tag
        size_t s = pos + 4, t = std::string::npos;
        while (s < tagEnd) {
            if ((html[s] == 's' || html[s] == 'S') &&
                (s + 2 <= tagEnd) && (html[s + 1] == 'r' || html[s + 1] == 'R') &&
                (html[s + 2] == 'c' || html[s + 2] == 'C')) {
                size_t u = s + 3;
                while (u < tagEnd && isspace((unsigned char)html[u])) u++;
                if (u < tagEnd && html[u] == '=') { t = u + 1; break; }
            }
            s++;
        }
        if (t == std::string::npos) { pos = tagEnd + 1; continue; }
        while (t < tagEnd && isspace((unsigned char)html[t])) t++;
        if (t >= tagEnd || (html[t] != '"' && html[t] != '\'')) { pos = tagEnd + 1; continue; }
        char quote = html[t];
        size_t valBegin = t + 1;
        size_t valEnd = html.find(quote, valBegin);
        if (valEnd == std::string::npos || valEnd > tagEnd) { pos = tagEnd + 1; continue; }

        h += html.substr(last, pos - last);
        h += buildImageReplacement(html, pos, valBegin, valEnd, quote, sourceDir);
        last = valEnd + 1;
        pos = last;
    }
    h += html.substr(last);

    // url(...) in CSS
    std::string h2;
    h2.reserve(h.size());
    last = 0;
    for (size_t pos = 0; (pos = findCI(h, "url(", pos)) != std::string::npos;) {
        size_t t = pos + 4;
        char quote = 0;
        if (t < h.size() && (h[t] == '"' || h[t] == '\'')) { quote = h[t]; t++; }
        size_t valBegin = t;
        size_t valEnd = quote
            ? h.find(quote, valBegin)
            : h.find(')', valBegin);
        if (valEnd == std::string::npos) break;
        std::string raw = trim(h.substr(valBegin, valEnd - valBegin));
        if (raw.rfind("data:", 0) != 0) raw = urlDecode(raw);
        std::string resolved = raw.empty() ? raw : resolvePath(raw, sourceDir);

        h2 += h.substr(last, pos - last);
        h2 += "url(";
        if (quote) h2 += quote;
        h2 += resolved;
        if (quote) h2 += quote;
        h2 += ")";
        last = valEnd + 1;
        pos = last;
    }
    h2 += h.substr(last);
    return h2;
}

// ---------------------------------------------------------------- config (mirrors config/app.php)

struct Config {
    std::wstring chromeBinary = L"C:\\Users\\Miha Nahtigal\\AppData\\Local\\Chromium\\Application\\chrome.exe";
    int timeout = 120;
    std::string pageSize = "A4";
    std::string orientation = "portrait";
    double marginTop = 20, marginRight = 10, marginBottom = 25, marginLeft = 25;
};

static std::wstring findChrome(const Config& cfg) {
    std::vector<std::wstring> candidates{ cfg.chromeBinary };
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (len) {
        std::vector<wchar_t> local(len);
        DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), len);
        if (got && got < len) {
            std::wstring base(local.data(), got);
            candidates.push_back(base + L"\\Google\\Chrome\\Application\\chrome.exe");
            candidates.push_back(base + L"\\Chromium\\Application\\chrome.exe");
        }
    }
    candidates.push_back(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");
    candidates.push_back(L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe");
    for (auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(W(c), ec)) return c;
    }
    return L"chrome.exe"; // last resort: PATH
}

// ---------------------------------------------------------------- CDP over WebSocket

static SOCKET tcpConnect(u_short port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(s); return INVALID_SOCKET; }
    struct timeval tv{ 5, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    return s;
}

static void recvAll(SOCKET s, std::string& out, int maxBytes = 1 << 22) {
    char buf[8192];
    while ((int)out.size() < maxBytes) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, n);
    }
}

// bind to 127.0.0.1:0, ask OS for a free port, release it
static int pickFreePort() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) { closesocket(s); return -1; }
    sockaddr_in bound{};
    int len = sizeof(bound);
    if (getsockname(s, (sockaddr*)&bound, &len) != 0) { closesocket(s); return -1; }
    int p = ntohs(bound.sin_port);
    closesocket(s);
    return p;
}

static std::string httpGet(u_short port, const std::string& path) {
    SOCKET s = tcpConnect(port);
    if (s == INVALID_SOCKET) return "";
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                      "\r\nConnection: close\r\n\r\n";
    send(s, req.data(), (int)req.size(), 0);
    std::string resp;
    recvAll(s, resp);
    closesocket(s);
    return resp;
}

struct Ws { SOCKET sock = INVALID_SOCKET; };

static bool wsHandshake(Ws& ws, u_short port, const std::string& path) {
    ws.sock = tcpConnect(port);
    if (ws.sock == INVALID_SOCKET) return false;
    unsigned char key[16];
    std::random_device rd;
    for (auto& b : key) b = (unsigned char)rd();
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + base64Encode(key, 16) +
                      "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (send(ws.sock, req.data(), (int)req.size(), 0) == SOCKET_ERROR) {
        closesocket(ws.sock); ws.sock = INVALID_SOCKET; return false;
    }
    // Read exactly the response headers and no further. recvAll() reads until the
    // peer closes, but after a 101 the server holds the connection open, so it
    // used to stall for the whole socket timeout on every run -- and could eat
    // the leading bytes of the first CDP frames.
    std::string resp;
    char ch;
    while (resp.size() < 8192 && resp.find("\r\n\r\n") == std::string::npos) {
        int k = recv(ws.sock, &ch, 1, 0);
        if (k <= 0) break;
        resp += ch;
    }
    if (resp.find(" 101") == std::string::npos) {
        closesocket(ws.sock); ws.sock = INVALID_SOCKET; return false;
    }
    return true;
}

static bool wsSendFrame(SOCKET sock, unsigned char opcode, const std::string& payload) {
    std::vector<unsigned char> frame;
    frame.push_back(0x80 | opcode);
    size_t n = payload.size();
    if (n < 126) {
        frame.push_back(0x80 | (unsigned char)n);
    } else if (n < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back((n >> 8) & 0xFF);
        frame.push_back(n & 0xFF);
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--) frame.push_back((n >> (i * 8)) & 0xFF);
    }
    unsigned char mask[4];
    std::random_device rd;
    for (auto& b : mask) b = (unsigned char)rd();
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < n; i++) frame.push_back((unsigned char)(payload[i] ^ mask[i % 4]));
    size_t off = 0;
    while (off < frame.size()) {
        int r = send(sock, (const char*)frame.data() + off, (int)(frame.size() - off), 0);
        if (r <= 0) return false;
        off += r;
    }
    return true;
}

// deadline: absolute nowMs() value; 0 means wait forever
static bool wsRecvN(SOCKET sock, unsigned char* buf, size_t n, uint64_t deadline) {
    size_t off = 0;
    while (off < n) {
        int r = recv(sock, (char*)buf + off, (int)(n - off), 0);
        if (r == 0) return false;                       // closed
        if (r < 0) {
            int e = WSAGetLastError();
            // a recv timeout only means "nothing yet": keep waiting until the deadline
            if (e == WSAETIMEDOUT && (deadline == 0 || nowMs() < deadline)) continue;
            return false;
        }
        off += r;
    }
    return true;
}

static void wsClose(Ws& ws) {
    if (ws.sock == INVALID_SOCKET) return;
    wsSendFrame(ws.sock, 0x8, "");
    closesocket(ws.sock);
    ws.sock = INVALID_SOCKET;
}

// Returns one full (reassembled) text/binary message; false on close/error/deadline.
static bool wsRecvMessage(Ws& ws, std::string& msg, uint64_t deadline) {
    msg.clear();
    for (;;) {
        unsigned char hdr[2];
        if (!wsRecvN(ws.sock, hdr, 2, deadline)) return false;
        bool fin = hdr[0] & 0x80;
        unsigned char opcode = hdr[0] & 0x0F;
        int masked = hdr[1] & 0x80;
        uint64_t len = hdr[1] & 0x7F;
        if (len == 126) {
            unsigned char b[2];
            if (!wsRecvN(ws.sock, b, 2, deadline)) return false;
            len = ((uint64_t)b[0] << 8) | b[1];
        } else if (len == 127) {
            unsigned char b[8];
            if (!wsRecvN(ws.sock, b, 8, deadline)) return false;
            len = 0; // the 0x7F marker must not stay in the accumulator
            for (int i = 0; i < 8; i++) len = (len << 8) | b[i];
        }
        if (len > (uint64_t)512 * 1024 * 1024) return false; // refuse absurd frames
        unsigned char mask[4] = {0};
        if (masked && !wsRecvN(ws.sock, mask, 4, deadline)) return false;
        std::string payload((size_t)len, '\0');
        for (size_t off = 0; off < (size_t)len;) {
            size_t chunk = std::min<size_t>((size_t)len - off, 65536);
            if (!wsRecvN(ws.sock, (unsigned char*)&payload[off], chunk, deadline)) return false;
            off += chunk;
        }
        if (masked) for (size_t i = 0; i < len; i++) payload[i] ^= mask[i % 4];

        switch (opcode) {
            case 0x0:                                   // continuation
                msg += payload;
                if (fin) return true;
                continue;
            case 0x1: case 0x2:                         // text / binary
                // A non-final first frame must be completed by continuation
                // frames; returning `fin` here reported a fragmented reply as a
                // connection error.
                msg = payload;
                if (fin) return true;
                continue;
            case 0x8: wsClose(ws); return false;        // close
            case 0x9: wsSendFrame(ws.sock, 0xA, payload); continue;         // ping -> pong
            case 0xA: continue;                         // pong
            default:  continue;                         // unknown opcode: ignore
        }
    }
}

// True when `msg` carries this exact command id. Plain substring matching is not
// enough: searching for "id":1 would also hit "id":13.
static bool isReplyTo(const std::string& msg, int id) {
    const std::string key = "\"id\":" + std::to_string(id);
    for (size_t p = 0; (p = msg.find(key, p)) != std::string::npos; p += key.size()) {
        size_t after = p + key.size();
        if (after >= msg.size() || !isdigit((unsigned char)msg[after])) return true;
    }
    return false;
}

// Send one CDP command and wait for its reply, skipping the events that arrive in
// between. Returns "" if the connection dies or the reply does not arrive in time.
static std::string cdpCall(Ws& ws, int id, const std::string& method,
                           const std::string& params, uint64_t timeoutMs = 120000) {
    if (ws.sock == INVALID_SOCKET) return "";
    std::string req = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + method +
                      "\",\"params\":" + params + "}";
    if (!wsSendFrame(ws.sock, 0x1, req)) return "";
    const uint64_t deadline = nowMs() + timeoutMs;
    for (;;) {
        std::string msg;
        if (!wsRecvMessage(ws, msg, deadline)) return "";   // closed, error or timed out
        if (isReplyTo(msg, id) &&
            (msg.find("\"result\"") != std::string::npos ||
             msg.find("\"error\"") != std::string::npos)) {
            return msg;
        }
        if (nowMs() >= deadline) return "";
    }
}

// ---------------------------------------------------------------- temp / profile dirs

static std::wstring tmpDir() {
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    return buf;
}

static std::string randHex(size_t n) {
    static const char* T = "0123456789abcdef";
    std::random_device rd;
    std::string s;
    for (size_t i = 0; i < n; i++) s += T[rd() & 15];
    return s;
}

// Sweep anything this tool left in %TEMP% more than an hour ago: profile dirs,
// the temp source HTML and the stderr logs (only profiles were cleaned before,
// so the .html and .log files accumulated forever).
static void cleanupOldTemp(const fs::path& tmp) {
    std::error_code ec;
    if (!fs::is_directory(tmp, ec)) return;
    const auto nowF = fs::file_time_type::clock::now();
    for (auto it = fs::directory_iterator(tmp, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const fs::path q = it->path();
        // compare as wstring: filename().string() throws on undecodable names
        if (q.filename().wstring().rfind(L"md2pdf-", 0) != 0) continue;
        std::error_code e2;
        auto wt = it->last_write_time(e2);
        if (e2) continue;
        if (std::chrono::duration_cast<std::chrono::seconds>(nowF - wt).count() <= 3600) continue;
        if (it->is_directory(e2)) fs::remove_all(q, e2);
        else fs::remove(q, e2);
    }
}

// ---------------------------------------------------------------- convert (mirrors PdfEngine::convert)

static std::pair<double, double> paperSize(const std::string& ps) {
    static const std::map<std::string, std::pair<double, double>> sizes = {
        {"A4", {8.27, 11.69}}, {"A3", {11.69, 16.54}}, {"A5", {5.83, 8.27}},
        {"LETTER", {8.5, 11.0}}, {"LEGAL", {8.5, 14.0}} };
    std::string up = ps;
    std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    auto it = sizes.find(up);
    return it != sizes.end() ? it->second : std::pair<double, double>(8.27, 11.69);
}

static double mmToInch(double mm) { return std::round(mm / 25.4 * 1000.0) / 1000.0; }

struct ConvertResult { bool ok = false; std::string error; };

// Read whatever Chrome printed, for error messages. Must run before finishChrome.
static std::string chromeDiag(const std::wstring& errFile) {
    std::string d = trim(readWholeFileUtf8(W(errFile)));
    if (d.size() > 600) d = "..." + d.substr(d.size() - 600);
    return d.empty() ? std::string() : "\n  chrome: " + d;
}

static void finishChrome(PROCESS_INFORMATION& pi, HANDLE job, const std::wstring& profileDir,
                         const std::wstring& sourceFile, const std::wstring& errFile) {
    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0)
        TerminateProcess(pi.hProcess, 1);

    // Killing the browser process does not kill its renderers: one wedged in a
    // runaway script outlives its parent and shows up as a stray chrome.exe.
    // Terminating the job takes the whole tree down, and the files below stay
    // locked until it is really gone -- so wait for the count to reach zero
    // rather than deleting into a half-dead process tree.
    if (job) {
        TerminateJobObject(job, 1);
        for (int i = 0; i < 200; i++) { // up to 10s
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc{};
            DWORD ret = 0;
            if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                           &acc, sizeof(acc), &ret) ||
                acc.ActiveProcesses == 0) {
                break;
            }
            Sleep(50);
        }
        CloseHandle(job);
    } else {
        WaitForSingleObject(pi.hProcess, 5000);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Retry all three: a handle can linger for a moment after the process dies.
    std::error_code ec;
    for (int attempt = 0; attempt < 40; attempt++) { // up to 4s
        bool left = false;
        if (fs::exists(W(profileDir), ec)) { fs::remove_all(W(profileDir), ec); left = true; }
        if (fs::exists(W(sourceFile), ec)) { fs::remove(W(sourceFile), ec);     left = true; }
        if (fs::exists(W(errFile), ec))    { fs::remove(W(errFile), ec);        left = true; }
        if (!left) break;
        if (!fs::exists(W(profileDir), ec) && !fs::exists(W(sourceFile), ec) &&
            !fs::exists(W(errFile), ec)) {
            break;
        }
        Sleep(100);
    }
}

static ConvertResult convert(const std::string& html, const fs::path& outFile,
                             const std::string& headerTemplate, const std::string& footerTemplate,
                             const Config& cfg) {
    ConvertResult r;
    cleanupOldTemp(W(tmpDir()));

    std::wstring profileDir = tmpDir() + L"md2pdf-chrome-" + utf8ToWide(randHex(16));
    std::wstring sourceFile = tmpDir() + L"md2pdf-cdp-" + utf8ToWide(randHex(16)) + L".html";

    { std::error_code ec; fs::remove(outFile, ec); }
    if (!writeBytes(sourceFile, html)) { r.error = "cannot write temp HTML: " + wideToUtf8(sourceFile); return r; }

    // --- launch chrome with remote debugging on a port we picked ourselves.
    // (parsing "DevTools listening" from stderr is unreliable: Chrome only writes it
    // when attached to a console, which CREATE_NO_WINDOW processes are not)
    int port = pickFreePort();
    if (port <= 0) { r.error = "cannot find free TCP port"; return r; }

    std::wstring cmdLine = L"\"" + findChrome(cfg) + L"\" --headless --no-sandbox --disable-extensions "
                           L"--hide-scrollbars --no-first-run --no-default-browser-check "
                           L"--allow-file-access-from-files --do-not-de-elevate --user-data-dir=" + profileDir +
                           L" --remote-debugging-port=" + std::to_wstring(port);

    // stderr -> temp file (diagnostics only)
    std::wstring errFile = tmpDir() + L"md2pdf-chrome-stderr-" + utf8ToWide(randHex(16)) + L".log";
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE; // a non-inheritable handle cannot be a child's stderr
    HANDLE hErr = CreateFileW(errFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    // Every Chrome process must land in one job object so that nothing can be
    // orphaned: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE terminates the whole tree when
    // the last handle closes, including if md2pdf itself is killed.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (hErr != INVALID_HANDLE_VALUE) {
        // hStdError is ignored unless STARTF_USESTDHANDLES is set -- without it the
        // redirect silently did nothing and the log was always empty. The flag
        // applies to all three handles, so point stdout at the log as well rather
        // than letting Chrome write into our console.
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hErr;
        si.hStdError  = hErr;
    }
    PROCESS_INFORMATION pi{};
    // Start suspended so the process can join the job before it spawns children.
    if (!CreateProcessW(nullptr, (LPWSTR)cmdLine.c_str(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        r.error = "failed to launch Chrome (error " + std::to_string(GetLastError()) + ")";
        if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        if (job) CloseHandle(job);
        return r;
    }
    if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
    if (job) AssignProcessToJobObject(job, pi.hProcess); // children inherit the job
    ResumeThread(pi.hThread);

    // poll the DevTools HTTP endpoint until Chrome answers
    uint64_t start = nowMs();
    bool up = false;
    while (nowMs() - start < (uint64_t)cfg.timeout * 1000) {
        if (WaitForSingleObject(pi.hProcess, 300) == WAIT_OBJECT_0) break;
        std::string v = httpGet((u_short)port, "/json/version");
        if (!v.empty() && v.find("webSocketDebuggerUrl") != std::string::npos) { up = true; break; }
    }
    if (!up) {
        std::string diag = chromeDiag(errFile);
        finishChrome(pi, job, profileDir, sourceFile, errFile);
        r.error = "Chrome did not open DevTools port" + diag;
        return r;
    }

    // --- find page target (JSON may or may not have spaces after colons)
    std::string list = httpGet((u_short)port, "/json/list");
    std::string wsPath;
    size_t pos = 0;
    while ((pos = list.find("\"type\"", pos)) != std::string::npos) {
        size_t p = pos + 6;
        while (p < list.size() && isspace((unsigned char)list[p])) p++;
        if (p < list.size() && list[p] == ':') p++;
        while (p < list.size() && isspace((unsigned char)list[p])) p++;
        if (p + 6 <= list.size() && list.compare(p, 6, "\"page\"") == 0) {
            size_t wp = list.find("webSocketDebuggerUrl", pos);
            if (wp != std::string::npos) {
                size_t colon = list.find(':', wp + 20);
                size_t a = list.find('"', colon + 1);
                size_t b = list.find('"', a + 1);
                if (a != std::string::npos && b != std::string::npos) { wsPath = list.substr(a + 1, b - a - 1); break; }
            }
        }
        pos += 6;
    }
    size_t slash = wsPath.find("devtools/");
    if (slash == std::string::npos) {
        std::string diag = chromeDiag(errFile);
        finishChrome(pi, job, profileDir, sourceFile, errFile);
        r.error = "no DevTools page target found" + diag;
        return r;
    }
    std::string wsWsPath = "/" + wsPath.substr(slash);

    Ws ws;
    if (!wsHandshake(ws, (u_short)port, wsWsPath)) {
        std::string diag = chromeDiag(errFile);
        finishChrome(pi, job, profileDir, sourceFile, errFile);
        r.error = "WebSocket handshake failed" + diag;
        return r;
    }

    const uint64_t cdpTimeout = (uint64_t)cfg.timeout * 1000;
    int id = 1;
    if (cdpCall(ws, id++, "Page.enable", "{}", cdpTimeout).empty()) {
        std::string diag = chromeDiag(errFile);
        wsClose(ws);
        finishChrome(pi, job, profileDir, sourceFile, errFile);
        r.error = "Chrome did not answer Page.enable" + diag;
        return r;
    }

    // --- navigate to temp html file
    {
        std::string u = wideToUtf8(sourceFile);
        std::replace(u.begin(), u.end(), '\\', '/');
        std::string resp = cdpCall(ws, id++, "Page.navigate",
                                   "{\"url\":\"file:///" + jsonEscape(urlEncodePath(u)) + "\"}",
                                   cdpTimeout);
        if (resp.empty() || resp.find("\"error\"") != std::string::npos) {
            std::string diag = chromeDiag(errFile);
            wsClose(ws);
            finishChrome(pi, job, profileDir, sourceFile, errFile);
            r.error = (resp.empty() ? "Page.navigate got no reply" : "Page.navigate failed") + diag;
            return r;
        }
    }

    // --- wait for load complete
    {
        uint64_t dl = nowMs() + (uint64_t)cfg.timeout * 1000;
        bool loaded = false, dead = false;
        // A stray `if ((polls++ % 20) == 0)` used to swallow this test on 19 of
        // every 20 polls, so a page that was ready immediately still waited ~5s.
        // Also hold off until webfonts have settled, or the first page can print
        // with fallback metrics.
        const char* expr =
            "{\"expression\":\"(document.readyState==='complete'&&"
            "(!document.fonts||document.fonts.status==='loaded'))?'complete':document.readyState\","
            "\"returnByValue\":true}";
        while (nowMs() < dl) {
            std::string resp = cdpCall(ws, id++, "Runtime.evaluate", expr, cdpTimeout);
            if (resp.empty()) { dead = true; break; }
            if (resp.find("\"value\":\"complete\"") != std::string::npos) { loaded = true; break; }
            Sleep(50);
        }
        if (!loaded) {
            std::string diag = chromeDiag(errFile);
            wsClose(ws);
            finishChrome(pi, job, profileDir, sourceFile, errFile);
            r.error = (dead ? "lost the CDP connection while waiting for page load"
                            : "page load timeout") + diag;
            return r;
        }
    }

    // --- printToPDF
    {
        auto sz = paperSize(cfg.pageSize);
        char b1[32], b2[32], b3[32], b4[32], pwB[32], phB[32];
        snprintf(b1, sizeof b1, "%.3f", mmToInch(cfg.marginTop));
        snprintf(b2, sizeof b2, "%.3f", mmToInch(cfg.marginRight));
        snprintf(b3, sizeof b3, "%.3f", mmToInch(cfg.marginBottom));
        snprintf(b4, sizeof b4, "%.3f", mmToInch(cfg.marginLeft));
        snprintf(pwB, sizeof pwB, "%.2f", sz.first);
        snprintf(phB, sizeof phB, "%.2f", sz.second);

        bool displayHF = !headerTemplate.empty() || !footerTemplate.empty();
        std::string params = "{\"printBackground\":true,\"displayHeaderFooter\":" +
                             std::string(displayHF ? "true" : "false") +
                             ",\"preferCSSPageSize\":false,"
                             "\"paperWidth\":" + pwB + ",\"paperHeight\":" + phB +
                             ",\"marginTop\":" + b1 + ",\"marginRight\":" + b2 +
                             ",\"marginBottom\":" + b3 + ",\"marginLeft\":" + b4;
        if (toLower(cfg.orientation) == "landscape") params += ",\"landscape\":true";
        if (displayHF) {
            // Chrome falls back to its built-in template for the unset side: always send both
            params += ",\"headerTemplate\":\"" + jsonEscape(headerTemplate.empty() ? "<span></span>" : headerTemplate) + "\"";
            params += ",\"footerTemplate\":\"" + jsonEscape(footerTemplate.empty() ? "<span></span>" : footerTemplate) + "\"";
        }
        params += "}";

        std::string resp = cdpCall(ws, id++, "Page.printToPDF", params, cdpTimeout);
        size_t dp = resp.find("\"data\":\"");
        if (resp.empty() || dp == std::string::npos ||
            resp.find("\"error\"") != std::string::npos) {
            std::string diag = chromeDiag(errFile);
            wsClose(ws);
            finishChrome(pi, job, profileDir, sourceFile, errFile);
            // the old message dropped the response whenever it had been parsed at all
            r.error = "Page.printToPDF failed: " +
                      (resp.empty() ? std::string("no reply") : resp.substr(0, 300)) + diag;
            return r;
        }
        size_t de = dp + 8; // "data":" is 8 chars
        size_t dend = resp.find('"', de);
        if (dend == std::string::npos) {
            wsClose(ws);
            finishChrome(pi, job, profileDir, sourceFile, errFile);
            r.error = "bad printToPDF response";
            return r;
        }
        std::string pdf = base64Decode(resp.substr(de, dend - de));

        wsClose(ws);
        finishChrome(pi, job, profileDir, sourceFile, errFile);

        if (!writeBytes(outFile, pdf)) { r.error = "cannot write PDF: " + outFile.string(); return r; }
        r.ok = true;
    }
    return r;
}

// ---------------------------------------------------------------- template arguments

// A --header/--body/--footer value is either the path of a template file or the
// template markup itself. The file wins: that is what these flags are for. Markup
// is only assumed when the value cannot be a path but does look like HTML, which
// keeps the older `--header="<div>...</div>"` form working while stopping a
// mistyped path from being silently pasted into the PDF.
static bool loadTemplateArg(const std::string& value, const char* what, std::string& outHtml) {
    std::error_code ec;
    fs::path f(utf8ToWide(value));
    if (fs::is_regular_file(f, ec)) {
        outHtml = readWholeFileUtf8(f);
        OUT_INFO(std::string(what) + ": " + value);
        return true;
    }
    if (value.find('<') != std::string::npos) { // inline markup, not a path
        outHtml = value;
        return true;
    }
    OUT_ERR(std::string(what) + " template not found: " + value);
    return false;
}

// ---------------------------------------------------------------- main (mirrors ConvertCommand.php)

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hOut, &mode))
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    fs::path exeDir;
    {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        exeDir = (fs::path(buf)).parent_path();
    }

    int code = 1;
    if (argc < 2) {
        OUT_ERR("Usage: md2pdf <markdown-file> [--output=<path>]\n"
                "         [--header=<file|html>]  header template   (default templates/header.html)\n"
                "         [--body=<file>]         page template     (default templates/pdf.html)\n"
                "         [--footer=<file|html>]  footer template   (default templates/footer.html)\n"
                "         [--no-header] [--no-footer] [--html-out=<path>] [--timeout=<seconds>]");
        WSACleanup();
        return 1;
    }

    if (std::wstring(argv[1]).rfind(L"--", 0) == 0) {
        OUT_ERR("First argument must be the markdown file, not an option: " +
                wideToUtf8(std::wstring(argv[1])));
        WSACleanup();
        return 1;
    }

    fs::path inputFile(argv[1]);
    std::error_code ec;
    if (!fs::exists(inputFile, ec)) {
        OUT_ERR("File not found: " + wideToUtf8(std::wstring(argv[1])));
        WSACleanup();
        return 1;
    }

    std::string outputFile, headerOverride, bodyOverride, footerOverride, htmlOutPath;
    bool noHeader = false, noFooter = false;
    int timeoutOverride = 0;
    for (int i = 2; i < argc; i++) {
        std::string a = wideToUtf8(std::wstring(argv[i]));
        if (a.rfind("--output=", 0) == 0) outputFile = a.substr(9);
        else if (a.rfind("--header=", 0) == 0) headerOverride = a.substr(9);
        else if (a.rfind("--footer=", 0) == 0) footerOverride = a.substr(9);
        else if (a.rfind("--body=", 0) == 0) bodyOverride = a.substr(7);
        else if (a == "--no-header") noHeader = true;
        else if (a == "--no-footer") noFooter = true;
        else if (a.rfind("--html-out=", 0) == 0) htmlOutPath = a.substr(11);
        else if (a.rfind("--timeout=", 0) == 0) timeoutOverride = atoi(a.c_str() + 10);
        else OUT_WARN("Ignoring unknown option: " + a);
    }

    OUT_INFO("Input:  " + wideToUtf8(std::wstring(argv[1])));

    // destination: --output= value, or a Save As dialog when omitted
    fs::path outPath;
    if (!htmlOutPath.empty()) {
        // debug dump only: no PDF destination is needed
    } else if (!outputFile.empty()) {
        outPath = fs::path(utf8ToWide(outputFile));
    } else {
        wchar_t fileBuf[MAX_PATH] = {};
        std::wstring defName = inputFile.stem().wstring() + L".pdf";
        if (defName.size() < MAX_PATH) wcscpy_s(fileBuf, MAX_PATH, defName.c_str());

        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hInstance = GetModuleHandleW(NULL);
        ofn.lpstrFilter = L"PDF documents\0*.pdf\0All files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"Save PDF as";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

        if (!GetSaveFileNameW(&ofn)) {
            // cancelled (or no common dialog) - abort without error
            OUT_INFO("No destination chosen - aborted.");
            WSACleanup();
            return 0;
        }
        outPath = fs::path(fileBuf);
    }

    if (!outPath.empty()) OUT_INFO("Output: " + outPath.string());

    std::string markdownContent = readWholeFileUtf8(inputFile);

    // markdown -> HTML (md4c)
    std::string htmlContent;
    try {
        htmlContent = mdToHtml(markdownContent);
    } catch (const std::exception& e) {
        OUT_ERR(std::string("Markdown parse error: ") + e.what());
        WSACleanup();
        return 1;
    }

    // headings get ids so that in-document links resolve inside the PDF
    htmlContent = addHeadingAnchors(htmlContent);

    // page break markers -> CSS page-break divs
    {
        static const char* kMarker = "<!-- NEW PAGE -->";
        static const char* kBreak  = "<div style=\"page-break-after:always\"></div>";
        const size_t ml = strlen(kMarker), bl = strlen(kBreak);
        // restart the search after the insert: searching from 0 each time is O(n^2)
        for (size_t pos = 0; (pos = htmlContent.find(kMarker, pos)) != std::string::npos; pos += bl)
            htmlContent.replace(pos, ml, kBreak);
    }

    // source dir for media resolution
    fs::path resolvedPath = fs::canonical(inputFile, ec);
    fs::path sourceDir = ec ? inputFile.parent_path() : resolvedPath.parent_path();

    // templates dir: next to exe, fallback cwd
    fs::path templatesDir = exeDir / L"templates";
    if (!fs::is_directory(templatesDir, ec)) templatesDir = fs::current_path() / "templates";

    if (noHeader && !headerOverride.empty())
        OUT_WARN("--no-header overrides --header");
    if (noFooter && !footerOverride.empty())
        OUT_WARN("--no-footer overrides --footer");

    std::string headerTemplate, footerTemplate;
    if (!noHeader) {
        if (!headerOverride.empty()) {
            if (!loadTemplateArg(headerOverride, "Header", headerTemplate)) { WSACleanup(); return 1; }
        } else {
            fs::path hf = templatesDir / L"header.html";
            std::error_code e2;
            if (fs::exists(hf, e2)) headerTemplate = readWholeFileUtf8(hf);
        }
    }
    if (!noFooter) {
        if (!footerOverride.empty()) {
            if (!loadTemplateArg(footerOverride, "Footer", footerTemplate)) { WSACleanup(); return 1; }
        } else {
            fs::path ff = templatesDir / L"footer.html";
            std::error_code e2;
            if (fs::exists(ff, e2)) footerTemplate = readWholeFileUtf8(ff);
        }
    }

    Config cfg;
    if (timeoutOverride > 0) cfg.timeout = timeoutOverride;

    // build full document from pdf.html template
    missingMedia.clear();
    std::string doc;
    {
        // --body= replaces templates/pdf.html; without it the built-in location is
        // used, and without that a minimal document wraps the content.
        bool haveTemplate = false;
        if (!bodyOverride.empty()) {
            if (!loadTemplateArg(bodyOverride, "Body", doc)) { WSACleanup(); return 1; }
            haveTemplate = true;
        } else {
            fs::path tf = templatesDir / L"pdf.html";
            std::error_code e2;
            if (fs::exists(tf, e2)) { doc = readWholeFileUtf8(tf); haveTemplate = true; }
        }

        if (haveTemplate) {
            // Resume after the substituted text. Restarting at 0 was O(n^2), and it
            // looped forever if the value itself contained the placeholder -- a
            // markdown file with the literal text {{CONTENT}} in it used to hang here.
            auto subst = [&](const char* key, const std::string& val) {
                const size_t kl = strlen(key);
                for (size_t q = 0; (q = doc.find(key, q)) != std::string::npos; q += val.size())
                    doc.replace(q, kl, val);
            };
            char b[32];
            subst("{{PAGE_SIZE}}", cfg.pageSize);
            subst("{{ORIENTATION}}", toLower(cfg.orientation));
            snprintf(b, sizeof b, "%g", cfg.marginTop);    subst("{{MARGIN_TOP}}", b);
            snprintf(b, sizeof b, "%g", cfg.marginRight);  subst("{{MARGIN_RIGHT}}", b);
            snprintf(b, sizeof b, "%g", cfg.marginBottom); subst("{{MARGIN_BOTTOM}}", b);
            snprintf(b, sizeof b, "%g", cfg.marginLeft);   subst("{{MARGIN_LEFT}}", b);
            subst("{{HEADER_HEIGHT}}", "0");
            subst("{{FOOTER_HEIGHT}}", "0");
            subst("{{HEADER}}", "");
            subst("{{FOOTER}}", "");
            subst("{{CONTENT}}", htmlContent);
        } else {
            doc = "<!doctype html><html><head><meta charset=\"utf-8\"></head><body>" + htmlContent + "</body></html>";
        }
    }
    doc = resolveMediaPaths(doc, sourceDir);

    std::set<std::string> seen;
    for (auto& m : missingMedia) {
        if (seen.insert(m).second) OUT_WARN("Warning: media not found: " + m);
    }

    if (!htmlOutPath.empty()) {
        // debug flag: write the exact document handed to Chrome, then stop
        if (writeBytes(fs::path(utf8ToWide(htmlOutPath)), doc))
            OUT_INFO("HTML dumped: " + htmlOutPath);
        else
            OUT_ERR("Cannot write HTML dump: " + htmlOutPath);
        WSACleanup();
        return 0;
    }

    OUT_INFO("Converting...");
    ConvertResult res = convert(doc, outPath, headerTemplate, footerTemplate, cfg);
    if (res.ok) {
        std::error_code e3;
        auto sz = fs::file_size(outPath, e3);
        char b[64];
        snprintf(b, sizeof b, "Done. PDF saved (%llu bytes)", (unsigned long long)sz);
        OUT_OK(b);
        code = 0;
    } else {
        OUT_ERR("Conversion failed: " + res.error);
        code = 1;
    }

    WSACleanup();
    return code;
}
