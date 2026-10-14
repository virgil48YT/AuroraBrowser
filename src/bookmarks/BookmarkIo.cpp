#include "bookmarks/BookmarkIo.hpp"

#include "bookmarks/BookmarkService.hpp"

#include <charconv>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace aurora::bookmarks {
namespace {

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

std::string escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            case '\n': out += "&#10;"; break;
            case '\r': out += "&#13;"; break;
            case '\t': out += "&#9;"; break;
            default: out += c; break;
        }
    }
    return out;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

std::string unescape(std::string_view text) {
    // Browsers emit UTF-8, the five markup entities, and numeric references.
    // Legacy HTML4 named entities are accepted too; unknown ones stay literal
    // rather than silently losing user data. Decode once (not recursively).
    static const std::map<std::string_view, uint32_t> named{
        {"AElig", 198}, {"AMP", 38}, {"Aacute", 193}, {"Acirc", 194},
        {"Agrave", 192}, {"Alpha", 913}, {"Aring", 197}, {"Atilde", 195},
        {"Auml", 196}, {"Beta", 914}, {"Ccedil", 199}, {"Chi", 935},
        {"Dagger", 8225}, {"Delta", 916}, {"ETH", 208}, {"Eacute", 201},
        {"Ecirc", 202}, {"Egrave", 200}, {"Epsilon", 917}, {"Eta", 919},
        {"Euml", 203}, {"GT", 62}, {"Gamma", 915}, {"Iacute", 205},
        {"Icirc", 206}, {"Igrave", 204}, {"Iota", 921}, {"Iuml", 207},
        {"Kappa", 922}, {"LT", 60}, {"Lambda", 923}, {"Mu", 924},
        {"Ntilde", 209}, {"Nu", 925}, {"OElig", 338}, {"Oacute", 211},
        {"Ocirc", 212}, {"Ograve", 210}, {"Omega", 937}, {"Omicron", 927},
        {"Oslash", 216}, {"Otilde", 213}, {"Ouml", 214}, {"Phi", 934},
        {"Pi", 928}, {"Prime", 8243}, {"Psi", 936}, {"QUOT", 34},
        {"Rho", 929}, {"Scaron", 352}, {"Sigma", 931}, {"THORN", 222},
        {"Tau", 932}, {"Theta", 920}, {"Uacute", 218}, {"Ucirc", 219},
        {"Ugrave", 217}, {"Upsilon", 933}, {"Uuml", 220}, {"Xi", 926},
        {"Yacute", 221}, {"Yuml", 376}, {"Zeta", 918}, {"aacute", 225},
        {"acirc", 226}, {"acute", 180}, {"aelig", 230}, {"agrave", 224},
        {"alefsym", 8501}, {"alpha", 945}, {"amp", 38}, {"and", 8743},
        {"ang", 8736}, {"apos", 39}, {"aring", 229}, {"asymp", 8776},
        {"atilde", 227}, {"auml", 228}, {"bdquo", 8222}, {"beta", 946},
        {"brvbar", 166}, {"bull", 8226}, {"cap", 8745}, {"ccedil", 231},
        {"cedil", 184}, {"cent", 162}, {"chi", 967}, {"circ", 710},
        {"clubs", 9827}, {"cong", 8773}, {"copy", 169}, {"crarr", 8629},
        {"cup", 8746}, {"curren", 164}, {"dArr", 8659}, {"dagger", 8224},
        {"darr", 8595}, {"deg", 176}, {"delta", 948}, {"diams", 9830},
        {"divide", 247}, {"eacute", 233}, {"ecirc", 234}, {"egrave", 232},
        {"empty", 8709}, {"emsp", 8195}, {"ensp", 8194}, {"epsilon", 949},
        {"equiv", 8801}, {"eta", 951}, {"eth", 240}, {"euml", 235},
        {"euro", 8364}, {"exist", 8707}, {"fnof", 402}, {"forall", 8704},
        {"frac12", 189}, {"frac14", 188}, {"frac34", 190}, {"frasl", 8260},
        {"gamma", 947}, {"ge", 8805}, {"gt", 62}, {"hArr", 8660},
        {"harr", 8596}, {"hearts", 9829}, {"hellip", 8230}, {"iacute", 237},
        {"icirc", 238}, {"iexcl", 161}, {"igrave", 236}, {"image", 8465},
        {"infin", 8734}, {"int", 8747}, {"iota", 953}, {"iquest", 191},
        {"isin", 8712}, {"iuml", 239}, {"kappa", 954}, {"lArr", 8656},
        {"lambda", 955}, {"lang", 9001}, {"laquo", 171}, {"larr", 8592},
        {"lceil", 8968}, {"ldquo", 8220}, {"le", 8804}, {"lfloor", 8970},
        {"lowast", 8727}, {"loz", 9674}, {"lrm", 8206}, {"lsaquo", 8249},
        {"lsquo", 8216}, {"lt", 60}, {"macr", 175}, {"mdash", 8212},
        {"micro", 181}, {"middot", 183}, {"minus", 8722}, {"mu", 956},
        {"nabla", 8711}, {"nbsp", 160}, {"ndash", 8211}, {"ne", 8800},
        {"ni", 8715}, {"not", 172}, {"notin", 8713}, {"nsub", 8836},
        {"ntilde", 241}, {"nu", 957}, {"oacute", 243}, {"ocirc", 244},
        {"oelig", 339}, {"ograve", 242}, {"oline", 8254}, {"omega", 969},
        {"omicron", 959}, {"oplus", 8853}, {"or", 8744}, {"ordf", 170},
        {"ordm", 186}, {"oslash", 248}, {"otilde", 245}, {"otimes", 8855},
        {"ouml", 246}, {"para", 182}, {"part", 8706}, {"permil", 8240},
        {"perp", 8869}, {"phi", 966}, {"pi", 960}, {"piv", 982},
        {"plusmn", 177}, {"pound", 163}, {"prime", 8242}, {"prod", 8719},
        {"prop", 8733}, {"psi", 968}, {"quot", 34}, {"rArr", 8658},
        {"radic", 8730}, {"rang", 9002}, {"raquo", 187}, {"rarr", 8594},
        {"rceil", 8969}, {"rdquo", 8221}, {"real", 8476}, {"reg", 174},
        {"rfloor", 8971}, {"rho", 961}, {"rlm", 8207}, {"rsaquo", 8250},
        {"rsquo", 8217}, {"sbquo", 8218}, {"scaron", 353}, {"sdot", 8901},
        {"sect", 167}, {"shy", 173}, {"sigma", 963}, {"sigmaf", 962},
        {"sim", 8764}, {"spades", 9824}, {"sub", 8834}, {"sube", 8838},
        {"sum", 8721}, {"sup", 8835}, {"sup1", 185}, {"sup2", 178},
        {"sup3", 179}, {"supe", 8839}, {"szlig", 223}, {"tau", 964},
        {"there4", 8756}, {"theta", 952}, {"thetasym", 977}, {"thinsp", 8201},
        {"thorn", 254}, {"tilde", 732}, {"times", 215}, {"trade", 8482},
        {"uArr", 8657}, {"uacute", 250}, {"uarr", 8593}, {"ucirc", 251},
        {"ugrave", 249}, {"uml", 168}, {"upsih", 978}, {"upsilon", 965},
        {"uuml", 252}, {"weierp", 8472}, {"xi", 958}, {"yacute", 253},
        {"yen", 165}, {"yuml", 255}, {"zeta", 950}, {"zwj", 8205},
        {"zwnj", 8204}
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') { out += text[i]; continue; }
        const auto tail = text.substr(i + 1, 32);
        const auto relative_end = tail.find(';');
        if (relative_end == std::string_view::npos) {
            out += '&';
            continue;
        }
        const auto end = i + 1 + relative_end;
        auto entity = text.substr(i + 1, end - i - 1);
        std::optional<uint32_t> cp;
        if (!entity.empty() && entity.front() == '#') {
            entity.remove_prefix(1);
            int base = 10;
            if (!entity.empty() && (entity.front() == 'x' || entity.front() == 'X')) {
                base = 16;
                entity.remove_prefix(1);
            }
            uint32_t value = 0;
            const auto parsed = std::from_chars(entity.data(), entity.data() + entity.size(), value, base);
            if (parsed.ec == std::errc{} && parsed.ptr == entity.data() + entity.size()) cp = value;
        } else if (const auto it = named.find(entity); it != named.end()) {
            cp = it->second;
        }
        if (cp) { append_utf8(out, *cp); i = end; }
        else out += '&';
    }
    return out;
}

struct Tag {
    std::string name;
    std::map<std::string, std::string> attributes;
    bool closing = false;
    bool malformed = false;
};

Tag read_tag(std::string_view html, std::size_t& pos) {
    Tag tag;
    ++pos; // '<'
    if (pos < html.size() && html[pos] == '/') { tag.closing = true; ++pos; }
    while (pos < html.size() && space(html[pos])) ++pos;
    auto start = pos;
    while (pos < html.size() && !space(html[pos]) && html[pos] != '>' &&
           html[pos] != '<' && html[pos] != '/') ++pos;
    tag.name = lower(html.substr(start, pos - start));
    while (pos < html.size() && html[pos] != '<') {
        if (space(html[pos]) || html[pos] == '/') { ++pos; continue; }
        if (html[pos] == '>') { ++pos; return tag; }
        start = pos;
        while (pos < html.size() && !space(html[pos]) && html[pos] != '=' &&
               html[pos] != '>' && html[pos] != '<') ++pos;
        const auto name = lower(html.substr(start, pos - start));
        while (pos < html.size() && space(html[pos])) ++pos;
        std::string_view value;
        if (pos < html.size() && html[pos] == '=') {
            ++pos;
            while (pos < html.size() && space(html[pos])) ++pos;
            const char quote = pos < html.size() && (html[pos] == '\'' || html[pos] == '"')
                                   ? html[pos++] : '\0';
            start = pos;
            while (pos < html.size() && html[pos] != '<' &&
                   (quote ? html[pos] != quote : !space(html[pos]) && html[pos] != '>')) ++pos;
            value = html.substr(start, pos - start);
            if (quote) {
                if (pos < html.size() && html[pos] == quote) ++pos;
                else tag.malformed = true;
            }
        }
        // Large embedded favicons are irrelevant to the bookmark tree.
        if (name == "href" || name == "add_date")
            tag.attributes.emplace(name, unescape(value));
    }
    tag.malformed = true;
    return tag;
}

bool allowed_url(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) return false;
    const auto prefix = lower(url.substr(0, scheme));
    if (prefix != "http" && prefix != "https") return false;
    const auto host = url.substr(scheme + 3);
    if (host.empty() || host.front() == '/' || host.front() == '?' || host.front() == '#') return false;
    for (char c : url) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7f) return false;
    }
    return true;
}

} // namespace

std::string export_netscape_html(const BookmarkService& service) {
    std::string out = "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
                      "<!-- This is an automatically generated file. -->\n"
                      "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
                      "<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n";
    // Explicit recursion stack: imported trees can be much deeper than the
    // UI's display limit. A corrupt parent cycle must never loop forever.
    struct Frame { std::vector<BookmarkNode> children; std::size_t next = 0; };
    std::vector<Frame> stack{{service.children(0), 0}};
    std::set<int64_t> visited{0};
    while (!stack.empty()) {
        auto& frame = stack.back();
        if (frame.next == frame.children.size()) {
            out += "</DL><p>\n";
            stack.pop_back();
            continue;
        }
        const auto node = frame.children[frame.next++];
        if (!visited.insert(node.id).second) continue;
        const auto date = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
            node.added_at.time_since_epoch()).count());
        if (node.is_folder) {
            out += "<DT><H3 ADD_DATE=\"" + date + "\">" + escape(node.title) + "</H3>\n<DL><p>\n";
            stack.push_back({service.children(node.id), 0});
        } else {
            out += "<DT><A HREF=\"" + escape(node.url) + "\" ADD_DATE=\"" + date +
                   "\">" + escape(node.title) + "</A>\n";
        }
    }
    return out;
}

BookmarkImportResult import_netscape_html(BookmarkService& service,
                                         std::string_view html, int64_t parent_id) {
    BookmarkImportResult result;
    std::vector<int64_t> parents{parent_id};
    std::optional<int64_t> pending_folder;
    std::optional<Tag> entry;
    std::string title;
    std::string raw_element;
    bool description = false;

    const auto finish = [&](bool unclosed) {
        if (!entry) return;
        bool malformed = entry->malformed || unclosed;
        const bool folder = entry->name == "h3";
        auto added = std::chrono::system_clock::now();
        if (const auto it = entry->attributes.find("add_date"); it != entry->attributes.end()) {
            const auto date = trim(it->second);
            int64_t seconds = 0;
            const auto parsed = std::from_chars(date.data(), date.data() + date.size(), seconds);
            const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::duration::max()).count();
            if (parsed.ec == std::errc{} && parsed.ptr == date.data() + date.size() &&
                seconds >= 0 && seconds <= max_seconds) {
                added = std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
            } else malformed = true;
        }
        if (malformed) ++result.malformed;
        std::string url;
        if (const auto it = entry->attributes.find("href"); it != entry->attributes.end()) {
            url = std::string(trim(it->second));
        }
        int64_t id = 0;
        if (!entry->malformed && (folder || allowed_url(url))) {
            id = service.add(parents.back(), unescape(title), folder ? "" : url, folder, added);
        }
        if (id == 0) ++result.skipped;
        else if (folder) ++result.folders_imported;
        else ++result.bookmarks_imported;
        if (folder) pending_folder = id > 0 ? id : -1;
        entry.reset();
        title.clear();
    };

    std::size_t pos = 0;
    while (pos < html.size()) {
        if (html[pos] != '<') {
            const auto end = html.find('<', pos);
            const auto length = (end == std::string_view::npos ? html.size() : end) - pos;
            if (entry && raw_element.empty()) title.append(html.substr(pos, length));
            pos += length;
            continue;
        }
        if (html.substr(pos, 4) == "<!--") {
            const auto end = html.find("-->", pos + 4);
            pos = end == std::string_view::npos ? html.size() : end + 3;
            continue;
        }
        auto tag = read_tag(html, pos);
        if (!raw_element.empty()) {
            if (tag.closing && tag.name == raw_element) raw_element.clear();
            continue;
        }
        if (!tag.closing && (tag.name == "script" || tag.name == "style")) {
            raw_element = tag.name;
            continue;
        }
        if (tag.name == "dt" || tag.name == "dl") description = false;
        if (description) continue;
        const bool boundary = tag.name == "dl" || tag.name == "dd" ||
            (!tag.closing && (tag.name == "dt" || tag.name == "a" || tag.name == "h3"));
        if (entry && tag.closing && tag.name == entry->name) {
            finish(false);
            continue;
        }
        if (boundary) finish(true);
        if (tag.name == "dd") {
            description = !tag.closing;
        } else if (tag.name == "dl") {
            if (tag.closing) {
                if (parents.size() > 1) parents.pop_back();
            } else {
                parents.push_back(pending_folder.value_or(parents.back()));
            }
            pending_folder.reset();
        } else if (!tag.closing && (tag.name == "a" || tag.name == "h3")) {
            pending_folder.reset();
            entry = std::move(tag);
        } else if (!tag.closing && tag.name == "dt") {
            pending_folder.reset();
        }
    }
    finish(true);
    return result;
}

} // namespace aurora::bookmarks
