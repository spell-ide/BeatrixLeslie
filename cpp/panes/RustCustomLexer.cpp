#include "panes/RustCustomLexer.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace spell {
namespace {

using CustomLang = SpellCustomLexer::CustomLang;
using Style = SpellCustomLexer::Style;

// -----------------------------------------------------------------------------
// Compile-time verified sorted keyword and built-in type tables
// -----------------------------------------------------------------------------

template <std::size_t N>
constexpr bool isStrictlySorted(const std::array<std::string_view, N>& arr) {
    for (std::size_t i = 1; i < N; ++i) {
        if (!(arr[i - 1] < arr[i])) {
            return false;
        }
    }
    return true;
}

template <std::size_t N>
bool containsWord(const std::array<std::string_view, N>& arr, std::string_view word) {
    return std::binary_search(arr.begin(), arr.end(), word);
}

constexpr std::array<std::string_view, 43> kRustKeywords = {
    "as", "async", "await", "box", "break", "const", "continue", "crate",
    "dyn", "else", "enum", "extern", "false", "fn", "for", "gen",
    "if", "impl", "in", "let", "loop", "macro_rules", "match", "mod",
    "move", "mut", "pub", "ref", "return", "self", "static", "struct",
    "super", "trait", "true", "try", "type", "union", "unsafe", "use",
    "where", "while", "yield"
};
static_assert(isStrictlySorted(kRustKeywords));

constexpr std::array<std::string_view, 75> kRustBuiltinTypes = {
    "Arc", "AsMut", "AsRef", "BTreeMap", "BTreeSet", "Box", "Cell", "Clone",
    "Copy", "Cow", "Debug", "Default", "Deref", "DerefMut", "Display", "Drop",
    "Eq", "Err", "Error", "Fn", "FnMut", "FnOnce", "From", "Hash",
    "HashMap", "HashSet", "Into", "IntoIterator", "Iterator", "Mutex", "None", "Ok",
    "Option", "Ord", "OsStr", "OsString", "PartialEq", "PartialOrd", "Path", "PathBuf",
    "PhantomData", "Pin", "Rc", "RefCell", "Result", "RwLock", "Self", "Send",
    "Sized", "Some", "String", "Sync", "TryFrom", "TryInto", "Unpin", "Vec",
    "bool", "char", "f128", "f16", "f32", "f64", "i128", "i16",
    "i32", "i64", "i8", "isize", "str", "u128", "u16", "u32",
    "u64", "u8", "usize"
};
static_assert(isStrictlySorted(kRustBuiltinTypes));

constexpr std::array<std::string_view, 82> kSwiftKeywords = {
    "actor", "any", "as", "associatedtype", "async", "await", "borrowing", "break",
    "case", "catch", "class", "consuming", "continue", "convenience", "default", "defer",
    "deinit", "didSet", "do", "dynamic", "else", "enum", "extension", "fallthrough",
    "false", "fileprivate", "final", "for", "func", "get", "guard", "if",
    "import", "in", "indirect", "infix", "init", "inout", "internal", "is",
    "isolated", "lazy", "let", "macro", "mutating", "nil", "nonisolated", "nonmutating",
    "open", "operator", "optional", "override", "package", "postfix", "precedencegroup", "prefix",
    "private", "protocol", "public", "repeat", "required", "rethrows", "return", "self",
    "set", "some", "static", "struct", "subscript", "super", "switch", "throw",
    "throws", "true", "try", "typealias", "unowned", "var", "weak", "where",
    "while", "willSet"
};
static_assert(isStrictlySorted(kSwiftKeywords));

constexpr std::array<std::string_view, 49> kSwiftBuiltinTypes = {
    "Any", "AnyObject", "Array", "AsyncStream", "Bool", "Character", "ClosedRange", "Codable",
    "Collection", "Comparable", "Data", "Date", "Decodable", "Dictionary", "Double", "Encodable",
    "Equatable", "Error", "Float", "Float16", "Float32", "Float64", "Hashable", "Identifiable",
    "Int", "Int16", "Int32", "Int64", "Int8", "Never", "Optional", "Range",
    "Result", "Self", "Sendable", "Sequence", "Set", "StaticString", "String", "Substring",
    "Task", "UInt", "UInt16", "UInt32", "UInt64", "UInt8", "URL", "UUID",
    "Void"
};
static_assert(isStrictlySorted(kSwiftBuiltinTypes));

constexpr std::array<std::string_view, 29> kGoKeywords = {
    "break", "case", "chan", "const", "continue", "default", "defer", "else",
    "fallthrough", "false", "for", "func", "go", "goto", "if", "import",
    "interface", "iota", "map", "nil", "package", "range", "return", "select",
    "struct", "switch", "true", "type", "var"
};
static_assert(isStrictlySorted(kGoKeywords));

constexpr std::array<std::string_view, 22> kGoBuiltinTypes = {
    "any", "bool", "byte", "comparable", "complex128", "complex64", "error", "float32",
    "float64", "int", "int16", "int32", "int64", "int8", "rune", "string",
    "uint", "uint16", "uint32", "uint64", "uint8", "uintptr"
};
static_assert(isStrictlySorted(kGoBuiltinTypes));

constexpr std::array<std::string_view, 68> kTypeScriptKeywords = {
    "abstract", "accessor", "as", "asserts", "async", "await", "break", "case",
    "catch", "class", "const", "continue", "debugger", "declare", "default", "delete",
    "do", "else", "enum", "export", "extends", "false", "finally", "for",
    "from", "function", "get", "if", "implements", "import", "in", "infer",
    "instanceof", "interface", "is", "keyof", "let", "module", "namespace", "new",
    "null", "of", "override", "package", "private", "protected", "public", "readonly",
    "return", "satisfies", "set", "static", "super", "switch", "this", "throw",
    "true", "try", "type", "typeof", "undefined", "unique", "using", "var",
    "void", "while", "with", "yield"
};
static_assert(isStrictlySorted(kTypeScriptKeywords));

constexpr std::array<std::string_view, 49> kTypeScriptBuiltinTypes = {
    "Array", "ArrayBuffer", "Awaited", "BigInt", "Boolean", "DataView", "Date", "Document",
    "Error", "Exclude", "Extract", "Float32Array", "Float64Array", "Function", "HTMLElement", "InstanceType",
    "Int32Array", "Map", "Node", "NonNullable", "Number", "Object", "Omit", "Parameters",
    "Partial", "Pick", "Promise", "Readonly", "ReadonlyArray", "Record", "RegExp", "Required",
    "ReturnType", "Set", "String", "Symbol", "Uint8Array", "WeakMap", "WeakSet", "Window",
    "any", "bigint", "boolean", "never", "number", "object", "string", "symbol",
    "unknown"
};
static_assert(isStrictlySorted(kTypeScriptBuiltinTypes));

constexpr std::array<std::string_view, 12> kRustIntSuffixes = {
    "i128", "i16", "i32", "i64", "i8", "isize",
    "u128", "u16", "u32", "u64", "u8", "usize"
};
static_assert(isStrictlySorted(kRustIntSuffixes));

constexpr std::array<std::string_view, 4> kRustFloatSuffixes = {
    "f128", "f16", "f32", "f64"
};
static_assert(isStrictlySorted(kRustFloatSuffixes));

bool isKeyword(CustomLang lang, std::string_view word) {
    switch (lang) {
    case CustomLang::Rust:       return containsWord(kRustKeywords, word);
    case CustomLang::Swift:      return containsWord(kSwiftKeywords, word);
    case CustomLang::Go:         return containsWord(kGoKeywords, word);
    case CustomLang::TypeScript: return containsWord(kTypeScriptKeywords, word);
    case CustomLang::Toml:       return word == "true" || word == "false";
    }
    return false;
}

bool isBuiltinType(CustomLang lang, std::string_view word) {
    switch (lang) {
    case CustomLang::Rust:       return containsWord(kRustBuiltinTypes, word);
    case CustomLang::Swift:      return containsWord(kSwiftBuiltinTypes, word);
    case CustomLang::Go:         return containsWord(kGoBuiltinTypes, word);
    case CustomLang::TypeScript: return containsWord(kTypeScriptBuiltinTypes, word);
    case CustomLang::Toml:       return false;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Pure-ASCII character helpers (safe on signed char / UTF-8 buffers)
// -----------------------------------------------------------------------------

constexpr unsigned char u8(char ch) {
    return static_cast<unsigned char>(ch);
}

constexpr bool isAsciiAlpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

constexpr bool isAsciiDigit(unsigned char c) {
    return c >= '0' && c <= '9';
}

constexpr bool isAsciiHexDigit(unsigned char c) {
    return isAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

constexpr bool isAsciiOctDigit(unsigned char c) {
    return c >= '0' && c <= '7';
}

constexpr bool isAsciiBinDigit(unsigned char c) {
    return c == '0' || c == '1';
}

constexpr bool isIdentStart(unsigned char c, CustomLang lang) {
    if (isAsciiAlpha(c) || c == '_') {
        return true;
    }
    return lang == CustomLang::TypeScript && c == '$';
}

constexpr bool isIdentCont(unsigned char c, CustomLang lang) {
    if (isAsciiAlpha(c) || isAsciiDigit(c) || c == '_') {
        return true;
    }
    if (lang == CustomLang::TypeScript && c == '$') {
        return true;
    }
    if (lang == CustomLang::Toml && c == '-') {
        return true;
    }
    return false;
}

constexpr int utf8SequenceLength(unsigned char lead) {
    if (lead < 0x80u) {
        return 1;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        return 2;
    }
    if ((lead & 0xF0u) == 0xE0u) {
        return 3;
    }
    if ((lead & 0xF8u) == 0xF0u) {
        return 4;
    }
    return 1;
}

bool looksLikeTypeName(std::string_view word) {
    if (word.empty()) {
        return false;
    }
    const unsigned char c0 = u8(word[0]);
    if (c0 < 'A' || c0 > 'Z') {
        return false;
    }
    if (word.size() == 1) {
        // Generic type parameters such as T, E, K, V.
        return true;
    }
    // Avoid styling SCREAMING_SNAKE_CASE constants as types.
    for (std::size_t i = 1; i < word.size(); ++i) {
        const unsigned char ch = u8(word[i]);
        if (ch >= 'a' && ch <= 'z') {
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// Incremental multiline state stored in Scintilla per-line state
// -----------------------------------------------------------------------------

enum class LexMode : uint8_t {
    Normal             = 0,
    BlockComment       = 1, // aux = nesting depth (1..255)
    RustRawString      = 2, // aux = '#' delimiter count (0..255)
    DoubleQuoteString  = 3, // Rust multiline "..." string
    TripleDoubleString = 4, // Swift (#..."""..."""#...) / TOML """..."""
    TripleSingleString = 5, // TOML '''...'''
    BacktickString     = 6, // Go raw `...` / TypeScript template `...`
    MultilineAttr      = 7  // Rust #[...] / #![...] (aux = '[' depth 1..255)
};

struct LexState {
    LexMode mode = LexMode::Normal;
    uint8_t aux = 0;
};

constexpr int encodeState(LexState s) {
    return static_cast<int>(static_cast<uint32_t>(s.mode) |
                            (static_cast<uint32_t>(s.aux) << 8));
}

constexpr LexState decodeState(int raw) {
    const uint32_t u = static_cast<uint32_t>(raw);
    const uint32_t m = u & 0xFFu;
    if (m > static_cast<uint32_t>(LexMode::MultilineAttr)) {
        return LexState{};
    }
    return LexState{
        static_cast<LexMode>(m),
        static_cast<uint8_t>((u >> 8) & 0xFFu)
    };
}

// Coalesces consecutive runs of identical style to minimize Scintilla calls.
struct StyleEmitter {
    SpellCustomLexer* lexer = nullptr;
    int pendingStyle = -1;
    int pendingLen = 0;

    void push(int len, int style) {
        if (len <= 0) {
            return;
        }
        if (pendingStyle == style) {
            pendingLen += len;
            return;
        }
        flush();
        pendingStyle = style;
        pendingLen = len;
    }

    void flush() {
        if (pendingLen > 0) {
            lexer->setStyling(pendingLen, pendingStyle);
            pendingLen = 0;
            pendingStyle = -1;
        }
    }
};

// -----------------------------------------------------------------------------
// Token scanning helpers
// -----------------------------------------------------------------------------

bool hasHashes(const char* data, int pos, int lineEnd, int count) {
    if (pos + count > lineEnd) {
        return false;
    }
    for (int k = 0; k < count; ++k) {
        if (data[pos + k] != '#') {
            return false;
        }
    }
    return true;
}

bool tryRustRawStringPrefix(const char* data,
                            int i,
                            int lineEnd,
                            int& prefixLen,
                            uint8_t& hashCount) {
    int p = i;
    if (data[p] == 'r') {
        ++p;
    } else if ((data[p] == 'b' || data[p] == 'c') && p + 1 < lineEnd && data[p + 1] == 'r') {
        p += 2;
    } else {
        return false;
    }

    int hashes = 0;
    while (p < lineEnd && data[p] == '#' && hashes < 255) {
        ++p;
        ++hashes;
    }
    if (p < lineEnd && data[p] == '"') {
        prefixLen = (p + 1) - i;
        hashCount = static_cast<uint8_t>(hashes);
        return true;
    }
    return false;
}

int scanSingleLineQuoted(const char* data,
                         int i,
                         int lineEnd,
                         char quote,
                         bool allowEscape) {
    int j = i + 1;
    while (j < lineEnd) {
        if (allowEscape && data[j] == '\\') {
            j += (j + 1 < lineEnd) ? 2 : 1;
            continue;
        }
        if (data[j] == quote) {
            return j + 1;
        }
        ++j;
    }
    return lineEnd;
}

int scanSwiftRawSingleLineString(const char* data, int i, int lineEnd, int hashes) {
    // Starts right at opening `#...#"` (hashes >= 1).
    int j = i + hashes + 1;
    while (j < lineEnd) {
        if (data[j] == '\\' && hasHashes(data, j + 1, lineEnd, hashes)) {
            j += 1 + hashes;
            if (j < lineEnd) {
                ++j;
            }
            continue;
        }
        if (data[j] == '"' && hasHashes(data, j + 1, lineEnd, hashes)) {
            return j + 1 + hashes;
        }
        ++j;
    }
    return lineEnd;
}

// Disambiguates Rust char literals ('a', '\n', '\u{1F600}', '🦀') from lifetimes ('a, 'static).
bool scanRustSingleQuote(const char* data, int i, int lineEnd, int& outEnd, int& outStyle) {
    if (i + 1 >= lineEnd) {
        outEnd = i + 1;
        outStyle = Style::Default;
        return true;
    }
    const char next = data[i + 1];
    if (next == '\\') {
        // Escaped char literal: '\n', '\'', '\x7f', '\u{...}'
        int j = i + 2;
        if (j < lineEnd) {
            ++j;
        }
        while (j < lineEnd && data[j] != '\'') {
            ++j;
        }
        if (j < lineEnd && data[j] == '\'') {
            ++j;
        }
        outEnd = j;
        outStyle = Style::String;
        return true;
    }

    if (next != '\'') {
        const int uLen = utf8SequenceLength(u8(next));
        if (i + 1 + uLen < lineEnd && data[i + 1 + uLen] == '\'') {
            outEnd = i + 1 + uLen + 1;
            outStyle = Style::String;
            return true;
        }
    }

    if (isAsciiAlpha(u8(next)) || next == '_') {
        // Rust lifetime or loop label: 'a, 'static, '_
        int j = i + 2;
        while (j < lineEnd && (isAsciiAlpha(u8(data[j])) || isAsciiDigit(u8(data[j])) || data[j] == '_')) {
            ++j;
        }
        outEnd = j;
        outStyle = Style::Type;
        return true;
    }

    outEnd = i + 1;
    outStyle = Style::Default;
    return true;
}

bool matchTomlTableHeader(const char* data, int i, int lineEnd, int& outEnd) {
    if (i >= lineEnd || data[i] != '[') {
        return false;
    }
    const bool isArrayTable = (i + 1 < lineEnd && data[i + 1] == '[');
    int j = i + (isArrayTable ? 2 : 1);
    bool hasKeyChar = false;

    while (j < lineEnd) {
        const char ch = data[j];
        if (ch == '"' || ch == '\'') {
            hasKeyChar = true;
            const int qEnd = scanSingleLineQuoted(data, j, lineEnd, ch, ch == '"');
            if (qEnd > lineEnd || data[qEnd - 1] != ch) {
                return false;
            }
            j = qEnd;
            continue;
        }
        if (ch == ']') {
            if (isArrayTable) {
                if (j + 1 >= lineEnd || data[j + 1] != ']') {
                    return false;
                }
                j += 2;
            } else {
                ++j;
            }
            if (!hasKeyChar) {
                return false;
            }
            // Only trailing whitespace or `#` comment may follow a TOML table header.
            int trail = j;
            while (trail < lineEnd && (data[trail] == ' ' || data[trail] == '\t')) {
                ++trail;
            }
            if (trail == lineEnd || data[trail] == '#') {
                outEnd = j;
                return true;
            }
            return false;
        }
        if (ch == ',' || ch == '[' || ch == '=' || ch == '#') {
            return false;
        }
        if (ch != ' ' && ch != '\t') {
            hasKeyChar = true;
        }
        ++j;
    }
    return false;
}

bool isTomlKeyFollowedByEquals(const char* data, int afterIdent, int lineEnd) {
    int k = afterIdent;
    while (k < lineEnd) {
        while (k < lineEnd && (data[k] == ' ' || data[k] == '\t')) {
            ++k;
        }
        if (k >= lineEnd) {
            return false;
        }
        if (data[k] == '=') {
            return true;
        }
        if (data[k] == '.') {
            ++k;
            while (k < lineEnd && (data[k] == ' ' || data[k] == '\t')) {
                ++k;
            }
            if (k >= lineEnd) {
                return false;
            }
            if (data[k] == '"' || data[k] == '\'') {
                const char q = data[k];
                const int qEnd = scanSingleLineQuoted(data, k, lineEnd, q, q == '"');
                if (qEnd <= k + 1 || data[qEnd - 1] != q) {
                    return false;
                }
                k = qEnd;
                continue;
            }
            if (isAsciiAlpha(u8(data[k])) || isAsciiDigit(u8(data[k])) ||
                data[k] == '_' || data[k] == '-') {
                while (k < lineEnd &&
                       (isAsciiAlpha(u8(data[k])) || isAsciiDigit(u8(data[k])) ||
                        data[k] == '_' || data[k] == '-')) {
                    ++k;
                }
                continue;
            }
            return false;
        }
        return false;
    }
    return false;
}

bool matchTomlSpecialFloat(const char* data, int pos, int lineEnd, int& outEnd) {
    if (pos + 3 > lineEnd) {
        return false;
    }
    const std::string_view sv(data + pos, 3);
    if (sv != "inf" && sv != "nan") {
        return false;
    }
    if (pos + 3 < lineEnd && isIdentCont(u8(data[pos + 3]), CustomLang::Toml)) {
        return false;
    }
    outEnd = pos + 3;
    return true;
}

int tryScanTomlDateTime(const char* data, int i, int lineEnd) {
    // Full date: YYYY-MM-DD (optionally followed by T/t/space HH:MM[:SS[.frac]][Z|+/-HH:MM])
    if (i + 10 <= lineEnd &&
        isAsciiDigit(u8(data[i])) && isAsciiDigit(u8(data[i + 1])) &&
        isAsciiDigit(u8(data[i + 2])) && isAsciiDigit(u8(data[i + 3])) &&
        data[i + 4] == '-' &&
        isAsciiDigit(u8(data[i + 5])) && isAsciiDigit(u8(data[i + 6])) &&
        data[i + 7] == '-' &&
        isAsciiDigit(u8(data[i + 8])) && isAsciiDigit(u8(data[i + 9]))) {
        int j = i + 10;
        if (j < lineEnd &&
            (data[j] == 'T' || data[j] == 't' ||
             (data[j] == ' ' && j + 3 < lineEnd &&
              isAsciiDigit(u8(data[j + 1])) && isAsciiDigit(u8(data[j + 2])) &&
              data[j + 3] == ':'))) {
            ++j;
            while (j < lineEnd) {
                const char c = data[j];
                if (isAsciiDigit(u8(c)) || c == ':' || c == '.' ||
                    c == 'Z' || c == 'z' || c == '+' || c == '-') {
                    ++j;
                } else {
                    break;
                }
            }
        }
        return j;
    }

    // Local time: HH:MM[:SS[.frac]]
    if (i + 5 <= lineEnd &&
        isAsciiDigit(u8(data[i])) && isAsciiDigit(u8(data[i + 1])) &&
        data[i + 2] == ':' &&
        isAsciiDigit(u8(data[i + 3])) && isAsciiDigit(u8(data[i + 4]))) {
        int j = i + 5;
        while (j < lineEnd && (isAsciiDigit(u8(data[j])) || data[j] == ':' || data[j] == '.')) {
            ++j;
        }
        return j;
    }

    return -1;
}

int scanRustNumericSuffix(const char* data, int j, int lineEnd, bool allowFloat) {
    int k = j;
    if (k < lineEnd && data[k] == '_') {
        ++k;
    }
    if (k >= lineEnd || !isAsciiAlpha(u8(data[k]))) {
        return j;
    }
    int end = k + 1;
    while (end < lineEnd && (isAsciiAlpha(u8(data[end])) || isAsciiDigit(u8(data[end])) || data[end] == '_')) {
        ++end;
    }
    const std::string_view suffix(data + k, static_cast<std::size_t>(end - k));
    if (containsWord(kRustIntSuffixes, suffix) ||
        (allowFloat && containsWord(kRustFloatSuffixes, suffix))) {
        return end;
    }
    return j;
}

int scanNumber(const char* data, int i, int lineEnd, CustomLang lang) {
    int j = i;

    if (lang == CustomLang::Toml && (data[j] == '+' || data[j] == '-')) {
        ++j;
        int specEnd = 0;
        if (matchTomlSpecialFloat(data, j, lineEnd, specEnd)) {
            return specEnd;
        }
    }

    if (lang == CustomLang::Toml && j == i) {
        const int dtEnd = tryScanTomlDateTime(data, j, lineEnd);
        if (dtEnd > j) {
            return dtEnd;
        }
    }

    // Radix prefixes: 0x / 0X, 0b / 0B, 0o / 0O
    if (j + 1 < lineEnd && data[j] == '0') {
        const char p = data[j + 1];
        if (p == 'x' || p == 'X') {
            j += 2;
            while (j < lineEnd && (isAsciiHexDigit(u8(data[j])) || data[j] == '_')) {
                ++j;
            }
            // Hex floats in Swift and Go: 0x1.fp+3
            if (lang == CustomLang::Swift || lang == CustomLang::Go) {
                if (j < lineEnd && data[j] == '.' &&
                    !(j + 1 < lineEnd && data[j + 1] == '.')) {
                    ++j;
                    while (j < lineEnd && (isAsciiHexDigit(u8(data[j])) || data[j] == '_')) {
                        ++j;
                    }
                }
                if (j < lineEnd && (data[j] == 'p' || data[j] == 'P')) {
                    int exp = j + 1;
                    if (exp < lineEnd && (data[exp] == '+' || data[exp] == '-')) {
                        ++exp;
                    }
                    if (exp < lineEnd && isAsciiDigit(u8(data[exp]))) {
                        j = exp;
                        while (j < lineEnd && (isAsciiDigit(u8(data[j])) || data[j] == '_')) {
                            ++j;
                        }
                    }
                }
            }
            if (lang == CustomLang::Rust) {
                j = scanRustNumericSuffix(data, j, lineEnd, false);
            } else if (lang == CustomLang::TypeScript && j < lineEnd && data[j] == 'n' &&
                       !(j + 1 < lineEnd && isIdentCont(u8(data[j + 1]), lang))) {
                ++j;
            } else if (lang == CustomLang::Go && j < lineEnd && data[j] == 'i' &&
                       !(j + 1 < lineEnd && isIdentCont(u8(data[j + 1]), lang))) {
                ++j;
            }
            return j;
        }
        if (p == 'b' || p == 'B') {
            j += 2;
            while (j < lineEnd && (isAsciiBinDigit(u8(data[j])) || data[j] == '_')) {
                ++j;
            }
            if (lang == CustomLang::Rust) {
                j = scanRustNumericSuffix(data, j, lineEnd, false);
            } else if (lang == CustomLang::TypeScript && j < lineEnd && data[j] == 'n' &&
                       !(j + 1 < lineEnd && isIdentCont(u8(data[j + 1]), lang))) {
                ++j;
            } else if (lang == CustomLang::Go && j < lineEnd && data[j] == 'i' &&
                       !(j + 1 < lineEnd && isIdentCont(u8(data[j + 1]), lang))) {
                ++j;
            }
            return j;
        }
        if (p == 'o' || p == 'O') {
            j += 2;
            while (j < lineEnd && (isAsciiOctDigit(u8(data[j])) || data[j] == '_')) {
                ++j;
            }
            if (lang == CustomLang::Rust) {
                j = scanRustNumericSuffix(data, j, lineEnd, false);
            } else if (lang == CustomLang::TypeScript && j < lineEnd && data[j] == 'n' &&
                       !(j + 1 < lineEnd && isIdentCont(u8(data[j + 1]), lang))) {
                ++j;
            } else if (lang == CustomLang::Go && j < lineEnd && data[j] == 'i' &&
                       !(j + 1 < lineEnd && isIdentCont(u8(data[j + 1]), lang))) {
                ++j;
            }
            return j;
        }
    }

    // Decimal integer / float
    bool isFloat = false;
    if (j < lineEnd && data[j] == '.') {
        isFloat = true;
        ++j;
        while (j < lineEnd && (isAsciiDigit(u8(data[j])) || data[j] == '_')) {
            ++j;
        }
    } else {
        while (j < lineEnd && (isAsciiDigit(u8(data[j])) || data[j] == '_')) {
            ++j;
        }
        if (j < lineEnd && data[j] == '.') {
            // Do not swallow range operators (1..10, 0..<n, ...) or method calls (42.abs()).
            const bool blocked = (j + 1 < lineEnd) &&
                                 (data[j + 1] == '.' || isIdentStart(u8(data[j + 1]), lang));
            if (!blocked) {
                isFloat = true;
                ++j;
                while (j < lineEnd && (isAsciiDigit(u8(data[j])) || data[j] == '_')) {
                    ++j;
                }
            }
        }
    }

    // Decimal exponent: [eE][+-]?[0-9][0-9_]*
    if (j < lineEnd && (data[j] == 'e' || data[j] == 'E')) {
        int exp = j + 1;
        if (exp < lineEnd && (data[exp] == '+' || data[exp] == '-')) {
            ++exp;
        }
        if (exp < lineEnd && isAsciiDigit(u8(data[exp]))) {
            isFloat = true;
            j = exp;
            while (j < lineEnd && (isAsciiDigit(u8(data[j])) || data[j] == '_')) {
                ++j;
            }
        }
    }

    if (lang == CustomLang::Rust) {
        j = scanRustNumericSuffix(data, j, lineEnd, true);
    } else if (lang == CustomLang::TypeScript && !isFloat && j < lineEnd && data[j] == 'n' &&
               !(j + 1 < lineEnd && isIdentCont(u8(data[j + 1]), lang))) {
        ++j;
    } else if (lang == CustomLang::Go && j < lineEnd && data[j] == 'i' &&
               !(j + 1 < lineEnd && isIdentCont(u8(data[j + 1]), lang))) {
        ++j;
    }

    return j;
}

bool isFollowedByCall(const char* data, int afterIdent, int lineEnd, CustomLang lang) {
    int k = afterIdent;
    while (k < lineEnd && (data[k] == ' ' || data[k] == '\t')) {
        ++k;
    }
    if (k < lineEnd && data[k] == '(') {
        return true;
    }
    if (lang == CustomLang::Rust && k + 2 < lineEnd &&
        data[k] == ':' && data[k + 1] == ':' && data[k + 2] == '<') {
        return true;
    }
    // Generic function call / declaration without space before `<`: foo<T>(...)
    if ((lang == CustomLang::TypeScript || lang == CustomLang::Swift || lang == CustomLang::Rust) &&
        afterIdent < lineEnd && data[afterIdent] == '<') {
        int depth = 0;
        int p = afterIdent;
        while (p < lineEnd) {
            const char c = data[p];
            if (c == '<') {
                ++depth;
            } else if (c == '>') {
                --depth;
                if (depth == 0) {
                    ++p;
                    while (p < lineEnd && (data[p] == ' ' || data[p] == '\t')) {
                        ++p;
                    }
                    return p < lineEnd && data[p] == '(';
                }
            } else if (c == ';' || c == '{' || c == '}' || c == '(' || c == ')' ||
                       (c == '&' && p + 1 < lineEnd && data[p + 1] == '&') ||
                       (c == '|' && p + 1 < lineEnd && data[p + 1] == '|') ||
                       (c == '=' && !(p + 1 < lineEnd && data[p + 1] == '>'))) {
                return false;
            }
            ++p;
        }
    }
    // Go generic function call / declaration: Sort[int](xs)
    if (lang == CustomLang::Go && afterIdent < lineEnd && data[afterIdent] == '[') {
        int depth = 0;
        int p = afterIdent;
        while (p < lineEnd) {
            const char c = data[p];
            if (c == '[') {
                ++depth;
            } else if (c == ']') {
                --depth;
                if (depth == 0) {
                    ++p;
                    while (p < lineEnd && (data[p] == ' ' || data[p] == '\t')) {
                        ++p;
                    }
                    return p < lineEnd && data[p] == '(';
                }
            } else if (c == ';' || c == '{' || c == '}') {
                return false;
            }
            ++p;
        }
    }
    return false;
}

// Resumes or finishes an active multiline construct on the current line.
void continueMultilineState(const char* data,
                            int& i,
                            int lineEnd,
                            CustomLang lang,
                            LexState& state,
                            StyleEmitter& emitter) {
    const int segStart = i;

    switch (state.mode) {
    case LexMode::Normal:
        return;

    case LexMode::BlockComment: {
        const bool nested = (lang == CustomLang::Rust || lang == CustomLang::Swift);
        while (i < lineEnd) {
            if (nested && i + 1 < lineEnd && data[i] == '/' && data[i + 1] == '*') {
                if (state.aux < 255) {
                    ++state.aux;
                }
                i += 2;
                continue;
            }
            if (i + 1 < lineEnd && data[i] == '*' && data[i + 1] == '/') {
                i += 2;
                if (!nested || state.aux <= 1) {
                    state = LexState{};
                    break;
                }
                --state.aux;
                continue;
            }
            ++i;
        }
        emitter.push(i - segStart, Style::Comment);
        return;
    }

    case LexMode::RustRawString: {
        const int hashes = static_cast<int>(state.aux);
        while (i < lineEnd) {
            if (data[i] == '"' && hasHashes(data, i + 1, lineEnd, hashes)) {
                i += 1 + hashes;
                state = LexState{};
                break;
            }
            ++i;
        }
        emitter.push(i - segStart, Style::String);
        return;
    }

    case LexMode::DoubleQuoteString: {
        while (i < lineEnd) {
            if (data[i] == '\\') {
                i += (i + 1 < lineEnd) ? 2 : 1;
                continue;
            }
            if (data[i] == '"') {
                ++i;
                state = LexState{};
                break;
            }
            ++i;
        }
        emitter.push(i - segStart, Style::String);
        return;
    }

    case LexMode::TripleDoubleString: {
        const int hashes = static_cast<int>(state.aux);
        while (i < lineEnd) {
            if (hashes == 0 && data[i] == '\\') {
                i += (i + 1 < lineEnd) ? 2 : 1;
                continue;
            }
            if (hashes > 0 && data[i] == '\\' && hasHashes(data, i + 1, lineEnd, hashes)) {
                i += 1 + hashes;
                if (i < lineEnd) {
                    ++i;
                }
                continue;
            }
            if (i + 2 < lineEnd && data[i] == '"' && data[i + 1] == '"' && data[i + 2] == '"') {
                if (lang == CustomLang::Toml) {
                    int qCount = 3;
                    while (qCount < 5 && i + qCount < lineEnd && data[i + qCount] == '"') {
                        ++qCount;
                    }
                    i += qCount;
                    state = LexState{};
                    break;
                }
                if (hasHashes(data, i + 3, lineEnd, hashes)) {
                    i += 3 + hashes;
                    state = LexState{};
                    break;
                }
            }
            ++i;
        }
        emitter.push(i - segStart, Style::String);
        return;
    }

    case LexMode::TripleSingleString: {
        while (i < lineEnd) {
            if (i + 2 < lineEnd && data[i] == '\'' && data[i + 1] == '\'' && data[i + 2] == '\'') {
                int qCount = 3;
                while (qCount < 5 && i + qCount < lineEnd && data[i + qCount] == '\'') {
                    ++qCount;
                }
                i += qCount;
                state = LexState{};
                break;
            }
            ++i;
        }
        emitter.push(i - segStart, Style::String);
        return;
    }

    case LexMode::BacktickString: {
        const bool allowEscape = (lang == CustomLang::TypeScript);
        while (i < lineEnd) {
            if (allowEscape && data[i] == '\\') {
                i += (i + 1 < lineEnd) ? 2 : 1;
                continue;
            }
            if (data[i] == '`') {
                ++i;
                state = LexState{};
                break;
            }
            ++i;
        }
        emitter.push(i - segStart, Style::String);
        return;
    }

    case LexMode::MultilineAttr: {
        while (i < lineEnd) {
            if (data[i] == 'r' || data[i] == 'b' || data[i] == 'c') {
                int rawPrefix = 0;
                uint8_t rawHashes = 0;
                if (tryRustRawStringPrefix(data, i, lineEnd, rawPrefix, rawHashes)) {
                    i += rawPrefix;
                    while (i < lineEnd) {
                        if (data[i] == '"' && hasHashes(data, i + 1, lineEnd, static_cast<int>(rawHashes))) {
                            i += 1 + static_cast<int>(rawHashes);
                            break;
                        }
                        ++i;
                    }
                    continue;
                }
            }
            if (data[i] == '"') {
                i = scanSingleLineQuoted(data, i, lineEnd, '"', true);
                continue;
            }
            if (i + 1 < lineEnd && data[i] == '/' && data[i + 1] == '/') {
                emitter.push(i - segStart, Style::MacroOrAttr);
                emitter.push(lineEnd - i, Style::Comment);
                i = lineEnd;
                return;
            }
            if (data[i] == '[') {
                if (state.aux < 255) {
                    ++state.aux;
                }
                ++i;
                continue;
            }
            if (data[i] == ']') {
                ++i;
                if (state.aux <= 1) {
                    state = LexState{};
                    break;
                }
                --state.aux;
                continue;
            }
            if (state.aux == 1 && (data[i] == ';' || data[i] == '{' || data[i] == '}')) {
                state = LexState{};
                break;
            }
            ++i;
        }
        emitter.push(i - segStart, Style::MacroOrAttr);
        return;
    }
    }
}

void lexLine(const char* data,
             int lineStart,
             int lineEnd,
             int lineNumber,
             CustomLang lang,
             LexState& state,
             StyleEmitter& emitter) {
    int i = lineStart;
    bool atLineStart = (state.mode == LexMode::Normal);
    bool expectFnName = false;
    bool expectTypeName = false;

    if (state.mode != LexMode::Normal) {
        continueMultilineState(data, i, lineEnd, lang, state, emitter);
    }

    // Optional Unix shebang on line 0 (excluding Rust `#![...]` inner attributes).
    if (lineNumber == 0 && i == lineStart && lang != CustomLang::Toml &&
        i + 1 < lineEnd && data[i] == '#' && data[i + 1] == '!' &&
        !(lang == CustomLang::Rust && i + 2 < lineEnd && data[i + 2] == '[')) {
        emitter.push(lineEnd - i, Style::Comment);
        return;
    }

    while (i < lineEnd) {
        const char ch = data[i];
        const unsigned char uch = u8(ch);

        // 1. Horizontal whitespace (preserves atLineStart and declaration context hints)
        if (ch == ' ' || ch == '\t') {
            int j = i + 1;
            while (j < lineEnd && (data[j] == ' ' || data[j] == '\t')) {
                ++j;
            }
            emitter.push(j - i, Style::Default);
            i = j;
            continue;
        }

        const bool wasAtLineStart = atLineStart;
        atLineStart = false;

        // 2. TOML table headers: [section] / [[array_table]]
        if (lang == CustomLang::Toml && wasAtLineStart && ch == '[') {
            int headerEnd = 0;
            if (matchTomlTableHeader(data, i, lineEnd, headerEnd)) {
                emitter.push(headerEnd - i, Style::MacroOrAttr);
                i = headerEnd;
                continue;
            }
        }

        // 3. Comments
        if (lang == CustomLang::Toml) {
            if (ch == '#') {
                emitter.push(lineEnd - i, Style::Comment);
                return;
            }
        } else if (ch == '/' && i + 1 < lineEnd) {
            if (data[i + 1] == '/') {
                emitter.push(lineEnd - i, Style::Comment);
                return;
            }
            if (data[i + 1] == '*') {
                emitter.push(2, Style::Comment);
                i += 2;
                state = LexState{LexMode::BlockComment, 1};
                continueMultilineState(data, i, lineEnd, lang, state, emitter);
                continue;
            }
        }

        // 4. Attributes, decorators, and directives
        if (lang == CustomLang::Rust && ch == '#') {
            if (i + 1 < lineEnd && data[i + 1] == '[') {
                emitter.push(2, Style::MacroOrAttr);
                i += 2;
                state = LexState{LexMode::MultilineAttr, 1};
                continueMultilineState(data, i, lineEnd, lang, state, emitter);
                expectFnName = false;
                expectTypeName = false;
                continue;
            }
            if (i + 2 < lineEnd && data[i + 1] == '!' && data[i + 2] == '[') {
                emitter.push(3, Style::MacroOrAttr);
                i += 3;
                state = LexState{LexMode::MultilineAttr, 1};
                continueMultilineState(data, i, lineEnd, lang, state, emitter);
                expectFnName = false;
                expectTypeName = false;
                continue;
            }
        }

        if (lang == CustomLang::Swift && ch == '#') {
            int h = i;
            while (h < lineEnd && data[h] == '#' && (h - i) < 255) {
                ++h;
            }
            const int hashes = h - i;
            if (h < lineEnd && data[h] == '"') {
                expectFnName = false;
                expectTypeName = false;
                if (h + 2 < lineEnd && data[h + 1] == '"' && data[h + 2] == '"') {
                    emitter.push(hashes + 3, Style::String);
                    i = h + 3;
                    state = LexState{LexMode::TripleDoubleString, static_cast<uint8_t>(hashes)};
                    continueMultilineState(data, i, lineEnd, lang, state, emitter);
                    continue;
                }
                const int strEnd = scanSwiftRawSingleLineString(data, i, lineEnd, hashes);
                emitter.push(strEnd - i, Style::String);
                i = strEnd;
                continue;
            }
            if (hashes == 1 && i + 1 < lineEnd && isAsciiAlpha(u8(data[i + 1]))) {
                int j = i + 2;
                while (j < lineEnd && isIdentCont(u8(data[j]), lang)) {
                    ++j;
                }
                emitter.push(j - i, Style::MacroOrAttr);
                i = j;
                expectFnName = false;
                expectTypeName = false;
                continue;
            }
        }

        if ((lang == CustomLang::Swift || lang == CustomLang::TypeScript) && ch == '@' &&
            i + 1 < lineEnd && isIdentStart(u8(data[i + 1]), lang)) {
            int j = i + 2;
            while (j < lineEnd && isIdentCont(u8(data[j]), lang)) {
                ++j;
            }
            emitter.push(j - i, Style::MacroOrAttr);
            i = j;
            expectFnName = false;
            expectTypeName = false;
            continue;
        }

        // 5. Rust raw strings, byte/C strings, byte char literals, and raw identifiers
        if (lang == CustomLang::Rust) {
            int rawPrefixLen = 0;
            uint8_t rawHashes = 0;
            if (tryRustRawStringPrefix(data, i, lineEnd, rawPrefixLen, rawHashes)) {
                emitter.push(rawPrefixLen, Style::String);
                i += rawPrefixLen;
                state = LexState{LexMode::RustRawString, rawHashes};
                continueMultilineState(data, i, lineEnd, lang, state, emitter);
                expectFnName = false;
                expectTypeName = false;
                continue;
            }
            if ((ch == 'b' || ch == 'c') && i + 1 < lineEnd && data[i + 1] == '"') {
                emitter.push(2, Style::String);
                i += 2;
                state = LexState{LexMode::DoubleQuoteString, 0};
                continueMultilineState(data, i, lineEnd, lang, state, emitter);
                expectFnName = false;
                expectTypeName = false;
                continue;
            }
            if (ch == 'b' && i + 1 < lineEnd && data[i + 1] == '\'') {
                int qEnd = 0;
                int qStyle = Style::String;
                scanRustSingleQuote(data, i + 1, lineEnd, qEnd, qStyle);
                if (qStyle == Style::String) {
                    emitter.push(qEnd - i, Style::String);
                    i = qEnd;
                    expectFnName = false;
                    expectTypeName = false;
                    continue;
                }
            }
            if (ch == 'r' && i + 2 < lineEnd && data[i + 1] == '#' &&
                isIdentStart(u8(data[i + 2]), lang)) {
                int j = i + 3;
                while (j < lineEnd && isIdentCont(u8(data[j]), lang)) {
                    ++j;
                }
                int style = Style::Default;
                if (expectFnName || isFollowedByCall(data, j, lineEnd, lang)) {
                    style = Style::Function;
                } else if (expectTypeName) {
                    style = Style::Type;
                }
                emitter.push(j - i, style);
                i = j;
                expectFnName = false;
                expectTypeName = false;
                continue;
            }
        }

        // 6. Strings & character literals
        if ((lang == CustomLang::Swift || lang == CustomLang::Toml) &&
            ch == '"' && i + 2 < lineEnd && data[i + 1] == '"' && data[i + 2] == '"') {
            emitter.push(3, Style::String);
            i += 3;
            state = LexState{LexMode::TripleDoubleString, 0};
            continueMultilineState(data, i, lineEnd, lang, state, emitter);
            expectFnName = false;
            expectTypeName = false;
            continue;
        }

        if (ch == '"') {
            expectFnName = false;
            expectTypeName = false;
            if (lang == CustomLang::Rust) {
                emitter.push(1, Style::String);
                ++i;
                state = LexState{LexMode::DoubleQuoteString, 0};
                continueMultilineState(data, i, lineEnd, lang, state, emitter);
                continue;
            }
            const int strEnd = scanSingleLineQuoted(data, i, lineEnd, '"', true);
            emitter.push(strEnd - i, Style::String);
            i = strEnd;
            continue;
        }

        if (lang == CustomLang::Toml && ch == '\'' &&
            i + 2 < lineEnd && data[i + 1] == '\'' && data[i + 2] == '\'') {
            emitter.push(3, Style::String);
            i += 3;
            state = LexState{LexMode::TripleSingleString, 0};
            continueMultilineState(data, i, lineEnd, lang, state, emitter);
            continue;
        }

        if (ch == '\'') {
            expectFnName = false;
            expectTypeName = false;
            if (lang == CustomLang::Rust) {
                int qEnd = i + 1;
                int qStyle = Style::Default;
                scanRustSingleQuote(data, i, lineEnd, qEnd, qStyle);
                emitter.push(qEnd - i, qStyle);
                i = qEnd;
                continue;
            }
            if (lang == CustomLang::Go || lang == CustomLang::TypeScript) {
                const int strEnd = scanSingleLineQuoted(data, i, lineEnd, '\'', true);
                emitter.push(strEnd - i, Style::String);
                i = strEnd;
                continue;
            }
            if (lang == CustomLang::Toml) {
                const int strEnd = scanSingleLineQuoted(data, i, lineEnd, '\'', false);
                emitter.push(strEnd - i, Style::String);
                i = strEnd;
                continue;
            }
        }

        if (ch == '`' && (lang == CustomLang::Go || lang == CustomLang::TypeScript)) {
            emitter.push(1, Style::String);
            ++i;
            state = LexState{LexMode::BacktickString, 0};
            continueMultilineState(data, i, lineEnd, lang, state, emitter);
            expectFnName = false;
            expectTypeName = false;
            continue;
        }

        // 7. Numbers (int, float, hex, bin, oct, TOML dates/times/signed inf/nan)
        bool startsNumber = isAsciiDigit(uch);
        if (!startsNumber && (lang == CustomLang::Go || lang == CustomLang::TypeScript) &&
            ch == '.' && i + 1 < lineEnd && isAsciiDigit(u8(data[i + 1]))) {
            startsNumber = true;
        }
        if (!startsNumber && lang == CustomLang::Toml && (ch == '+' || ch == '-') &&
            i + 1 < lineEnd) {
            int dummyEnd = 0;
            if (isAsciiDigit(u8(data[i + 1])) ||
                matchTomlSpecialFloat(data, i + 1, lineEnd, dummyEnd)) {
                startsNumber = true;
            }
        }

        if (startsNumber) {
            const int numEnd = scanNumber(data, i, lineEnd, lang);
            emitter.push(numEnd - i, Style::Number);
            i = numEnd;
            expectFnName = false;
            expectTypeName = false;
            continue;
        }

        // 8. Identifiers, keywords, built-in types, functions, and Rust macros
        if (isIdentStart(uch, lang)) {
            int j = i + 1;
            while (j < lineEnd && isIdentCont(u8(data[j]), lang)) {
                ++j;
            }
            const std::string_view word(data + i, static_cast<std::size_t>(j - i));

            if (lang == CustomLang::Toml) {
                int style = Style::Default;
                if (isTomlKeyFollowedByEquals(data, j, lineEnd)) {
                    style = Style::Keyword;
                } else if (word == "true" || word == "false") {
                    style = Style::Keyword;
                } else if (word == "inf" || word == "nan") {
                    style = Style::Number;
                }
                emitter.push(j - i, style);
                i = j;
                continue;
            }

            // Rust macro invocation: println!, vec!, assert_eq!, macro_rules!
            // Guard against `x!=y` (inequality operator).
            if (lang == CustomLang::Rust && j < lineEnd && data[j] == '!' &&
                !(j + 1 < lineEnd && data[j + 1] == '=')) {
                ++j;
                emitter.push(j - i, Style::MacroOrAttr);
                i = j;
                expectFnName = false;
                expectTypeName = false;
                continue;
            }

            if (isKeyword(lang, word)) {
                emitter.push(j - i, Style::Keyword);
                expectFnName = (lang == CustomLang::Rust && word == "fn") ||
                               ((lang == CustomLang::Swift || lang == CustomLang::Go) && word == "func") ||
                               (lang == CustomLang::TypeScript && word == "function");
                expectTypeName = (word == "struct" || word == "enum" || word == "trait" ||
                                  word == "class" || word == "interface" || word == "protocol" ||
                                  word == "actor" || word == "type" || word == "typealias");
                i = j;
                continue;
            }

            int style = Style::Default;
            if (expectFnName) {
                style = Style::Function;
            } else if (expectTypeName) {
                style = Style::Type;
            } else if (isBuiltinType(lang, word)) {
                style = Style::Type;
            } else if (isFollowedByCall(data, j, lineEnd, lang)) {
                style = Style::Function;
            } else if (looksLikeTypeName(word)) {
                style = Style::Type;
            }

            emitter.push(j - i, style);
            i = j;
            expectFnName = false;
            expectTypeName = false;
            continue;
        }

        // 9. Operators, punctuation, or non-ASCII characters outside strings/comments
        expectFnName = false;
        expectTypeName = false;
        emitter.push(1, Style::Default);
        ++i;
    }
}

} // namespace

// -----------------------------------------------------------------------------
// SpellCustomLexer implementation
// -----------------------------------------------------------------------------

struct SpellCustomLexer::Impl {
    CustomLang lang = CustomLang::Rust;
    CodePalette palette = CodePalette::codePaletteFor(CodeThemeId::OneDark, Appearance::Dark);
    QFont font;
};

SpellCustomLexer::SpellCustomLexer(CustomLang lang, QObject* parent)
    : QsciLexerCustom(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->lang = lang;

    if (qobject_cast<QApplication*>(QCoreApplication::instance()) != nullptr) {
        m_impl->palette = ThemeManager::instance().code();
        m_impl->font = ThemeManager::instance().editorFont();
    } else {
        m_impl->palette = CodePalette::codePaletteFor(CodeThemeId::OneDark, Appearance::Dark);
        m_impl->font = QFont(QStringLiteral("Monospace"));
        m_impl->font.setStyleHint(QFont::Monospace);
        m_impl->font.setPointSizeF(13.0);
    }

    applyCodePalette(m_impl->palette, m_impl->font);
}

SpellCustomLexer::~SpellCustomLexer() = default;

SpellCustomLexer::CustomLang SpellCustomLexer::customLanguage() const {
    return m_impl->lang;
}

void SpellCustomLexer::applyCodePalette(const CodePalette& palette, const QFont& font) {
    m_impl->palette = palette;
    m_impl->font = font;

    const QColor paperColor = palette.background.isValid() ? palette.background : rgbHex(0x000000);
    const QColor defaultFg = palette.plain.isValid() ? palette.plain : rgbHex(0xFFFFFF);

    setDefaultFont(font);
    setDefaultPaper(paperColor);
    setDefaultColor(defaultFg);

    setFont(font, -1);
    setPaper(paperColor, -1);

    for (int style = Style::Default; style <= Style::MacroOrAttr; ++style) {
        setColor(defaultColor(style), style);
    }
}

const char* SpellCustomLexer::language() const {
    switch (m_impl->lang) {
    case CustomLang::Rust:       return "Rust";
    case CustomLang::Swift:      return "Swift";
    case CustomLang::Go:         return "Go";
    case CustomLang::Toml:       return "TOML";
    case CustomLang::TypeScript: return "TypeScript";
    }
    return "Rust";
}

QString SpellCustomLexer::description(int style) const {
    switch (style) {
    case Style::Default:     return QStringLiteral("Default");
    case Style::Keyword:     return QStringLiteral("Keyword");
    case Style::Type:        return QStringLiteral("Type");
    case Style::Function:    return QStringLiteral("Function");
    case Style::String:      return QStringLiteral("String");
    case Style::Number:      return QStringLiteral("Number");
    case Style::Comment:     return QStringLiteral("Comment");
    case Style::MacroOrAttr: return QStringLiteral("Macro / Attribute");
    default:                 return QString();
    }
}

QColor SpellCustomLexer::defaultColor(int style) const {
    const CodePalette& p = m_impl->palette;
    const QColor fallback = p.plain.isValid() ? p.plain : rgbHex(0xFFFFFF);

    switch (style) {
    case Style::Default:     return fallback;
    case Style::Keyword:     return p.keyword.isValid() ? p.keyword : fallback;
    case Style::Type:        return p.type.isValid() ? p.type : fallback;
    case Style::Function:    return p.function.isValid() ? p.function : fallback;
    case Style::String:      return p.string.isValid() ? p.string : fallback;
    case Style::Number:      return p.number.isValid() ? p.number : fallback;
    case Style::Comment:     return p.comment.isValid() ? p.comment : fallback;
    case Style::MacroOrAttr:
        if (p.hunk.isValid()) {
            return p.hunk;
        }
        if (p.function.isValid()) {
            return p.function;
        }
        return p.keyword.isValid() ? p.keyword : fallback;
    default:
        return fallback;
    }
}

QColor SpellCustomLexer::defaultPaper(int style) const {
    Q_UNUSED(style);
    return m_impl->palette.background.isValid() ? m_impl->palette.background : rgbHex(0x000000);
}

QFont SpellCustomLexer::defaultFont(int style) const {
    Q_UNUSED(style);
    return m_impl->font;
}

void SpellCustomLexer::styleText(int start, int end) {
    QsciScintilla* ed = editor();
    if (!ed) {
        return;
    }

    const int docLen = static_cast<int>(ed->SendScintilla(QsciScintillaBase::SCI_GETLENGTH));
    if (start < 0) {
        start = 0;
    }
    if (end > docLen) {
        end = docLen;
    }
    if (end <= start) {
        return;
    }

    // Align start to line start so incremental calls never split a token mid-line.
    const int firstLine = static_cast<int>(
        ed->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,
                          static_cast<unsigned long>(start)));
    const int lineOrigin = static_cast<int>(
        ed->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE,
                          static_cast<unsigned long>(firstLine)));
    if (lineOrigin >= 0 && lineOrigin < start) {
        start = lineOrigin;
    }

    // QsciScintilla::bytes(start, end) appends a trailing NUL byte; clamp to end - start.
    const QByteArray bytes = ed->bytes(start, end);
    const char* data = bytes.constData();
    const int len = std::min(end - start, static_cast<int>(bytes.size()));
    if (len <= 0) {
        return;
    }

    LexState state{};
    if (firstLine > 0) {
        const int prevRaw = static_cast<int>(
            ed->SendScintilla(QsciScintillaBase::SCI_GETLINESTATE,
                              static_cast<unsigned long>(firstLine - 1)));
        state = decodeState(prevRaw);
    }

    startStyling(start);
    StyleEmitter emitter{this, -1, 0};

    int curLine = firstLine;
    int pos = 0;
    while (pos < len) {
        int lineEnd = pos;
        while (lineEnd < len && data[lineEnd] != '\n' && data[lineEnd] != '\r') {
            ++lineEnd;
        }

        int eolEnd = lineEnd;
        if (eolEnd < len) {
            if (data[eolEnd] == '\r') {
                ++eolEnd;
                if (eolEnd < len && data[eolEnd] == '\n') {
                    ++eolEnd;
                }
            } else if (data[eolEnd] == '\n') {
                ++eolEnd;
            }
        }

        lexLine(data, pos, lineEnd, curLine, m_impl->lang, state, emitter);

        if (eolEnd > lineEnd) {
            emitter.push(eolEnd - lineEnd, Style::Default);
        }

        // Persist exit state whenever we styled through the end of this line.
        if (eolEnd > lineEnd || start + eolEnd >= docLen) {
            ed->SendScintilla(QsciScintillaBase::SCI_SETLINESTATE,
                              static_cast<unsigned long>(curLine),
                              static_cast<long>(encodeState(state)));
        }

        pos = eolEnd;
        ++curLine;
    }

    emitter.flush();
}

} // namespace spell
