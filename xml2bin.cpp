// ============================================================================
//  xml2bin.cpp — 《东方非想天则》数据 XML → 二进制 / 文本 转换器（C++ 单文件版）
//
//  与 PowerShell 版 (xml2bin.ps1) 功能一致，但速度更快、无需 PowerShell：
//    movepattern（角色动作） → <名称>.bin       魔数 "SKMP"
//    animpattern（粒子特效） → <名称>_p.bin     魔数 "SKAP"
//    layout（界面布局）      → <名称>_layout.txt（带中文简短解析）
//    -CombineSprite <文件夹> → <文件夹名>.bin   魔数 "SKSP"（图片按名字排序合成动画 default）
//
//  [二进制格式] 小端序，与 PS 版逐字节兼容：
//    Header(32B): "SKMP"/"SKAP"/"SKSP", u16 version=1, u16 headerSize=32,
//                 u32 stringCount, u32 stringTableOffset(=32), u32 cloneCount,
//                 u32 recordCount, u32 totalFrames, u32 dataOffset
//    StringTable: u32 count, 每项 u16 len + UTF8 字节
//    CloneTable : (i32 id, i32 target) *
//    RecordTable: (i32 id, u8 index, u8 loop, u8 movelock, u8 actionlock,
//                  u32 frameCount, Frame*) *
//
//    Frame      : u32 imageIdx, u16 index, i32 xtexoffset, i32 ytexoffset,
//                 u16 texwidth, u16 texheight, i32 xoffset, i32 yoffset,
//                 u16 duration, u8 unknown, u8 rendergroup,
//                 u16 blendCount     + Blend(15B)*
//                 u16 attackCount    + (u16 boxCount + Box5(10B)*)*
//                 u16 collisionCount + (u16 boxCount + Box4(8B)*)*
//                 u16 hitCount       + (u16 boxCount + Box4(8B)*)*
//                 u16 effectCount    + (i32 x9)*
//                 u16 traitsCount    + (i32 x20 + u16 flagCount + u16 flagIdx*)*
//
//    SKSP（-CombineSprite 精灵合并）结构同 SKAP，仅魔数不同：
//      头部 cloneCount 槽位 = 动画名在字符串表中的索引（第 0 项即 "default"）
//
//  [编译] 见 build.bat（约 1~2 秒）
//     cl  /nologo /std:c++17 /O2 /EHsc /utf-8 /DNDEBUG xml2bin.cpp
//     g++ -O2 -std=c++17 -o xml2bin.exe xml2bin.cpp
//
//  [用法]
//     xml2bin.exe <XML或目录> [-OutputPath <路径>] [-Recurse] [-Verify] [-Strict]
//                 [-ParticleSuffix _p] [-LayoutSuffix _layout]
//     xml2bin.exe -CombineSprite <文件夹> [-Extension .bmp] [-Verify]
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdarg>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <stdexcept>

// ==================== 控制台颜色 / 输出 ====================
enum Color {
    C_DEF = 7, C_DGRAY = 8, C_CYAN = 11, C_DCYAN = 3, C_GREEN = 10,
    C_YELLOW = 14, C_DYELLOW = 6, C_RED = 12, C_MAGENTA = 13, C_DGREEN = 2, C_GRAY = 7
};

static void setColor(int c) {
#ifdef _WIN32
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), (WORD)c);
#else
    (void)c;
#endif
}

static void col(int c, const char* fmt, ...) {
    setColor(c);
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    setColor(C_DEF);
}

static void rule(const char* ch, int color) {
    setColor(color);
    for (int i = 0; i < 66; ++i) fputs(ch, stdout);
    fputc('\n', stdout);
    setColor(C_DEF);
}

// 心跳（「已完成 x/y 段」）状态：完成后该行由结果行原地覆盖，不保留进度文字
static bool g_hbActive = false;

// 用空格把当前行整行抹掉（光标回到行首）
static void clearLine() {
    fputs("\r", stdout);
    for (int k = 0; k < 72; ++k) fputc(' ', stdout);
    fputs("\r", stdout);
}

static void clearHeartbeat() {
    if (!g_hbActive) return;
    setColor(C_DGRAY);
    clearLine();
    setColor(C_DEF);
    g_hbActive = false;
}

static std::string formatSize(long long b) {
    char buf[64];
    if (b >= 1024 * 1024)      snprintf(buf, sizeof buf, "%.1f MB", b / 1048576.0);
    else if (b >= 1024)        snprintf(buf, sizeof buf, "%.1f KB", b / 1024.0);
    else                       snprintf(buf, sizeof buf, "%lld B", b);
    return buf;
}

static std::string formatPct(long long part, long long total) {
    char buf[32];
    if (total > 0) snprintf(buf, sizeof buf, "%.1f%%", 100.0 * (double)part / (double)total);
    else           snprintf(buf, sizeof buf, "-");
    return buf;
}

static std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// ==================== 路径工具（UTF-8 <-> UTF-16） ====================
#ifdef _WIN32
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string fromWide(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

static bool readFileBytes(const std::string& path, std::string& out) {
#ifdef _WIN32
    FILE* f = _wfopen(toWide(path).c_str(), L"rb");
#else
    FILE* f = fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = (n > 0) ? fread(&out[0], 1, (size_t)n, f) : 0;
    fclose(f);
    out.resize(got);
    return true;
}

static long long fileSize(const std::string& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(toWide(path).c_str(), GetFileExInfoStandard, &d))
        return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return -1;
#else
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long long n = ftell(f); fclose(f); return n;
#endif
}

static std::string dirName(const std::string& p) {
    size_t k = p.find_last_of("\\/");
    return (k == std::string::npos) ? std::string() : p.substr(0, k);
}

static std::string baseName(const std::string& p) {
    size_t k = p.find_last_of("\\/");
    return (k == std::string::npos) ? p : p.substr(k + 1);
}

static std::string stemName(const std::string& p) {
    std::string b = baseName(p);
    size_t k = b.find_last_of('.');
    return (k == std::string::npos) ? b : b.substr(0, k);
}

static std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (!a.empty() && (a.back() == '\\' || a.back() == '/')) return a + b;
    return a + "\\" + b;
}

static bool isDirectory(const std::string& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(toWide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)p; return false;
#endif
}

static bool isFile(const std::string& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(toWide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)p; return false;
#endif
}

// 递归收集 *.xml（返回 UTF-8 路径，排序后输出保持一致）
static void collectXml(const std::string& dir, bool recurse, std::vector<std::string>& out) {
#ifdef _WIN32
    std::wstring pattern = toWide(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring full = toWide(dir) + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recurse) collectXml(fromWide(full), true, out);
        } else {
            std::string name = fromWide(fd.cFileName);
            if (name.size() > 4) {
                std::string ext = name.substr(name.size() - 4);
                for (auto& c : ext) c = (char)tolower((unsigned char)c);
                if (ext == ".xml") out.push_back(fromWide(full));
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    (void)dir; (void)recurse; (void)out;
#endif
}

// ==================== 简易 XML DOM ====================
struct Attr { std::string name, value; };

struct Node {
    std::string name;
    std::vector<Attr> attrs;
    std::vector<Node> children;
    std::string text;

    const std::string* attr(const char* key) const {
        for (const Attr& a : attrs)
            if (a.name == key) return &a.value;
        return nullptr;
    }
    bool is(const char* n) const { return name == n; }
};

static void decodeEntities(std::string& s) {
    if (s.find('&') == std::string::npos) return;
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '&') {
            size_t j = s.find(';', i);
            if (j != std::string::npos && j - i <= 10) {
                std::string e = s.substr(i + 1, j - i - 1);
                if (e == "amp") o += '&';
                else if (e == "lt") o += '<';
                else if (e == "gt") o += '>';
                else if (e == "quot") o += '"';
                else if (e == "apos") o += '\'';
                else if (e.size() > 1 && e[0] == '#') {
                    long cp = (e[1] == 'x' || e[1] == 'X') ? strtol(e.c_str() + 2, nullptr, 16)
                                                           : strtol(e.c_str() + 1, nullptr, 10);
                    if (cp > 0 && cp < 0x110000) {           // UTF-8 编码
                        char b[4]; int n = 0;
                        if (cp < 0x80)       { b[n++] = (char)cp; }
                        else if (cp < 0x800) { b[n++] = (char)(0xC0 | (cp >> 6));  b[n++] = (char)(0x80 | (cp & 0x3F)); }
                        else if (cp < 0x10000){ b[n++] = (char)(0xE0 | (cp >> 12)); b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[n++] = (char)(0x80 | (cp & 0x3F)); }
                        else                 { b[n++] = (char)(0xF0 | (cp >> 18)); b[n++] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[n++] = (char)(0x80 | (cp & 0x3F)); }
                        o.append(b, n);
                    }
                } else o.append(s, i, j - i + 1);
                i = j + 1;
                continue;
            }
        }
        o += s[i++];
    }
    s.swap(o);
}

class XmlParser {
public:
    explicit XmlParser(const std::string& src) : s(src), i(0), n(src.size()) {}

    Node parse() {
        // 跳过 BOM / 声明 / 注释
        if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) i = 3;
        skipMisc();
        return parseElement();
    }

private:
    const std::string& s;
    size_t i, n;

    void skipWs() { while (i < n && isspace((unsigned char)s[i])) ++i; }

    bool starts(const char* lit) const {
        size_t k = strlen(lit);
        return i + k <= n && memcmp(s.data() + i, lit, k) == 0;
    }

    void skipMisc() {
        for (;;) {
            skipWs();
            if (i >= n) return;
            if (starts("<?")) { size_t j = s.find("?>", i); i = (j == std::string::npos) ? n : j + 2; continue; }
            if (starts("<!--")) { size_t j = s.find("-->", i); i = (j == std::string::npos) ? n : j + 3; continue; }
            if (starts("<!")) { size_t j = s.find('>', i); i = (j == std::string::npos) ? n : j + 1; continue; }
            return;
        }
    }

    std::string parseName() {
        size_t b = i;
        while (i < n) {
            unsigned char c = (unsigned char)s[i];
            if (isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.') ++i; else break;
        }
        return s.substr(b, i - b);
    }

    Node parseElement() {
        Node node;
        ++i;                                    // 跳过 '<'
        node.name = parseName();

        for (;;) {                              // 属性
            skipWs();
            if (i >= n) return node;
            if (s[i] == '>') { ++i; break; }
            if (s[i] == '/') { while (i < n && s[i] != '>') ++i; if (i < n) ++i; return node; }
            Attr a;
            a.name = parseName();
            skipWs();
            if (i < n && s[i] == '=') {
                ++i; skipWs();
                if (i < n && (s[i] == '"' || s[i] == '\'')) {
                    char q = s[i++];
                    size_t b = i;
                    while (i < n && s[i] != q) ++i;
                    a.value.assign(s, b, i - b);
                    if (i < n) ++i;
                    decodeEntities(a.value);
                }
            }
            if (a.name.empty() && a.value.empty()) { ++i; continue; }
            node.attrs.push_back(std::move(a));
        }

        for (;;) {                              // 内容
            if (i >= n) break;
            if (starts("</")) { size_t j = s.find('>', i); i = (j == std::string::npos) ? n : j + 1; break; }
            if (starts("<!--")) { size_t j = s.find("-->", i); i = (j == std::string::npos) ? n : j + 3; continue; }
            if (starts("<![CDATA[")) {
                size_t b = i + 9, j = s.find("]]>", i);
                size_t e = (j == std::string::npos) ? n : j;
                node.text.append(s, b, e - b);
                i = (j == std::string::npos) ? n : j + 3;
                continue;
            }
            if (s[i] == '<') { node.children.push_back(parseElement()); continue; }
            size_t b = i;
            while (i < n && s[i] != '<') ++i;
            if (s.find_first_not_of(" \t\r\n", b) < i || b != i) {
                std::string t = s.substr(b, i - b);
                if (t.find_first_not_of(" \t\r\n") != std::string::npos) {
                    decodeEntities(t);
                    node.text += t;
                }
            }
        }
        return node;
    }
};

// ==================== 数值 / 警告 ====================
struct WarnBox {
    bool strict = false;
    std::unordered_set<std::string> seen;
    std::vector<std::string> list;

    void add(const std::string& msg) {
        if (!seen.insert(msg).second) return;
        list.push_back(msg);
        setColor(C_YELLOW);
        clearLine();                            // 先抹掉心跳行：警告完整可见、不留残影
        printf("      ! %s\n", msg.c_str());
        setColor(C_DEF);
        if (strict) throw std::runtime_error(msg);
    }
};

static std::string strf(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

// 十进制优先，其次十六进制（如 unknown02="ffff"）
static bool parseInt(const std::string& raw, long long& out) {
    if (raw.empty()) return false;
    char* e = nullptr;
    long long v = strtoll(raw.c_str(), &e, 10);
    if (e && e != raw.c_str() && *e == '\0') { out = v; return true; }
    bool hexLike = true;
    for (char c : raw) if (!isxdigit((unsigned char)c)) { hexLike = false; break; }
    if (hexLike) {
        unsigned long long hv = strtoull(raw.c_str(), &e, 16);
        if (e && *e == '\0') { out = (long long)hv; return true; }
    }
    return false;
}

static long long getNum(const Node& nd, const char* name, long long def,
                        long long lo, long long hi, WarnBox& warn) {
    long long v = def;
    const std::string* raw = nd.attr(name);
    if (raw && !raw->empty()) {
        if (!parseInt(*raw, v)) {
            warn.add(strf("<%s> 属性 %s=\"%s\" 不是整数，按 %lld 处理", nd.name.c_str(), name, raw->c_str(), def));
            v = def;
        }
    }
    if (v < lo) { warn.add(strf("<%s> 属性 %s=%lld 小于 %lld，已截断", nd.name.c_str(), name, v, lo)); v = lo; }
    if (v > hi) { warn.add(strf("<%s> 属性 %s=%lld 大于 %lld，已截断", nd.name.c_str(), name, v, hi)); v = hi; }
    return v;
}

static unsigned getColorAttr(const Node& nd, const char* name, WarnBox& warn) {
    const std::string* raw = nd.attr(name);
    if (!raw || raw->empty()) return 0xFFFFFFFFu;
    char* e = nullptr;
    unsigned long v = strtoul(raw->c_str(), &e, 16);
    if (e && e != raw->c_str() && *e == '\0') return (unsigned)v;
    warn.add(strf("<%s> 属性 %s=\"%s\" 不是合法 16 进制颜色，按 FFFFFFFF 处理", nd.name.c_str(), name, raw->c_str()));
    return 0xFFFFFFFFu;
}

static void checkAttrs(const Node& nd, const char* const* allowed, size_t nAllowed, WarnBox& warn) {
    for (const Attr& a : nd.attrs) {
        bool okAttr = false;
        for (size_t k = 0; k < nAllowed; ++k) if (a.name == allowed[k]) { okAttr = true; break; }
        if (!okAttr)
            warn.add(strf("<%s> 上发现未知属性 %s=\"%s\"，已忽略", nd.name.c_str(), a.name.c_str(), a.value.c_str()));
    }
}

// ==================== 字节流 / 字符串表 ====================
struct Sink {
    std::vector<unsigned char> v;
    void u8(unsigned x)  { v.push_back((unsigned char)(x & 0xFF)); }
    void u16(unsigned x) { v.push_back((unsigned char)(x & 0xFF)); v.push_back((unsigned char)((x >> 8) & 0xFF)); }
    void i16(long long x) { u16((unsigned)(x & 0xFFFF)); }
    void u32(unsigned long x) {
        v.push_back((unsigned char)(x & 0xFF));         v.push_back((unsigned char)((x >> 8) & 0xFF));
        v.push_back((unsigned char)((x >> 16) & 0xFF)); v.push_back((unsigned char)((x >> 24) & 0xFF));
    }
    void i32(long long x) { u32((unsigned long)(x & 0xFFFFFFFFu)); }
};

struct StrTab {
    std::unordered_map<std::string, int> map;
    std::vector<std::string> list;

    int add(const std::string& s) {
        auto it = map.find(s);
        if (it != map.end()) return it->second;
        int id = (int)list.size();
        list.push_back(s);
        map.emplace(s, id);
        return id;
    }
};

// 属性名表
static const char* FRAME_ATTRS[]  = { "image","index","unknown","xtexoffset","ytexoffset","texwidth","texheight","xoffset","yoffset","duration","rendergroup" };
static const char* MOVE_ATTRS[]   = { "id","index","loop","movelock","actionlock" };
static const char* CLONE_ATTRS[]  = { "id","target" };
static const char* BLEND_ATTRS[]  = { "mode","color","xscale","yscale","vertflip","horzflip","angle" };
static const char* BOX5_ATTRS[]   = { "left","up","right","down","unknown" };
static const char* BOX4_ATTRS[]   = { "left","up","right","down" };
static const char* EFFECT_ATTRS[] = { "xpivot","ypivot","xpositionextra","ypositionextra","xposition","yposition","unknown02","xspeed","yspeed" };
static const char* TRAITS_ATTRS[] = { "damage","proration","chipdamage","spiritdamage","untech","power","limit",
                                      "onhitplayerstun","onhitenemystun","onblockplayerstun","onblockenemystun",
                                      "onhitcardgain","onblockcardgain","onairhitsetsequence","ongroundhitsetsequence",
                                      "xspeed","yspeed","onhitsfx","onhiteffect","attacklevel" };
static const char* FRAME_CHILD_KINDS[] = { "blend","traits","effect","collision","hit","attack" };
static const char* ROOT_ATTRS[] = { "version" };

// ==================== Frame 编码 ====================
static void writeFrame(Sink& s, const Node& f, StrTab& str, WarnBox& warn) {
    checkAttrs(f, FRAME_ATTRS, sizeof(FRAME_ATTRS) / sizeof(FRAME_ATTRS[0]), warn);

    for (const Node& ch : f.children) {
        bool known = false;
        for (const char* k : FRAME_CHILD_KINDS) if (ch.is(k)) { known = true; break; }
        if (!known) warn.add(strf("<frame> 下发现未知子元素 <%s>，已跳过", ch.name.c_str()));
    }

    const std::string* img = f.attr("image");
    s.u32((unsigned long)str.add(img ? *img : std::string()));
    s.u16((unsigned)getNum(f, "index", 0, 0, 65535, warn));
    s.i32(getNum(f, "xtexoffset", 0, -2000000000LL, 2000000000LL, warn));
    s.i32(getNum(f, "ytexoffset", 0, -2000000000LL, 2000000000LL, warn));
    s.u16((unsigned)getNum(f, "texwidth", 0, 0, 65535, warn));
    s.u16((unsigned)getNum(f, "texheight", 0, 0, 65535, warn));
    s.i32(getNum(f, "xoffset", 0, -2000000000LL, 2000000000LL, warn));
    s.i32(getNum(f, "yoffset", 0, -2000000000LL, 2000000000LL, warn));
    s.u16((unsigned)getNum(f, "duration", 0, 0, 65535, warn));
    s.u8((unsigned)getNum(f, "unknown", 0, 0, 255, warn));
    s.u8((unsigned)getNum(f, "rendergroup", 0, 0, 255, warn));

    // ---- blend ----
    unsigned blendCount = 0;
    for (const Node& c : f.children) if (c.is("blend")) ++blendCount;
    s.u16(blendCount);
    for (const Node& b : f.children) {
        if (!b.is("blend")) continue;
        checkAttrs(b, BLEND_ATTRS, sizeof(BLEND_ATTRS) / sizeof(BLEND_ATTRS[0]), warn);
        s.u8((unsigned)getNum(b, "mode", 0, 0, 255, warn));
        s.u32(getColorAttr(b, "color", warn));
        s.i16(getNum(b, "xscale", 100, -32768, 32767, warn));
        s.i16(getNum(b, "yscale", 100, -32768, 32767, warn));
        s.i16(getNum(b, "vertflip", 0, -32768, 32767, warn));
        s.i16(getNum(b, "horzflip", 0, -32768, 32767, warn));
        s.i16(getNum(b, "angle", 0, -32768, 32767, warn));
    }

    // ---- attack ----
    unsigned attackCount = 0;
    for (const Node& c : f.children) if (c.is("attack")) ++attackCount;
    s.u16(attackCount);
    for (const Node& a : f.children) {
        if (!a.is("attack")) continue;
        checkAttrs(a, nullptr, 0, warn);
        unsigned boxCount = 0;
        for (const Node& c : a.children) if (c.is("box")) ++boxCount;
        s.u16(boxCount);
        for (const Node& b : a.children) {
            if (!b.is("box")) continue;
            checkAttrs(b, BOX5_ATTRS, sizeof(BOX5_ATTRS) / sizeof(BOX5_ATTRS[0]), warn);
            s.i16(getNum(b, "left", 0, -32768, 32767, warn));
            s.i16(getNum(b, "up", 0, -32768, 32767, warn));
            s.i16(getNum(b, "right", 0, -32768, 32767, warn));
            s.i16(getNum(b, "down", 0, -32768, 32767, warn));
            s.i16(getNum(b, "unknown", 0, -32768, 32767, warn));
        }
    }

    // ---- collision / hit ----
    const char* kinds[2] = { "collision", "hit" };
    for (const char* kind : kinds) {
        unsigned cnt = 0;
        for (const Node& c : f.children) if (c.is(kind)) ++cnt;
        s.u16(cnt);
        for (const Node& g : f.children) {
            if (!g.is(kind)) continue;
            checkAttrs(g, nullptr, 0, warn);
            unsigned boxCount = 0;
            for (const Node& c : g.children) if (c.is("box")) ++boxCount;
            s.u16(boxCount);
            for (const Node& b : g.children) {
                if (!b.is("box")) continue;
                checkAttrs(b, BOX4_ATTRS, sizeof(BOX4_ATTRS) / sizeof(BOX4_ATTRS[0]), warn);
                s.i16(getNum(b, "left", 0, -32768, 32767, warn));
                s.i16(getNum(b, "up", 0, -32768, 32767, warn));
                s.i16(getNum(b, "right", 0, -32768, 32767, warn));
                s.i16(getNum(b, "down", 0, -32768, 32767, warn));
            }
        }
    }

    // ---- effect ----
    unsigned effectCount = 0;
    for (const Node& c : f.children) if (c.is("effect")) ++effectCount;
    s.u16(effectCount);
    for (const Node& e : f.children) {
        if (!e.is("effect")) continue;
        checkAttrs(e, EFFECT_ATTRS, sizeof(EFFECT_ATTRS) / sizeof(EFFECT_ATTRS[0]), warn);
        for (const char* an : EFFECT_ATTRS) s.i32(getNum(e, an, 0, -2000000000LL, 2000000000LL, warn));
    }

    // ---- traits ----
    unsigned traitsCount = 0;
    for (const Node& c : f.children) if (c.is("traits")) ++traitsCount;
    s.u16(traitsCount);
    for (const Node& t : f.children) {
        if (!t.is("traits")) continue;
        checkAttrs(t, TRAITS_ATTRS, sizeof(TRAITS_ATTRS) / sizeof(TRAITS_ATTRS[0]), warn);
        for (const char* an : TRAITS_ATTRS) s.i32(getNum(t, an, 0, -2000000000LL, 2000000000LL, warn));

        std::vector<int> flags;
        for (const Node& fl : t.children) {
            if (fl.attrs.size() > 0 || !fl.text.empty())
                warn.add(strf("<traits> 的旗标 <%s> 带有额外属性或文本，已忽略", fl.name.c_str()));
            flags.push_back(str.add(fl.name));
        }
        s.u16((unsigned)flags.size());
        for (int idx : flags) s.u16((unsigned)idx);
    }
}

// ==================== XML -> BIN ====================
struct BinStats {
    std::string kind;
    long long records = 0, frames = 0, strings = 0, clones = 0, bytes = 0, warnings = 0;
};

static BinStats convertBin(const Node& root, const std::string& srcPath, const std::string& outPath,
                           bool isAnim, bool strict) {
    BinStats st;
    st.kind = isAnim ? "粒子" : "角色";

    WarnBox warn; warn.strict = strict;
    StrTab str;
    Sink rec;
    struct RecInfo { long long id; long long index, loop, movelock, actionlock; unsigned frameCount; size_t off, len; };
    std::vector<RecInfo> recs;
    std::vector<std::pair<long long, long long>> clones;

    checkAttrs(root, ROOT_ATTRS, 1, warn);

    // clone（仅角色文件）
    if (!isAnim) {
        for (const Node& c : root.children) {
            if (!c.is("clone")) continue;
            checkAttrs(c, CLONE_ATTRS, sizeof(CLONE_ATTRS) / sizeof(CLONE_ATTRS[0]), warn);
            clones.emplace_back(getNum(c, "id", 0, -2000000000LL, 2000000000LL, warn),
                                getNum(c, "target", 0, -2000000000LL, 2000000000LL, warn));
        }
    }

    // 记录列表：<move>（角色）或 <animation>（粒子）
    std::vector<const Node*> records;
    for (const Node& c : root.children) if (c.is("move")) records.push_back(&c);
    if (records.empty())
        for (const Node& c : root.children) if (c.is("animation")) records.push_back(&c);

    // 心跳计时（无延迟：每段都刷新同一行）
    auto hbLast = std::chrono::steady_clock::now();
    (void)hbLast;

    int ri = 0;
    for (const Node* m : records) {
        checkAttrs(*m, MOVE_ATTRS, sizeof(MOVE_ATTRS) / sizeof(MOVE_ATTRS[0]), warn);

        size_t start = rec.v.size();
        unsigned fc = 0;
        for (const Node& f : m->children) {
            if (!f.is("frame")) {
                warn.add(strf("<%s> 下发现未知子元素 <%s>，已跳过", m->name.c_str(), f.name.c_str()));
                continue;
            }
            writeFrame(rec, f, str, warn);
            ++fc;
        }
        recs.push_back(RecInfo{
            getNum(*m, "id", 0, -2000000000LL, 2000000000LL, warn),
            getNum(*m, "index", 0, 0, 255, warn),
            getNum(*m, "loop", 0, 0, 255, warn),
            getNum(*m, "movelock", 0, 0, 255, warn),
            getNum(*m, "actionlock", 0, 0, 255, warn),
            fc, start, rec.v.size() - start });

        ++ri;
        // 心跳：同一行原地刷新；完成后由结果行原地覆盖（不换行、不保留）
        setColor(C_DGRAY);
        printf("\r      · 已完成 %d/%d 段", ri, (int)records.size());
        setColor(C_DEF);
        fflush(stdout);
        g_hbActive = true;
    }

    st.records = (long long)recs.size();
    st.strings = (long long)str.list.size();
    st.clones  = (long long)clones.size();
    for (const RecInfo& r : recs) st.frames += r.frameCount;
    st.warnings = (long long)warn.list.size();

    // ---- 组装输出 ----
    Sink strTabBytes;
    strTabBytes.u32((unsigned long)str.list.size());
    for (const std::string& s : str.list) {
        strTabBytes.u16((unsigned)s.size());
        strTabBytes.v.insert(strTabBytes.v.end(), s.begin(), s.end());
    }

    Sink cloneBytes;
    for (auto& c : clones) { cloneBytes.i32(c.first); cloneBytes.i32(c.second); }

    unsigned long dataOffset = 32 + (unsigned long)strTabBytes.v.size() + (unsigned long)cloneBytes.v.size();

#ifdef _WIN32
    FILE* f = _wfopen(toWide(outPath).c_str(), L"wb");
#else
    FILE* f = fopen(outPath.c_str(), "wb");
#endif
    if (!f) throw std::runtime_error("无法写入文件: " + outPath);

    auto wr = [f](const void* p, size_t n) { if (n) fwrite(p, 1, n, f); };

    unsigned char hdr[32];
    memcpy(hdr, isAnim ? "SKAP" : "SKMP", 4);
    auto put16 = [](unsigned char* p, unsigned v) { p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF); };
    auto put32 = [](unsigned char* p, unsigned long v) {
        p[0] = (unsigned char)(v & 0xFF);         p[1] = (unsigned char)((v >> 8) & 0xFF);
        p[2] = (unsigned char)((v >> 16) & 0xFF); p[3] = (unsigned char)((v >> 24) & 0xFF);
    };
    put16(hdr + 4, 1);                       // version
    put16(hdr + 6, 32);                      // headerSize
    put32(hdr + 8, (unsigned long)str.list.size());
    put32(hdr + 12, 32);                     // stringTableOffset
    put32(hdr + 16, (unsigned long)clones.size());
    put32(hdr + 20, (unsigned long)recs.size());
    put32(hdr + 24, (unsigned long)st.frames);
    put32(hdr + 28, dataOffset);
    wr(hdr, sizeof hdr);
    wr(strTabBytes.v.data(), strTabBytes.v.size());
    wr(cloneBytes.v.data(), cloneBytes.v.size());

    for (const RecInfo& r : recs) {
        unsigned char rh[12];                       // id(4) + index/loop/movelock/actionlock(4) + frameCount(4)
        put32(rh + 0, (unsigned long)(r.id & 0xFFFFFFFFu));
        rh[4] = (unsigned char)(r.index & 0xFF);
        rh[5] = (unsigned char)(r.loop & 0xFF);
        rh[6] = (unsigned char)(r.movelock & 0xFF);
        rh[7] = (unsigned char)(r.actionlock & 0xFF);
        put32(rh + 8, r.frameCount);
        wr(rh, sizeof rh);
        wr(rec.v.data() + r.off, r.len);
    }
    st.bytes = _ftelli64(f);
    fclose(f);

    return st;
}

// ==================== 读回校验 ====================
struct VerifyResult { std::string magic; long long records = 0, frames = 0, strings = 0, clones = 0, bytes = 0; };

static unsigned char* readAll(const std::string& path, std::string& holder) {
    if (!readFileBytes(path, holder) || holder.empty()) return nullptr;
    return (unsigned char*)&holder[0];
}

static VerifyResult verifyBin(const std::string& path) {
    std::string data;
    unsigned char* p = readAll(path, data);
    if (!p) throw std::runtime_error("无法读取文件: " + path);

    VerifyResult r;
    r.bytes = (long long)data.size();
    if (data.size() < 32) throw std::runtime_error("文件过小: " + path);

    r.magic.assign(data, 0, 4);
    if (r.magic != "SKMP" && r.magic != "SKAP" && r.magic != "SKSP")
        throw std::runtime_error(strf("魔数不匹配（%s），不是 SKMP/SKAP/SKSP 文件: %s", r.magic.c_str(), path.c_str()));

    auto get16 = [&](size_t o) { return (unsigned)data[o] | ((unsigned)data[o + 1] << 8); };
    auto get32 = [&](size_t o) {
        return (unsigned long)(unsigned char)data[o] | ((unsigned long)(unsigned char)data[o + 1] << 8) |
               ((unsigned long)(unsigned char)data[o + 2] << 16) | ((unsigned long)(unsigned char)data[o + 3] << 24);
    };

    unsigned headerSize = get16(6);
    unsigned long strCount = get32(8), strOffset = get32(12), cloneCount = get32(16);
    unsigned long recCount = get32(20), frameTotal = get32(24), dataOffset = get32(28);
    if (headerSize != 32) throw std::runtime_error("头部长度异常");
    if (strOffset != 32) throw std::runtime_error("字符串表偏移异常");

    size_t pos = 32;
    unsigned long tblCount = get32(pos); pos += 4;
    (void)tblCount;
    for (unsigned long k = 0; k < strCount; ++k) {
        if (pos + 2 > data.size()) throw std::runtime_error("字符串表越界");
        unsigned len = get16(pos); pos += 2 + len;
    }
    pos += (size_t)cloneCount * 8;
    if (pos != dataOffset)
        throw std::runtime_error(strf("数据区偏移异常: 实际 %zu，头部记录 %lu", pos, dataOffset));

    r.strings = strCount;
    r.clones = cloneCount;
    r.records = recCount;

    long long frames = 0;
    for (unsigned long mi = 0; mi < recCount; ++mi) {
        if (pos + 12 > data.size()) throw std::runtime_error("记录区越界");
        pos += 8;                                  // id(4) + index/loop/movelock/actionlock(4)
        unsigned long fc = get32(pos); pos += 4;   // frameCount
        for (unsigned long fi = 0; fi < fc; ++fi) {
            if (pos + 30 > data.size()) throw std::runtime_error("帧数据越界");
            pos += 30;                             // image..rendergroup
            unsigned bc = get16(pos); pos += 2 + (size_t)bc * 15;
            for (int kind = 0; kind < 3; ++kind) { // attack / collision / hit
                unsigned cnt = get16(pos); pos += 2;
                for (unsigned c = 0; c < cnt; ++c) {
                    unsigned bc2 = get16(pos); pos += 2 + (size_t)bc2 * (kind == 0 ? 10 : 8);
                }
            }
            unsigned ec = get16(pos); pos += 2 + (size_t)ec * 36;
            unsigned tc = get16(pos); pos += 2;
            for (unsigned t = 0; t < tc; ++t) {
                pos += 80;
                unsigned flagCount = get16(pos); pos += 2 + (size_t)flagCount * 2;
            }
            ++frames;
        }
    }
    if (pos != data.size())
        throw std::runtime_error(strf("文件末尾存在 %lld 字节多余数据", (long long)(data.size() - pos)));
    if (frames != (long long)frameTotal)
        throw std::runtime_error(strf("帧数不一致: 实际 %lld，头部记录 %lu", frames, frameTotal));
    r.frames = frames;
    return r;
}

// ==================== Layout -> TXT（带中文简短解析） ====================
static std::string layoutValue(const std::string& v) {
    std::string t;
    t.reserve(v.size());
    for (char c : v) t += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    if (t.find_first_of(" \t\"") != std::string::npos || t.find('"') != std::string::npos) {
        std::string q = "\"";
        for (char c : t) { if (c == '"') q += "\\\""; else q += c; }
        return q + "\"";
    }
    return t;
}

static std::string attrText(const Node& nd, const char* k) {
    const std::string* v = nd.attr(k);
    return v ? *v : std::string();
}

static std::string layoutComment(const Node& nd) {
    if (nd.is("layout")) return "界面布局定义（version = 数据格式版本）";
    if (nd.is("static"))
        return strf("静态元素：固定显示的图片；屏幕坐标 (%s,%s)，mirror=%s（1=水平镜像）",
                    attrText(nd, "xposition").c_str(), attrText(nd, "yposition").c_str(), attrText(nd, "mirror").c_str());
    if (nd.is("image"))
        return strf("图片：%s；绘制尺寸 %sx%s；相对父元素偏移 (%s,%s)",
                    attrText(nd, "name").c_str(), attrText(nd, "width").c_str(), attrText(nd, "height").c_str(),
                    attrText(nd, "xposition").c_str(), attrText(nd, "yposition").c_str());
    if (nd.is("mutable"))
        return strf("可变元素：运行时由程序控制显隐的位置标记 (%s,%s)",
                    attrText(nd, "xposition").c_str(), attrText(nd, "yposition").c_str());
    if (nd.is("slidervert"))
        return strf("垂直滑块：竖直填充条（如血量/灵力槽）(%s,%s)",
                    attrText(nd, "xposition").c_str(), attrText(nd, "yposition").c_str());
    if (nd.is("sliderhorz"))
        return strf("水平滑块：水平填充条 (%s,%s)",
                    attrText(nd, "xposition").c_str(), attrText(nd, "yposition").c_str());
    if (nd.is("number"))
        return strf("数字显示：单字 %sx%s，字距 %s，数字间距 %s，字号 %s，floatsize=%s（1=按小数显示）",
                    attrText(nd, "width").c_str(), attrText(nd, "height").c_str(),
                    attrText(nd, "fontspacing").c_str(), attrText(nd, "textspacing").c_str(),
                    attrText(nd, "size").c_str(), attrText(nd, "floatsize").c_str());
    return "未知节点类型";
}

static std::string writeLayout(const Node& root, const std::string& srcPath, const std::string& outPath,
                               long long& nodeCount) {
    std::string out;
    out += "# Soku 界面布局解析（由 xml2bin.exe 生成，UTF-8）\n";
    out += "# source: " + srcPath + "\n";
    out += "# 行格式: <缩进><标签> [属性=值 ...] [=文本]    # 中文简短解析\n";
    out += "#\n";
    out += "# 标签速查:\n";
    out += "#   layout      整个界面布局定义（version = 数据格式版本，本数据为 4）\n";
    out += "#   static      静态元素：固定显示的图片容器，含一个 image 子节点\n";
    out += "#   image       图片引用：name = 图片文件；width/height = 绘制尺寸；x/yposition = 相对父元素偏移\n";
    out += "#   mutable     可变元素：运行时由程序控制显隐的位置标记（如选人光标、提示框位置）\n";
    out += "#   slidervert  垂直滑块：竖直方向的填充条（如血量、灵力槽）\n";
    out += "#   sliderhorz  水平滑块：水平方向的填充条（如卡片槽、进度条）\n";
    out += "#   number      数字显示：width/height = 单字尺寸，fontspacing = 字距，textspacing = 数字间距，\n";
    out += "#               size = 字号档位，floatsize = 1 时按小数（浮点）显示，如计时/连段数/伤害值\n";

    nodeCount = 0;
    // 深度优先、保持文档顺序
    struct Frame { const Node* n; int d; };
    std::vector<Frame> work;
    work.push_back(Frame{ &root, 0 });
    while (!work.empty()) {
        Frame cur = work.back();
        work.pop_back();
        const Node& nd = *cur.n;

        out.append((size_t)cur.d * 2, ' ');
        out += nd.name;
        for (const Attr& a : nd.attrs) {
            out += ' ';
            out += a.name;
            out += '=';
            out += layoutValue(a.value);
        }
        if (!nd.text.empty()) { out += " ="; out += layoutValue(nd.text); }
        out += "    # ";
        out += layoutComment(nd);
        out += "\n";
        ++nodeCount;

        for (size_t k = nd.children.size(); k-- > 0;)
            work.push_back(Frame{ &nd.children[k], cur.d + 1 });
    }
    out += "\n";

#ifdef _WIN32
    FILE* f = _wfopen(toWide(outPath).c_str(), L"wb");
#else
    FILE* f = fopen(outPath.c_str(), "wb");
#endif
    if (!f) throw std::runtime_error("无法写入文件: " + outPath);
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return out;
}

// ==================== 精灵合并（-CombineSprite） ====================
// 把文件夹内的图片按文件名排序，整合为一个名为 "default" 的动画，
// 输出 <文件夹名>.bin；魔数 "SKSP"（头部 cloneCount 槽位 = 动画名索引）

static bool readFileHead(const std::string& path, size_t maxBytes, std::string& out) {
#ifdef _WIN32
    FILE* f = _wfopen(toWide(path).c_str(), L"rb");
#else
    FILE* f = fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    out.resize(maxBytes);
    size_t got = fread(&out[0], 1, maxBytes, f);
    fclose(f);
    out.resize(got);
    return true;
}

// 规范化扩展名：空字符串 = 不添加；"bmp" / ".bmp" 都接受
static std::string normalizeExt(const std::string& v) {
    if (v.empty()) return std::string();
    if (v[0] == '.') return v;
    return "." + v;
}

static bool isImageFile(const std::string& name) {
    size_t k = name.find_last_of('.');
    if (k == std::string::npos) return false;
    std::string e = lower(name.substr(k));
    return e == ".png" || e == ".bmp" || e == ".jpg" || e == ".jpeg" || e == ".gif" ||
           e == ".webp" || e == ".tga" || e == ".dds";
}

// 列出目录内的文件（非递归）；imagesOnly=true 时只保留常见图片格式
static void collectFilesFlat(const std::string& dir, bool imagesOnly, std::vector<std::string>& out) {
#ifdef _WIN32
    std::wstring base = toWide(dir);
    std::wstring pattern = base + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fromWide(fd.cFileName);
        if (imagesOnly && !isImageFile(name)) continue;
        out.push_back(fromWide(base + L"\\" + fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    (void)dir; (void)imagesOnly; (void)out;
#endif
}

// 读取图片像素尺寸（PNG / BMP / GIF / JPEG），失败返回 false
static bool readImageSize(const std::string& path, long long& w, long long& h) {
    std::string d;
    if (!readFileHead(path, 65536, d) || d.size() < 26) return false;
    const unsigned char* p = (const unsigned char*)d.data();
    auto be32 = [&](size_t o) { return ((unsigned long)p[o] << 24) | ((unsigned long)p[o + 1] << 16) | ((unsigned long)p[o + 2] << 8) | (unsigned long)p[o + 3]; };
    auto le32 = [&](size_t o) { return (unsigned long)p[o] | ((unsigned long)p[o + 1] << 8) | ((unsigned long)p[o + 2] << 16) | ((unsigned long)p[o + 3] << 24); };
    auto le16 = [&](size_t o) { return (unsigned)p[o] | ((unsigned)p[o + 1] << 8); };

    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G' && d.size() >= 24) {   // PNG
        w = (long long)be32(16); h = (long long)be32(20); return true;
    }
    if (p[0] == 'G' && p[1] == 'I' && p[2] == 'F') {                                      // GIF
        w = (long long)le16(6); h = (long long)le16(8); return true;
    }
    if (p[0] == 'B' && p[1] == 'M' && d.size() >= 26) {                                   // BMP
        long long bw = (int)le32(18), bh = (int)le32(22);
        w = bw < 0 ? -bw : bw; h = bh < 0 ? -bh : bh; return true;
    }
    if (p[0] == 0xFF && p[1] == 0xD8) {                                                   // JPEG
        size_t i = 2;
        while (i + 9 < d.size()) {
            if (p[i] != 0xFF) { ++i; continue; }
            unsigned char m = p[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            unsigned len = ((unsigned)p[i + 2] << 8) | p[i + 3];
            if (len < 2) break;
            bool sof = (m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) ||
                       (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF);
            if (sof) {
                h = (long long)(((unsigned)p[i + 5] << 8) | p[i + 6]);
                w = (long long)(((unsigned)p[i + 7] << 8) | p[i + 8]);
                return true;
            }
            i += 2 + len;
        }
    }
    return false;
}

struct SpriteStats {
    long long frames = 0, bytes = 0, warnings = 0, skipped = 0;
    std::string name;
};

static SpriteStats combineSpriteBin(const std::string& folder, const std::string& outPath,
                                    const std::string& ext, bool strict) {
    SpriteStats st;
    st.name = baseName(folder);
    while (!st.name.empty() && (st.name.back() == '\\' || st.name.back() == '/')) st.name.pop_back();

    std::vector<std::string> files, all;
    collectFilesFlat(folder, true, files);
    collectFilesFlat(folder, false, all);
    std::sort(files.begin(), files.end());                 // 按文件名排序
    if (files.empty()) throw std::runtime_error("文件夹内没有图片: " + folder);
    st.skipped = (long long)all.size() - (long long)files.size();

    WarnBox warn; warn.strict = strict;
    StrTab str;
    str.add("default");                                    // 字符串表第 0 项 = 动画名

    Sink rec;
    unsigned frameCount = 0;
    for (const std::string& f : files) {
        std::string imgName = stemName(f) + ext;
        long long w = 0, h = 0;
        if (!readImageSize(f, w, h))
            warn.add(strf("无法识别图片尺寸: %s（按 0x0 处理）", baseName(f).c_str()));
        if (w > 65535) { warn.add(strf("%s 宽度 %lld 超出 uint16，已截断", baseName(f).c_str(), w)); w = 65535; }
        if (h > 65535) { warn.add(strf("%s 高度 %lld 超出 uint16，已截断", baseName(f).c_str(), h)); h = 65535; }

        rec.u32((unsigned long)str.add(imgName));          // image
        rec.u16(frameCount);                               // index
        rec.i32(0); rec.i32(0);                            // xtexoffset / ytexoffset
        rec.u16((unsigned)w); rec.u16((unsigned)h);        // texwidth / texheight
        rec.i32(0); rec.i32(0);                            // xoffset / yoffset（默认 0）
        rec.u16(1);                                        // duration = 1
        rec.u8(0); rec.u8(0);                              // unknown / rendergroup
        rec.u16(0);                                        // blendCount
        rec.u16(0);                                        // attackCount
        rec.u16(0);                                        // collisionCount
        rec.u16(0);                                        // hitCount
        rec.u16(0);                                        // effectCount
        rec.u16(0);                                        // traitsCount
        ++frameCount;
    }
    st.frames = frameCount;
    st.warnings = (long long)warn.list.size();

    // ---- 组装输出（布局同 SKAP，魔数 SKSP）----
    Sink strTabBytes;
    strTabBytes.u32((unsigned long)str.list.size());
    for (const std::string& s : str.list) {
        strTabBytes.u16((unsigned)s.size());
        strTabBytes.v.insert(strTabBytes.v.end(), s.begin(), s.end());
    }
    const unsigned long dataOffset = 32 + (unsigned long)strTabBytes.v.size();

#ifdef _WIN32
    FILE* f = _wfopen(toWide(outPath).c_str(), L"wb");
#else
    FILE* f = fopen(outPath.c_str(), "wb");
#endif
    if (!f) throw std::runtime_error("无法写入文件: " + outPath);

    auto wr = [f](const void* p, size_t n) { if (n) fwrite(p, 1, n, f); };
    unsigned char hdr[32];
    memcpy(hdr, "SKSP", 4);
    auto put16 = [](unsigned char* p, unsigned v) { p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF); };
    auto put32 = [](unsigned char* p, unsigned long v) {
        p[0] = (unsigned char)(v & 0xFF);         p[1] = (unsigned char)((v >> 8) & 0xFF);
        p[2] = (unsigned char)((v >> 16) & 0xFF); p[3] = (unsigned char)((v >> 24) & 0xFF);
    };
    put16(hdr + 4, 1);                                     // version
    put16(hdr + 6, 32);                                    // headerSize
    put32(hdr + 8, (unsigned long)str.list.size());        // stringCount
    put32(hdr + 12, 32);                                   // stringTableOffset
    put32(hdr + 16, 0);                                    // cloneCount 槽位 = 动画名索引（"default" = 0）
    put32(hdr + 20, 1);                                    // recordCount = 1 个动画
    put32(hdr + 24, frameCount);                           // totalFrames
    put32(hdr + 28, dataOffset);
    wr(hdr, sizeof hdr);
    wr(strTabBytes.v.data(), strTabBytes.v.size());

    unsigned char rh[12];
    put32(rh + 0, 0);                                      // id = 0
    rh[4] = 0;                                             // index = 0
    rh[5] = 1;                                             // loop = 1
    rh[6] = 0; rh[7] = 0;                                  // movelock / actionlock
    put32(rh + 8, frameCount);
    wr(rh, sizeof rh);
    wr(rec.v.data(), rec.v.size());

    st.bytes = _ftelli64(f);
    fclose(f);
    return st;
}

// ==================== 主流程 ====================
struct Options {
    std::string input, output;
    bool recurse = false, verify = false, strict = false, help = false;
    std::string particleSuffix = "_p", layoutSuffix = "_layout";
    std::vector<std::string> sprites;      // -CombineSprite 文件夹（可多次指定）
    std::string extension;                 // -Extension：写入图片名的扩展名（空 = 不添加）
};

static void printUsage() {
    col(C_CYAN, "用法: xml2bin [XML文件或目录] [选项]\n");
    printf("  -OutputPath <路径>    输出文件（单文件模式）或输出目录\n");
    printf("  -Recurse              目录模式下递归查找 *.xml\n");
    printf("  -Verify               转换后读回校验\n");
    printf("  -Strict               出现警告即报错（默认仅警告并跳过）\n");
    printf("  -ParticleSuffix <后缀>  粒子文件后缀，默认 _p\n");
    printf("  -LayoutSuffix <后缀>    布局文本文件后缀，默认 _layout\n");
    col(C_CYAN, "精灵合并:\n");
    printf("  -CombineSprite <文件夹>  文件夹内图片按名字排序，整合为一个名为 default\n");
    printf("                           的动画，输出 <文件夹名>.bin（魔数 SKSP）；可多次指定\n");
    printf("  -Extension <扩展名>      写入图片名时使用的扩展名（如 .bmp / bmp）；\n");
    printf("                           空字符串 = 不添加扩展名（默认）\n");
    col(C_GRAY, "示例:\n");
    printf("  xml2bin -CombineSprite \"..\\character\\reimu\\stand\" -Verify\n");
    printf("  xml2bin -CombineSprite .\\stand -Extension .bmp\n");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    Options opt;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string la = lower(a);
        auto next = [&](std::string& dst) { if (i + 1 < argc) dst = argv[++i]; };
        if (la == "-inputpath" || la == "-inputxml" || la == "-i") next(opt.input);
        else if (la == "-outputpath" || la == "-outputbin" || la == "-o") next(opt.output);
        else if (la == "-recurse" || la == "-r") opt.recurse = true;
        else if (la == "-verify") opt.verify = true;
        else if (la == "-strict") opt.strict = true;
        else if (la == "-particlesuffix") next(opt.particleSuffix);
        else if (la == "-layoutsuffix") next(opt.layoutSuffix);
        else if (la == "-combinesprite") { std::string v; next(v); if (!v.empty()) opt.sprites.push_back(v); }
        else if (la == "-extension") next(opt.extension);
        else if (la == "-h" || la == "-help" || la == "--help" || la == "/?") opt.help = true;
        else positional.push_back(a);
    }
    if (opt.input.empty() && !positional.empty()) opt.input = positional[0];

    if (opt.help || (opt.input.empty() && opt.sprites.empty())) {
        printUsage();
        return opt.help ? 0 : 1;
    }

    std::vector<std::string> files;
    std::string baseDir;
    bool inputIsDir = false;
    if (!opt.input.empty()) {
        inputIsDir = isDirectory(opt.input);
        if (inputIsDir) {
            collectXml(opt.input, opt.recurse, files);
            baseDir = opt.input;
            while (!baseDir.empty() && (baseDir.back() == '\\' || baseDir.back() == '/')) baseDir.pop_back();
        } else if (isFile(opt.input)) {
            files.push_back(opt.input);
            baseDir = dirName(opt.input);
        } else {
            col(C_RED, "找不到输入路径: %s\n", opt.input.c_str());
            return 1;
        }
        std::sort(files.begin(), files.end());
        if (files.empty() && opt.sprites.empty()) { col(C_YELLOW, "未找到任何 *.xml 文件\n"); return 0; }
    }

    bool outIsDirMode = false;
    std::string explicitBin, outDir;
    if (!opt.input.empty() && !opt.output.empty()) {
        if (!inputIsDir && !isDirectory(opt.output)) explicitBin = opt.output;
        else { outIsDirMode = true; outDir = opt.output; }
    }

    // ---------------- 横幅 ----------------
    fputc('\n', stdout);
    rule("═", C_CYAN);
    col(C_CYAN, "   东方非想天则  XML → BIN / TXT  转换器  (C++)\n");
    col(C_DCYAN, "   SKMP v1 角色 · SKAP v1 粒子 · LAYOUT 文本布局 · SKSP v1 精灵\n");
    rule("═", C_CYAN);
    if (!opt.input.empty()) {
        col(C_GRAY, "   输入: %s\n", opt.input.c_str());
        col(C_GRAY, "   文件: %d 个 XML %s\n", (int)files.size(), opt.verify ? "（转换后校验）" : "");
    }
    if (!opt.sprites.empty())
        col(C_GRAY, "   精灵: %d 个文件夹%s\n", (int)opt.sprites.size(),
            opt.verify ? "  （转换后校验）" : "");
    rule("─", C_DGRAY);
    fputc('\n', stdout);

    int ok = 0, skipped = 0;
    std::vector<std::pair<std::string, std::string>> failures;
    long long sumXml = 0, sumBin = 0, sumTxt = 0;
    long long txtCount = 0;
    auto totalStart = std::chrono::steady_clock::now();

    for (size_t idx = 0; idx < files.size(); ++idx) {
        const std::string& file = files[idx];
        int i = (int)idx + 1, total = (int)files.size();

        std::string rel = file;
        if (!baseDir.empty() && rel.compare(0, baseDir.size(), baseDir) == 0) {
            rel = rel.substr(baseDir.size());
            while (!rel.empty() && (rel[0] == '\\' || rel[0] == '/')) rel.erase(0, 1);
        }

        std::string outPath;
        std::string status = "FAIL";
        std::string detail;
        std::string role;
        int roleColor = C_DEF;

        try {
            std::string xml;
            if (!readFileBytes(file, xml)) throw std::runtime_error("无法读取 XML: " + file);
            XmlParser parser(xml);
            Node root = parser.parse();
            if (root.name.empty()) throw std::runtime_error("XML 解析失败（无根节点）: " + file);

            if (!root.is("movepattern") && !root.is("animpattern") && !root.is("layout")) {
                ++skipped;
                std::string cnt = strf("%d/%d", i, total);
                while (cnt.size() < 7) cnt = " " + cnt;
                setColor(C_DGRAY); fputs("   [", stdout); setColor(C_GRAY); fputs(cnt.c_str(), stdout);
                setColor(C_DGRAY); fputs("] ", stdout); setColor(C_DYELLOW); fputs("SKIP ", stdout);
                setColor(C_GRAY); printf("%s  （根节点 <%s> 暂不支持）\n", rel.c_str(), root.name.c_str());
                setColor(C_DEF);
                continue;
            }

            bool isLayout = root.is("layout");
            bool isAnim = root.is("animpattern");
            role = isLayout ? "布局" : (isAnim ? "粒子" : "角色");
            roleColor = isLayout ? C_DGREEN : (isAnim ? C_DCYAN : C_MAGENTA);

            std::string suffix = isLayout ? opt.layoutSuffix : (isAnim ? opt.particleSuffix : std::string());
            std::string ext = isLayout ? ".txt" : ".bin";
            std::string name = stemName(file);
            if (!explicitBin.empty())      outPath = explicitBin;
            else if (outIsDirMode)         outPath = joinPath(outDir, name + suffix + ext);
            else                           outPath = joinPath(dirName(file), name + suffix + ext);

            auto fileStart = std::chrono::steady_clock::now();
            long long outBytes = 0, records = 0, frames = 0, nodes = 0, warnings = 0;
            if (isLayout) {
                writeLayout(root, file, outPath, nodes);
                outBytes = fileSize(outPath);
            } else {
                BinStats st = convertBin(root, file, outPath, isAnim, opt.strict);
                records = st.records; frames = st.frames; warnings = st.warnings;
                outBytes = st.bytes;
            }
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - fileStart).count();

            if (opt.verify) {
                if (isLayout) {
                    if (fileSize(outPath) <= 0) throw std::runtime_error("布局文本写出失败: " + outPath);
                } else {
                    VerifyResult vr = verifyBin(outPath);
                    if (vr.bytes != outBytes) throw std::runtime_error("校验后文件大小不一致");
                }
            }

            long long xmlSize = fileSize(file);
            sumXml += xmlSize;
            ++ok;
            std::string ratio = formatPct(outBytes, xmlSize);
            if (isLayout) {
                sumTxt += outBytes; ++txtCount;
                detail = strf("%lld 节点 · %s (%s) · %.1fs", nodes, formatSize(outBytes).c_str(), ratio.c_str(), secs);
            } else {
                sumBin += outBytes;
                detail = strf("%lld %s · %lld 帧 · %s (%s) · %.1fs",
                              records, isAnim ? "动画" : "动作", frames, formatSize(outBytes).c_str(), ratio.c_str(), secs);
            }
            if (warnings > 0) detail += strf(" · %lld 警告", warnings);
            if (opt.verify) detail += " · 校验 OK";
            status = "OK";
        } catch (const std::exception& ex) {
            failures.emplace_back(rel, ex.what());
            clearHeartbeat();                   // 失败行同样原地替换心跳行
            setColor(C_DGRAY); { std::string cnt = strf("%d/%d", i, total); while (cnt.size() < 7) cnt = " " + cnt;
                                 fputs("   [", stdout); setColor(C_GRAY); fputs(cnt.c_str(), stdout);
                                 setColor(C_DGRAY); fputs("] ", stdout); }
            setColor(C_RED); fputs("FAIL ", stdout);
            setColor(C_GRAY); printf("%s  %s\n", rel.c_str(), ex.what());
            setColor(C_DEF);
            continue;
        }

        clearHeartbeat();                       // 结果行原地替换心跳行
        std::string cnt = strf("%d/%d", i, total);
        while (cnt.size() < 7) cnt = " " + cnt;
        setColor(C_DGRAY); fputs("   [", stdout);
        setColor(C_GRAY);  fputs(cnt.c_str(), stdout);
        setColor(C_DGRAY); fputs("] ", stdout);
        setColor(C_GREEN); printf("%-5s", status.c_str());
        setColor(roleColor); printf("[%s] ", role.c_str());
        setColor(C_GRAY); printf("%s  →  %s", rel.c_str(), baseName(outPath).c_str());
        if (!detail.empty()) { setColor(C_DGRAY); printf("  %s", detail.c_str()); }
        fputc('\n', stdout);
        setColor(C_DEF);
    }

    // ---------------- 精灵合并（-CombineSprite） ----------------
    long long spriteBins = 0, sumSprite = 0;
    if (!opt.sprites.empty()) {
        const std::string ext = normalizeExt(opt.extension);
        if (!files.empty()) rule("─", C_DGRAY);
        col(C_CYAN, "   精灵合并（-CombineSprite）%s\n", ext.empty() ? "（图片名不带扩展名）" : ("（扩展名 " + ext + "）").c_str());
        for (size_t k = 0; k < opt.sprites.size(); ++k) {
            const std::string folder = opt.sprites[k];
            std::string folName = baseName(folder);
            while (!folName.empty() && (folName.back() == '\\' || folName.back() == '/')) folName.pop_back();

            std::string outPath;
            if (!opt.output.empty() && isDirectory(opt.output)) outPath = joinPath(opt.output, folName + ".bin");
            else if (!opt.output.empty() && opt.input.empty() && opt.sprites.size() == 1) outPath = opt.output;
            else outPath = joinPath(dirName(folder), folName + ".bin");

            std::string cnt = strf("%d/%d", (int)k + 1, (int)opt.sprites.size());
            while (cnt.size() < 7) cnt = " " + cnt;

            try {
                auto t0 = std::chrono::steady_clock::now();
                SpriteStats sst = combineSpriteBin(folder, outPath, ext, opt.strict);
                double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (opt.verify) {
                    VerifyResult vr = verifyBin(outPath);
                    if (vr.bytes != sst.bytes) throw std::runtime_error("校验后文件大小不一致");
                }
                ++ok; ++spriteBins; sumSprite += sst.bytes;

                std::string detail = strf("%lld 帧 · %s · %.1fs", sst.frames, formatSize(sst.bytes).c_str(), secs);
                if (sst.skipped > 0) detail += strf(" · 跳过 %lld 个非图片", sst.skipped);
                if (sst.warnings > 0) detail += strf(" · %lld 警告", sst.warnings);
                if (opt.verify) detail += " · 校验 OK";

                setColor(C_DGRAY); fputs("   [", stdout);
                setColor(C_GRAY);  fputs(cnt.c_str(), stdout);
                setColor(C_DGRAY); fputs("] ", stdout);
                setColor(C_GREEN); fputs("OK   ", stdout);
                setColor(C_CYAN);  fputs("[精灵] ", stdout);
                setColor(C_GRAY);  printf("%s  →  %s", folder.c_str(), baseName(outPath).c_str());
                setColor(C_DGRAY); printf("  %s\n", detail.c_str());
                setColor(C_DEF);
            } catch (const std::exception& ex) {
                clearHeartbeat();
                failures.emplace_back(folder, ex.what());
                setColor(C_DGRAY); fputs("   [", stdout);
                setColor(C_GRAY);  fputs(cnt.c_str(), stdout);
                setColor(C_DGRAY); fputs("] ", stdout);
                setColor(C_RED);   fputs("FAIL ", stdout);
                setColor(C_GRAY);  printf("%s  %s\n", folder.c_str(), ex.what());
                setColor(C_DEF);
            }
        }
    }

    // ---------------- 汇总 ----------------
    double totalSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - totalStart).count();
    fputc('\n', stdout);
    rule("═", C_CYAN);
    col(C_CYAN, "   汇总\n");
    setColor(C_GRAY); fputs("   成功 ", stdout); setColor(C_GREEN); printf("%d", ok);
    setColor(C_GRAY); fputs("    跳过 ", stdout); setColor(C_YELLOW); printf("%d", skipped);
    setColor(C_GRAY); fputs("    失败 ", stdout);
    setColor(failures.empty() ? C_DGRAY : C_RED); printf("%d\n", (int)failures.size());
    setColor(C_DEF);

    if (sumBin > 0)
        col(C_GRAY, "   XML %s  →  BIN %s   (%s)\n", formatSize(sumXml).c_str(), formatSize(sumBin).c_str(),
            formatPct(sumBin, sumXml).c_str());
    if (txtCount > 0)
        col(C_DGRAY, "   布局 TXT %lld 个 · %s\n", txtCount, formatSize(sumTxt).c_str());
    if (spriteBins > 0)
        col(C_DGRAY, "   精灵 BIN %lld 个 · %s\n", spriteBins, formatSize(sumSprite).c_str());
    col(C_DGRAY, "   用时 %.1f s   平均 %.1f MB/s\n", totalSecs,
        totalSecs > 0 ? (sumXml / 1048576.0) / totalSecs : 0.0);

    if (!failures.empty()) {
        fputc('\n', stdout);
        col(C_RED, "   失败的条目:\n");
        for (auto& f : failures) {
            col(C_RED, "   - %s\n", f.first.c_str());
            col(C_RED, "     %s\n", f.second.c_str());
        }
        rule("═", C_RED);
        return 1;
    }

    col(C_GREEN, "   √ 全部完成\n");
    rule("═", C_CYAN);
    return 0;
}
