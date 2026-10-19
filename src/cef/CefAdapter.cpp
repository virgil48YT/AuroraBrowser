#include "cef/CefAdapter.hpp"
#include "cef/ContextMenuText.hpp"

#include "adblock/AdblockEngine.hpp"
#include "adblock/AdblockService.hpp"
#include "cef/MediaPolicy.hpp"
#include "cef/CefRuntime.hpp"
#include "privacy/CertificateAudit.hpp"
#include "settings/Settings.hpp"
#include "util/Log.hpp"
#include "util/Sha256.hpp"

#include "include/cef_audio_handler.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_cookie.h"
#include "include/cef_download_handler.h"
#include "include/cef_download_item.h"
#include "include/cef_image.h"
#include "include/cef_navigation_entry.h"
#include "include/cef_parser.h"
#include "include/cef_permission_handler.h"
#include "include/cef_request_context.h"
#include "include/cef_request_context_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_request_handler.h"
#include "include/cef_ssl_info.h"
#include "include/cef_ssl_status.h"
#include "include/cef_task.h"
#include "include/cef_values.h"

#include "permissions/PermissionStore.hpp"
#include "privacy/CookieSite.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(OS_WIN)
#include <windows.h>
#endif

#if defined(OS_MAC)
#include "cef/CefMacSupport.hpp"
#endif

#if defined(OS_LINUX)
// Resizing a child browser window on X11 means moving CEF's own window, and
// CEF exposes no API for it — cefclient does the same thing through its
// toolkit. We declare the two Xlib entry points we need instead of including
// Xlib.h, because Xlib's macros (None, Status, Bool, Success) collide with
// both CEF and the standard library. cef_get_xdisplay() hands us the very
// display connection CEF is using, so no second connection is opened.
extern "C" {
int XMoveResizeWindow(XDisplay* display, unsigned long window, int x, int y,
                      unsigned int width, unsigned int height);
int XFlush(XDisplay* display);
}
#endif

namespace aurora::cef {

namespace {

using AskWhereProvider = std::function<bool()>;
using DownloadsDirProvider = std::function<std::string()>;
using SavePathChooser =
    std::function<void(std::string, std::function<void(std::string)>)>;

// Installed by the Qt application before it creates the first browser. The
// providers read Settings on demand, so changes made on aurora://settings apply
// to existing tabs without rebuilding their CEF clients.
AskWhereProvider g_ask_where_provider;
DownloadsDirProvider g_downloads_dir_provider;
SavePathChooser g_save_path_chooser;

// Only the UI thread touches these borrowed profile services. The network
// hot path uses atomics, just as ad blocking uses a separate locked snapshot.
settings::Settings* g_privacy_prefs = nullptr;
privacy::CertificateAudit* g_certificate_audit = nullptr;
std::atomic<bool> g_https_only{settings::PrefDefaults::kHttpsOnly};
std::atomic<bool> g_referer_trim{settings::PrefDefaults::kRefererTrim};
std::atomic<bool> g_cert_audit{settings::PrefDefaults::kCertAudit};
// "Send a Do Not Track request" and "Block third-party cookies" from
// aurora://settings. Both are read on the IO thread (OnBeforeResourceLoad /
// GetCookieAccessFilter), hence atomics refreshed from the UI thread exactly
// like the three above.
std::atomic<bool> g_send_dnt{settings::PrefDefaults::kSendDoNotTrack};
std::atomic<bool> g_block_3p_cookies{
    settings::PrefDefaults::kBlockThirdPartyCookies};

// Use Chromium's parser, not core::Url's display parser, for security keys.
// In particular query-only URLs, credentials, IPv6 and default ports must
// never become distinct pins or leak into an outgoing Referer.
std::string web_origin(const CefString& url) {
    CefURLParts parts;
    if (!CefParseURL(url, parts)) return {};
    const auto scheme = CefString(&parts.scheme).ToString();
    auto host = CefString(&parts.host).ToString();
    const auto port = CefString(&parts.port).ToString();
    if ((scheme != "http" && scheme != "https") || host.empty()) return {};
    if (host.find(':') != std::string::npos && host.front() != '[')
        host = "[" + host + "]";
    std::string origin = scheme + "://" + host;
    if (!port.empty() && !((scheme == "https" && port == "443") ||
                          (scheme == "http" && port == "80")))
        origin += ":" + port;
    return origin;
}

std::string certificate_fingerprint(CefRefPtr<CefX509Certificate> certificate) {
    if (!certificate) return {};
    const auto der = certificate->GetDEREncoded();
    if (!der || der->GetSize() == 0) return {};
    std::vector<unsigned char> bytes(der->GetSize());
    if (der->GetData(bytes.data(), bytes.size(), 0) != bytes.size()) return {};
    util::Sha256 hash;
    hash.update(bytes.data(), bytes.size());
    return hash.finish_hex();
}

// The registrable-domain-like site of an http(s)/ws(s) URL
// (privacy::cookie_site, an embedded-table approximation of the Public Suffix
// List — see privacy/CookieSite.hpp), or empty for anything else. The host
// comes from Chromium's own URL parser, so it is already canonical
// (lower-case, IDNA punycode).
std::string url_cookie_site(const CefString& url) {
    CefURLParts parts;
    if (!CefParseURL(url, parts)) return {};
    const auto scheme = CefString(&parts.scheme).ToString();
    if (scheme != "http" && scheme != "https" && scheme != "ws" &&
        scheme != "wss")
        return {};
    return privacy::cookie_site(CefString(&parts.host).ToString());
}

// Third-party cookie filter for one request, both directions: CanSendCookie
// gates every cookie Chromium would attach in the Cookie request header, and
// CanSaveCookie gates every Set-Cookie of the response. Handed out by
// TabClient::GetCookieAccessFilter with the top-level page's site captured at
// request start. The decision is re-made per cookie from the request's
// CURRENT url, so a first-party sub-resource that redirects to a tracker
// loses its cookies on the third-party hop, and it re-reads the setting, so
// flipping the toggle off takes effect even for a request already in flight.
class ThirdPartyCookieFilter final : public CefCookieAccessFilter {
public:
    explicit ThirdPartyCookieFilter(std::string top_site)
        : top_site_(std::move(top_site)) {}

    bool CanSendCookie(CefRefPtr<CefBrowser> /*browser*/,
                       CefRefPtr<CefFrame> /*frame*/,
                       CefRefPtr<CefRequest> request,
                       const CefCookie& /*cookie*/) override {
        return !is_third_party(request);
    }
    bool CanSaveCookie(CefRefPtr<CefBrowser> /*browser*/,
                       CefRefPtr<CefFrame> /*frame*/,
                       CefRefPtr<CefRequest> request,
                       CefRefPtr<CefResponse> /*response*/,
                       const CefCookie& /*cookie*/) override {
        return !is_third_party(request);
    }

private:
    bool is_third_party(const CefRefPtr<CefRequest>& request) const {
        if (!g_block_3p_cookies.load() || !request) return false;
        const std::string site = url_cookie_site(request->GetURL());
        // Non-web schemes carry no cookies worth judging; leave them alone.
        return !site.empty() && site != top_site_;
    }

    const std::string top_site_;
    IMPLEMENT_REFCOUNTING(ThirdPartyCookieFilter);
};

// Completion of CefBrowserHost::PrintToPDF, forwarded to the host callback.
// CEF runs OnPdfPrintFinished on the browser UI thread — the thread Aurora
// pumps CEF from — so the host callback can touch Qt directly.
class PdfPrintCallback final : public CefPdfPrintCallback {
public:
    explicit PdfPrintCallback(webview::WebViewController::PdfCallback done)
        : done_(std::move(done)) {}
    void OnPdfPrintFinished(const CefString& /*path*/, bool ok) override {
        // Exactly once, even if CEF were to report twice.
        if (auto done = std::move(done_)) {
            done_ = nullptr;
            done(ok);
        }
    }

private:
    webview::WebViewController::PdfCallback done_;
    IMPLEMENT_REFCOUNTING(PdfPrintCallback);
};

struct CookieClearState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
};

class CookieFlushCallback final : public CefCompletionCallback {
public:
    explicit CookieFlushCallback(std::shared_ptr<CookieClearState> state)
        : state_(std::move(state)) {}
    void OnComplete() override {
        state_->success.store(true);
        state_->done.store(true);
    }
private:
    std::shared_ptr<CookieClearState> state_;
    IMPLEMENT_REFCOUNTING(CookieFlushCallback);
};

class CookieDeleteCallback final : public CefDeleteCookiesCallback {
public:
    CookieDeleteCallback(CefRefPtr<CefCookieManager> manager,
                         std::shared_ptr<CookieClearState> state)
        : manager_(manager), state_(std::move(state)) {}
    void OnComplete(int num_deleted) override {
        if (num_deleted < 0 ||
            !manager_->FlushStore(new CookieFlushCallback(state_)))
            state_->done.store(true);
    }
private:
    CefRefPtr<CefCookieManager> manager_;
    std::shared_ptr<CookieClearState> state_;
    IMPLEMENT_REFCOUNTING(CookieDeleteCallback);
};

// Cancel handles for running downloads, keyed by CEF download id.
//
// CefDownloadItemCallback is the ONLY way to stop a transfer, and CEF hands a
// fresh one to the client on every OnDownloadUpdated rather than exposing it
// on the item — so the newest one per id is kept here. It lives at file scope
// (not on TabClient) because the host cancels by download id alone and has no
// idea which tab's client started the transfer.
//
// g_pending_cancels holds ids the host asked to cancel before CEF had reported
// an update for them (a cancel clicked between OnBeforeDownload and the first
// OnDownloadUpdated); the first update for such an id cancels immediately.
//
// Every CEF download callback and every host cancel request runs on the UI
// thread, so no lock — the same reasoning as TabClient::target_paths_.
std::unordered_map<uint32_t, CefRefPtr<CefDownloadItemCallback>>
    g_download_callbacks;
std::unordered_set<uint32_t> g_pending_cancels;
// Pause/resume requests that raced the first OnDownloadUpdated, exactly like
// g_pending_cancels. A pause clicked between OnBeforeDownload and the first
// update lands here and is applied on that first update. A queued pause and
// a queued resume for the same id cannot both exist: the host only offers one
// of the two buttons per row state, and a queued pause is dropped if the
// download reached a terminal state first.
std::unordered_set<uint32_t> g_pending_pauses;
std::unordered_set<uint32_t> g_pending_resumes;

// --- Ad blocking ---------------------------------------------------------
//
// The blocker's process-global state. The engine, the enabled flag and the
// per-site exception set are written from the UI thread (host calls) and read
// from CEF's IO thread inside OnBeforeResourceLoad, hence the one mutex. The
// per-tab blocked totals are the page-lifetime counts the shield badge shows;
// the host resets them on main-frame navigation via reset_adblock_count().
struct AdblockState {
    std::mutex mutex;
    std::unique_ptr<adblock::AdblockEngine> engine;
    bool enabled = false;
    std::unordered_set<std::string> exceptions;  // site keys, shields down
    std::unordered_map<TabId, uint64_t> blocked; // per page, per tab
    // Set from the UI thread; invoked on the UI thread (see AdblockTask) with
    // the tab and its current total. Copied under the mutex before posting.
    std::function<void(TabId, uint64_t)> notify;
};
AdblockState g_adblock;

// UI-thread-only refresh hooks, removed before each TabClient closes. Shields
// are process-global, so a preference change must reach every open window.
std::unordered_map<TabId, std::function<void()>> g_cosmetic_refreshers;
void refresh_cosmetic_pages() {
    const auto refreshers = g_cosmetic_refreshers;
    for (const auto& [id, refresh] : refreshers) {
        (void)id;
        refresh();
    }
}

// Runs a std::function on a CEF thread via CefPostTask. The IO-thread request
// interceptor cannot call into Qt directly (widgets are UI-thread only and
// the callback captures the window), so it posts one of these to TID_UI.
class AdblockTask final : public CefTask {
public:
    explicit AdblockTask(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Execute() override {
        if (fn_) fn_();
    }

private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(AdblockTask);
};

// Map a Chromium resource type to the adblock-rust request-type spelling.
// The blocker only differentiates a handful; anything unmapped is "other".
const char* adblock_type_for(cef_resource_type_t type) {
    switch (type) {
        case RT_MAIN_FRAME:    return "document";
        case RT_SUB_FRAME:     return "subdocument";
        case RT_STYLESHEET:    return "stylesheet";
        case RT_SCRIPT:        return "script";
        case RT_IMAGE:         return "image";
        case RT_FONT_RESOURCE: return "font";
        case RT_MEDIA:         return "media";
        case RT_XHR:           return "xmlhttprequest";
        case RT_PING:          return "ping";
        case RT_WORKER:
        case RT_SHARED_WORKER:
        case RT_SERVICE_WORKER: return "script";
        case RT_FAVICON:       return "image";
        default:               return "other";
    }
}

// Quote untrusted tab metadata as a JavaScript string literal. ExecuteJavaScript
// receives source directly (there is no HTML parser), so JSON-compatible
// escaping of quotes, slashes and controls is sufficient.
//
// Defined here rather than beside its other callers further down because the
// cosmetic-filter injection below (inside TabClient) quotes filter-list
// selectors with it.
std::string javascript_string(std::string_view value) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (byte < 0x20u) {
                out += "\\u00";
                out.push_back(kHex[(byte >> 4u) & 0x0fu]);
                out.push_back(kHex[byte & 0x0fu]);
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

// --- Cosmetic filtering --------------------------------------------------
//
// The engine supplies declarative resources and expanded scriptlets. The
// small YouTube floor below only targets ad markup; it must not include the
// in-player .video-ads module, any .ytp-ad-skip-button variant, the video or
// .ytp-cued-thumbnail-overlay. Both stylesheet rules and DOM actions exclude
// protected elements (and their ancestors), even for compound selectors.
constexpr const char* kYoutubeCosmeticSelectors[] = {
    ".ytp-ad-overlay-container",
    ".ytp-ad-overlay-slot",
    ".ytp-ad-text",
    "#player-ads",
    "#masthead-ad",
    "ytd-ad-slot-renderer",
    "ytd-in-feed-ad-layout-renderer",
    "ytd-promoted-video-renderer",
    "ytd-companion-slot-renderer",
};

// True for youtube.com and its subdomains. Site keys already have a leading
// "www." stripped (adblock::site_key), so "youtube.com" and "m.youtube.com"
// both land here.
bool is_youtube_site(std::string_view site) {
    constexpr std::string_view kHost("youtube.com");
    constexpr std::string_view kDotHost(".youtube.com");
    if (site == kHost) return true;
    return site.size() > kDotHost.size() &&
           site.compare(site.size() - kDotHost.size(), kDotHost.size(),
                        kDotHost) == 0;
}

// CSS handles ordinary selectors without DOM walks. Only inline enforcement
// for the small built-in set and explicit removal actions need an observer.
// The JSON is quoted as data, never spliced into executable source.
std::string cosmetic_filter_script(const adblock::CosmeticResources& resources,
                                   const std::vector<std::string>& watched,
                                   bool youtube) {
    std::string script("(function(){try{\nvar payload=");
    script += javascript_string(resources.json.empty() ? "{}" : resources.json);
    script += ";\nvar builtin=[";
    for (size_t i = 0; i < watched.size(); ++i) {
        if (i) script += ',';
        script += javascript_string(watched[i]);
    }
    script += "];\nvar youtube=";
    script += youtube ? "true;\n" : "false;\n";
    script += R"JS(
var previous=window.__auroraCosmeticInstalled;
if(previous){
  if(typeof previous.update==="function")previous.update(payload,builtin,youtube);
  return;
}
var ID="aurora-cosmetic-filters";
var protectedSelf="html,body,#movie_player,.html5-video-player,ytd-player,ytm-player,"+
  "video,.html5-video-container,.video-ads,"+
  "[class*='ytp-ad-skip-button'],[class*='ytp-skip-ad-button'],"+
  ".ytp-cued-thumbnail-overlay,.videoAdUiSkipButton,[id^='skip-button']";
var protectedTree="[class*='ytp-ad-skip-button'],[class*='ytp-skip-ad-button'],"+
  ".ytp-cued-thumbnail-overlay,.videoAdUiSkipButton,[id^='skip-button']";
var style=null,css="",rules=[],timer=0,roots=new Set(),observer=null;
var lastPayload=null,lastBuiltin="",isYoutube=false,globalActions=false;
var probe=document.createElement("div"),hidden=new Map();
function restoreHidden(){
  hidden.forEach(function(old,node){
    // Do not overwrite a page's own later change to its inline display.
    if(node.style.getPropertyValue("display")==="none"&&
       node.style.getPropertyPriority("display")==="important"){
      if(old.value)node.style.setProperty("display",old.value,old.priority);
      else node.style.removeProperty("display");
    }
  });
  hidden.clear();
}

function protectedNode(node){
  try{
    return isYoutube&&(node.matches(protectedSelf)||node.closest(protectedTree)||
                       node.querySelector(protectedSelf)!==null);
  }catch(e){return isYoutube;} // On YouTube an uncertain match is never hidden.
}
function safeSelector(selector){
  // :is() contains selector lists. The exclusions also cover broad/compound
  // selectors and ancestors of the player, not just exact blacklist strings.
  // Do not exclude every descendant of .video-ads: harmless overlays inside
  // that module can still be hidden without hiding the module or its controls.
  return isYoutube?":is("+selector+"):not(:is("+protectedSelf+"))"+
    ":not(:has(:is("+protectedSelf+"))):not(:is("+protectedTree+") *)":selector;
}
function ensureStyle(){
  try{
    var root=document.head||document.documentElement;
    if(!root||!css)return;
    if(!style){
      style=document.createElement("style");
      style.id=ID;
      style.type="text/css";
    }
    if(style.textContent!==css)style.textContent=css;
    if(!style.isConnected)root.appendChild(style);
  }catch(e){}
}
function applyRoot(root){
  try{
    if(!root||!root.isConnected)return;
    for(var i=0;i<rules.length;i++){
      var rule=rules[i];
      try{
        var nodes=root.querySelectorAll?root.querySelectorAll(rule.selector):[];
        var own=root.nodeType===1&&root.matches(rule.selector)?root:null;
        // Do not spread a large NodeList into function arguments: broad
        // removal rules can exceed the JS argument limit on long feeds.
        for(var j=own?-1:0;j<nodes.length;j++){
          var node=j<0?own:nodes[j];
          if(!node.isConnected||protectedNode(node))continue;
          for(var k=0;k<rule.actions.length;k++){
            var action=rule.actions[k];
            try{
              if(action.kind==="hide"){
                if(node.style.getPropertyValue("display")!=="none"||
                   node.style.getPropertyPriority("display")!=="important")
                {
                  if(!hidden.has(node))hidden.set(node,{
                    value:node.style.getPropertyValue("display"),
                    priority:node.style.getPropertyPriority("display")});
                  node.style.setProperty("display","none","important");
                }
              }else if(action.kind==="remove"){
                node.remove();
              }else{
                for(var n=0;n<action.names.length;n++){
                  var name=action.names[n];
                  if(action.kind==="attr"){
                    if(node.hasAttribute(name))node.removeAttribute(name);
                  }else if(node.classList.contains(name)){
                    node.classList.remove(name);
                  }
                }
              }
            }catch(e){} // A bad attribute/class cannot discard the other actions.
          }
        }
      }catch(e){} // Isolate invalid/unsupported selectors.
    }
  }catch(e){}
}
function flush(){
  timer=0;
  try{
    ensureStyle();
    hidden.forEach(function(old,node){if(!node.isConnected)hidden.delete(node);});
    var work=Array.from(roots);
    roots.clear();
    // A parent already covers its queued descendants; never re-walk a large
    // SPA subtree once for every nested MutationRecord.
    var queued=new Set(work);
    for(var i=0;i<work.length;i++){
      var node=work[i],parent=node.parentElement,covered=false;
      while(parent){
        if(queued.has(parent)){covered=true;break;}
        parent=parent.parentElement;
      }
      if(!covered)applyRoot(node);
    }
  }catch(e){}
}
function schedule(node){
  try{
    if(node&&node.nodeType===1&&rules.length){
      if(globalActions||roots.size>=64){
        roots.clear();
        if(document.documentElement)roots.add(document.documentElement);
      }else roots.add(node);
    }
    // Coalesce, but do not restart the timer on every mutation (starvation).
    if(!timer)timer=setTimeout(flush,50);
  }catch(e){}
}
function onMutations(records){
  try{
    var missing=css&&(!style||!style.isConnected);
    if(!rules.length){
      if(missing)schedule(null);
      else for(var n=0;n<records.length;n++){
        if(records[n].target===style){schedule(null);break;}
      }
      return;
    }
    for(var i=0;i<records.length;i++){
      var record=records[i];
      if(record.target===style){
        if(style.textContent!==css)schedule(null);
        continue; // Ignore our own stylesheet text writes.
      }
      if(record.type==="attributes"){
        schedule(record.target);
      }else{
        // Recheck the parent for :empty / sibling / relational actions.
        if(globalActions)schedule(record.target);
        for(var j=0;j<record.addedNodes.length;j++){
          var node=record.addedNodes[j];
          if(node!==style)schedule(node);
        }
      }
    }
    if(missing)schedule(null);
  }catch(e){}
}
function strings(value){
  if(typeof value==="string")return value?[value]:[];
  return Array.isArray(value)?value.filter(function(v){return typeof v==="string"&&v;}):[];
}
function entries(value,field,visit){
  // Direct UrlSpecificResources maps and pair/object arrays use the same
  // normalized path. The engine, not this code, has matched domains.
  try{
    if(Array.isArray(value)){
      for(var i=0;i<value.length;i++){
        var item=value[i];
        if(Array.isArray(item))visit(item[0],item[1]);
        else if(item&&typeof item==="object")visit(item.selector,item[field]);
      }
    }else if(value&&typeof value==="object"){
      Object.keys(value).forEach(function(selector){try{visit(selector,value[selector]);}catch(e){}});
    }
  }catch(e){}
}
function update(text,builtins,onYoutube){
  try{
    var builtinKey=JSON.stringify(builtins);
    if(text===lastPayload&&builtinKey===lastBuiltin&&onYoutube===isYoutube){
      ensureStyle();
      // Address changes can be same-document navigations with an identical
      // resource snapshot. Recheck actions without creating another observer.
      if(rules.length)schedule(document.documentElement);
      return;
    }
    var resources=JSON.parse(text);
    if(!resources||typeof resources!=="object")return;
    isYoutube=onYoutube;
    var exceptions=new Set(strings(resources.exceptions)),hide=new Set();
    var declarations=[],actions=new Map(),attrs=new Set(),seenStyles=new Set();
    globalActions=false;
    function allowed(selector){
      return typeof selector==="string"&&selector.trim()&&!exceptions.has(selector);
    }
    function addStyle(selector,value){
      try{
        if(!allowed(selector)||typeof value!=="string"||!value.trim())return;
        probe.matches(selector);
        var rule=safeSelector(selector)+"{"+value+"}";
        if(!seenStyles.has(rule)){seenStyles.add(rule);declarations.push(rule);}
      }catch(e){}
    }
    function addAction(selector,kind,names){
      try{
        if(!allowed(selector))return;
        // Validate off-DOM instead of issuing a whole-document query.
        probe.matches(selector);
        if(kind!=="hide"&&kind!=="remove"){
          names=strings(names).flatMap(function(name){return name.split(/[|\s]+/);})
            .filter(function(name){return name&&!/[\0"'<>/=]/.test(name);});
          if(!names.length)return;
        }else names=[];
        if(!actions.has(selector))actions.set(selector,[]);
        var list=actions.get(selector);
        var key=kind+"\n"+names.join("\n");
        if(!list.some(function(action){return action.key===key;}))
          list.push({kind:kind,names:names,key:key});
        // Only observe attributes that can affect these DOM-changing rules.
        if(/[.#]/.test(selector)){attrs.add("class");attrs.add("id");}
        var re=/\[\s*([-\w:]+)/g,match;
        while((match=re.exec(selector)))attrs.add(match[1].toLowerCase());
        if(kind==="attr")names.forEach(function(name){attrs.add(name.toLowerCase());});
        if(kind==="class")attrs.add("class");
        if(kind==="hide")attrs.add("style");
        // Relational selectors may change outside the added subtree. They
        // are uncommon for actions; coalesce their fallback to one DOM pass.
        if(/[+~]|:has\(|:nth-|:first-|:last-|:only-|:empty/.test(selector))
          globalActions=true;
      }catch(e){}
    }
    strings(resources.hide_selectors).concat(builtins).forEach(function(selector){
      try{if(allowed(selector))hide.add(selector);}catch(e){}
    });
    // Validate before grouping: one unsupported selector must not invalidate
    // an entire batch. Bound both selector count and rule size instead of
    // building EasyList-sized comma lists (Chromium has selector limits).
    var chunk=[],chunkBytes=0;
    function flushHide(){
      if(chunk.length)declarations.push(chunk.join(",")+"{display:none!important;}");
      chunk=[];chunkBytes=0;
    }
    hide.forEach(function(selector){
      try{
        probe.matches(selector);
        var safe=safeSelector(selector);
        if(chunk.length>=64||chunkBytes+safe.length>8192)flushHide();
        chunk.push(safe);chunkBytes+=safe.length+1;
      }catch(e){}
    });
    flushHide();
    builtins.forEach(function(selector){addAction(selector,"hide",[]);});
    entries(resources.non_hide_styles,"style",function(selector,values){
      strings(values).forEach(function(value){addStyle(selector,value);});
    });
    entries(resources.remove_attrs,"attributes",function(selector,names){addAction(selector,"attr",names);});
    entries(resources.remove_classes,"classes",function(selector,names){addAction(selector,"class",names);});

    // Some engine versions represent styles/removals as serialized actions
    // instead of separate maps. Only CSS selector steps are executable here:
    // never turn an unsupported procedural predicate into a broader CSS rule.
    var procedural=resources.procedural_actions;
    if(Array.isArray(procedural))procedural.forEach(function(value){
      try{
        if(typeof value==="string"&&exceptions.has(value))return;
        var item=typeof value==="string"?JSON.parse(value):value;
        if(!item||typeof item!=="object")return;
        var selector=item.selector,action=item.action;
        if(Array.isArray(selector)){
          if(selector.length!==1)return;
          var part=selector[0];
          if(part&&part.type==="css-selector")selector=part.arg;
          else if(part&&typeof part.CssSelector==="string")selector=part.CssSelector;
          else return;
        }
        if(typeof selector!=="string"||!action)return;
        var kind=typeof action==="string"?action:action.type;
        var arg=typeof action==="object"?action.arg:undefined;
        if(!kind&&typeof action==="object"){
          var keys=Object.keys(action);
          if(keys.length!==1)return;
          kind=keys[0];arg=action[kind];
        }
        switch(kind){
          case "style":case "Style":addStyle(selector,arg);break;
          case "remove-attr":case "RemoveAttr":addAction(selector,"attr",arg);break;
          case "remove-class":case "RemoveClass":addAction(selector,"class",arg);break;
          case "remove":case "Remove":addAction(selector,"remove",[]);break;
          case "hide":case "Hide":addStyle(selector,"display:none!important;");break;
        }
      }catch(e){}
    });

    // CSSOM validates each rule separately, so a malformed selector/style
    // cannot consume the rest of the stylesheet or inject an extra rule.
    var sheet=new CSSStyleSheet(),valid=[];
    for(var i=0;i<declarations.length;i++){
      try{
        var index=sheet.insertRule(declarations[i],sheet.cssRules.length);
        if(sheet.cssRules[index].type===CSSRule.STYLE_RULE)
          valid.push(sheet.cssRules[index].cssText);
        else sheet.deleteRule(index);
      }catch(e){}
    }
    css=valid.join("\n");
    rules=Array.from(actions,function(pair){return {selector:pair[0],actions:pair[1]};});
    if(timer){clearTimeout(timer);timer=0;}
    roots.clear();
    if(observer)observer.disconnect();
    restoreHidden();
    if(!css&&style){style.remove();style=null;}
    ensureStyle();
    if(css||rules.length){
      if(!observer)observer=new MutationObserver(onMutations);
      var options={childList:true,subtree:true};
      if(attrs.size){options.attributes=true;options.attributeFilter=Array.from(attrs);}
      // Observe the document so replacing <html>/<head> is recoverable too.
      observer.observe(document,options);
      if(rules.length)schedule(document.documentElement);
    }
    lastPayload=text;lastBuiltin=builtinKey;
  }catch(e){}
}
window.__auroraCosmeticInstalled={update:update};
update(payload,builtin,youtube);
}catch(e){}})();
)JS";
    return script;
}

// Execute expanded scriptlets through CEF, not eval(), Function() or a page
// script element. Cosmetic installation and scriptlet installation have
// separate guards: an update can add scripts without duplicating hooks.
std::string cosmetic_scriptlet_script(std::string_view scripts) {
    if (scripts.empty()) return {};
    std::string script("(function(){try{\nvar run=function(){try{\n");
    script += scripts;
    script += R"JS(
}catch(e){}};
var installed=window.__auroraCosmeticScriptlets;
if(!installed||typeof installed.has!=="function")
  installed=window.__auroraCosmeticScriptlets=new Set();
var key=Function.prototype.toString.call(run);
if(installed.has(key))return;
installed.add(key);
run();
}catch(e){}})();
)JS";
    return script;
}

// --- YouTube ad skipping -------------------------------------------------
// Four layers, cheapest first: PREVENT prunes player-response ad metadata;
// DISMISS uses the player's own controls; END makes a few spaced attempts
// against the ad's explicit progress clock; RECOVER reloads known content
// once when a break remains stuck. No video hiding, blind seeks, playback-rate
// changes or document-wide attribute observer. All hooks fail open.
// OnLoadStart is a best-effort early install; OnLoadEnd is an idempotent
// fallback, not a guarantee that no page script has already executed.
constexpr char kYoutubeAdskipScript[] = R"JS(
(function(){try{
if(window.__auroraYoutubeAdskip)return;
window.__auroraYoutubeAdskip=true;

// PREVENT: edit data, never media requests. Leave unrelated responses and
// native API error/argument semantics alone.
const AD_KEYS=new Set(["adPlacements","playerAds","adSlots","adBreakHeartbeatParams"]);
const AD_TEXT=/"(?:adPlacements|playerAds|adSlots|adBreakHeartbeatParams|adClientParams)"\s*:/;
const PLAYER_PATH=/^\/(?:youtubei\/v1\/(?:player|next|get_watch|reel\/(?:reel_item_watch|reel_watch_sequence))(?:\/|$)|get_video_info(?:\/|$))/;
const rawParse=JSON.parse,rawStringify=JSON.stringify;
function prune(root){
  let changed=false;
  try{
    const stack=[root],seen=new WeakSet();
    // Iterative and cycle-safe: deep player responses are not cut off at an
    // arbitrary recursion depth, and page getters are never invoked.
    while(stack.length){
      const node=stack.pop();
      if(!node||typeof node!=="object"||seen.has(node))continue;
      seen.add(node);
      let keys;
      try{keys=Object.keys(node);}catch(e){continue;}
      for(let i=0;i<keys.length;i++){
        const key=keys[i];
        try{
          if(AD_KEYS.has(key)){
            if(delete node[key])changed=true;
            continue;
          }
          const desc=Object.getOwnPropertyDescriptor(node,key);
          if(!desc||!("value" in desc))continue;
          const value=desc.value;
          if(key==="adClientParams"&&value&&typeof value==="object"){
            const flag=Object.getOwnPropertyDescriptor(value,"isAd");
            if(flag&&flag.value===true){value.isAd=false;changed=true;}
          }
          if(value&&typeof value==="object")stack.push(value);
        }catch(e){}
      }
    }
  }catch(e){}
  return changed;
}
function stripText(text){
  try{
    if(typeof text!=="string")return text;
    if(AD_TEXT.test(text)){
      try{
        const value=rawParse(text);
        return prune(value)?rawStringify(value):text;
      }catch(e){} // A legacy form body may contain unescaped JSON.
    }
    // The legacy endpoint encodes its JSON inside a form value. Partial XHR
    // bodies and malformed JSON are returned unchanged, not regex-renamed.
    if(text.indexOf("player_response=")!==-1){
      const form=new URLSearchParams(text),value=form.get("player_response");
      if(value&&AD_TEXT.test(value)){
        const clean=stripText(value);
        if(clean!==value){form.set("player_response",clean);return form.toString();}
      }
    }
  }catch(e){}
  return text;
}
function playerUrl(input){
  try{
    const value=typeof input==="string"?input:input&&(input.url||input.href);
    if(typeof value!=="string")return false;
    const url=new URL(value,location.href);
    return (/(^|\.)youtube\.com$/.test(url.hostname)||
            url.hostname==="youtubei.googleapis.com")&&PLAYER_PATH.test(url.pathname);
  }catch(e){return false;}
}
try{
  JSON.parse=function(text){
    // Preserve revivers, coercion, this and SyntaxError from native parsing.
    const value=Reflect.apply(rawParse,this,arguments);
    try{if(typeof text==="string"&&AD_TEXT.test(text))prune(value);}catch(e){}
    return value;
  };
}catch(e){}

const patchedResponses=new WeakSet();
function patchResponse(response){
  try{
    if(!response||patchedResponses.has(response))return response;
    const text=response.text,json=response.json,clone=response.clone;
    Object.defineProperties(response,{
      text:{configurable:true,writable:true,value:function(){
        return Reflect.apply(text,this,arguments).then(stripText);
      }},
      json:{configurable:true,writable:true,value:function(){
        return Reflect.apply(json,this,arguments).then(function(value){
          try{prune(value);}catch(e){}
          return value;
        });
      }},
      clone:{configurable:true,writable:true,value:function(){
        return patchResponse(Reflect.apply(clone,this,arguments));
      }}
    });
    patchedResponses.add(response);
  }catch(e){}
  // No replacement Response: status, URL, headers, bodyUsed and streams keep
  // their native behavior. clone() keeps the same text()/json() protection.
  return response;
}
try{
  const rawFetch=window.fetch;
  if(typeof rawFetch==="function")window.fetch=function(input){
    const pending=Reflect.apply(rawFetch,this,arguments);
    try{if(playerUrl(input))return pending.then(patchResponse);}catch(e){}
    return pending;
  };
}catch(e){}

try{
  const proto=XMLHttpRequest.prototype,rawOpen=proto.open;
  const text=Object.getOwnPropertyDescriptor(proto,"responseText");
  const response=Object.getOwnPropertyDescriptor(proto,"response");
  const requests=new WeakMap();
  function clean(xhr,value){
    try{
      const state=requests.get(xhr);
      if(!state||xhr.readyState!==4)return value;
      if(typeof value==="string"){
        if(state.text!==value){state.text=value;state.clean=stripText(value);}
        return state.clean;
      }
      if(xhr.responseType==="json"&&value&&state.object!==value){
        prune(value);state.object=value;
      }
    }catch(e){}
    return value;
  }
  // Install accessors once, not new closures on every request. Every open()
  // resets eligibility and caches, including reuse for an unrelated URL.
  proto.open=function(method,url){
    const result=Reflect.apply(rawOpen,this,arguments);
    try{
      if(playerUrl(url))requests.set(this,{text:null,clean:null,object:null});
      else requests.delete(this);
    }catch(e){}
    return result;
  };
  if(text&&text.get&&text.configurable)Object.defineProperty(proto,"responseText",{
    configurable:text.configurable,enumerable:text.enumerable,
    get:function(){return clean(this,text.get.call(this));}
  });
  if(response&&response.get&&response.configurable)Object.defineProperty(proto,"response",{
    configurable:response.configurable,enumerable:response.enumerable,
    get:function(){return clean(this,response.get.call(this));}
  });
}catch(e){}

function pruneInitial(){
  try{prune(window.ytInitialPlayerResponse);}catch(e){}
}
try{
  const desc=Object.getOwnPropertyDescriptor(window,"ytInitialPlayerResponse");
  pruneInitial();
  // Do not replace someone else's accessors or turn a readonly global into a
  // writable property. The common cold-load data-property path is hookable.
  if(!desc||(desc.configurable&&"value" in desc&&desc.writable)){
    let initial=window.ytInitialPlayerResponse;
    Object.defineProperty(window,"ytInitialPlayerResponse",{
      configurable:true,enumerable:desc?desc.enumerable:true,
      get:function(){return initial;},
      set:function(value){try{prune(value);}catch(e){}initial=value;}
    });
  }
}catch(e){}

// Player guard: never hide the video or the controls that can end an ad.
// The poster and .video-ads are protected by the cosmetic selector exclusions;
// forcing a poster to display during playback would itself cover the video.
const STYLE_ID="aurora-youtube-player-guard";
const STYLE_CSS="#movie_player video,video.html5-main-video,.html5-video-container"+
  "{display:block!important;visibility:visible!important;opacity:1!important;}"+
  "#movie_player [class*='ytp-ad-skip-button'],#movie_player [class*='ytp-skip-ad-button'],"+
  "[class*='ytp-skip-ad-button'],[class*='ytp-ad-skip-button'],"+
  ".videoAdUiSkipButton,button[id^='skip-button']"+
  "{display:inline-block!important;visibility:visible!important;opacity:1!important;pointer-events:auto!important;}";
let guard=null;
function ensureVisible(){
  try{
    const root=document.documentElement;
    if(!root)return;
    if(!guard){
      guard=document.getElementById(STYLE_ID)||document.createElement("style");
      guard.id=STYLE_ID;guard.type="text/css";
    }
    if(guard.textContent!==STYLE_CSS)guard.textContent=STYLE_CSS;
    if(!guard.isConnected)root.appendChild(guard);
  }catch(e){}
}

// DISMISS / END / RECOVER: one cached player, one coalesced scheduler.
const PLAYER="#movie_player,.html5-video-player,ytd-player,ytm-player";
const AD_PLAYER="#movie_player.ad-showing,.html5-video-player.ad-showing,"+
  "ytd-player.ad-showing,ytm-player.ad-showing";
const SKIP=[
  ".ytp-skip-ad-button",".ytp-ad-skip-button-modern",".ytp-ad-skip-button",
  ".ytp-ad-skip-button-text",".ytp-skip-ad-button__text",".videoAdUiSkipButton",
  "[class*='ytp-ad-skip-button']:is(button,[role='button'])",
  ".ytp-ad-skip-button-container button",".ytp-ad-skip-button-slot button",
  ".ytp-skip-ad-button-container button","button[id^='skip-button']",
  "button[aria-label^='Skip ad']","button[title^='Skip ad']"
].join(",");
const IDLE_MS=1500,AD_MS=350,HIDDEN_MS=5000,MIN_RUN_MS=100;
let player=null,playerWrapper=null,video=null,playerObserver=null,slot=null;
let timer=0,due=Infinity,lastRun=-Infinity,refresh=true;
let route="",contentId="",contentTime=0,contentDuration=0;
let recoveryAttempts=0,lastReload=-Infinity,lastClick=-Infinity,lastTrack=-Infinity;
const clicked=new WeakMap();

function adClass(node){
  try{
    return !!node&&(node.classList.contains("ad-showing")||
      (playerWrapper&&playerWrapper.classList.contains("ad-showing")));
  }catch(e){return false;}
}
function isAd(node){
  if(adClass(node))return true;
  try{
    // SSAP breaks do not always expose the class; use an explicit ad signal,
    // never infer an ad from a short/unknown media duration.
    const stats=node&&typeof node.getStatsForNerds==="function"?node.getStatsForNerds():null;
    return !!stats&&typeof stats.debug_info==="string"&&/^SSAP,\s*AD\b/.test(stats.debug_info);
  }catch(e){return false;}
}
function syncRoute(){
  try{
    if(route===location.href)return;
    route=location.href;
    const url=new URL(route),path=/^\/(?:shorts|embed|live)\/([^/?]+)/.exec(url.pathname);
    const id=url.searchParams.get("v")||(path&&path[1])||"";
    // A previous video's position/ad ID must never leak into a SPA recovery.
    if(id!==contentId){
      contentId=/^[\w-]{11}$/.test(id)?id:"";
      contentTime=0;contentDuration=0;recoveryAttempts=0;slot=null;
    }
    refresh=true;
  }catch(e){}
}
function trackContent(){
  try{
    const now=performance.now();
    if(!player||!video||slot||now-lastTrack<500||isAd(player))return;
    lastTrack=now;
    const data=typeof player.getVideoData==="function"?player.getVideoData():null;
    const id=data&&data.video_id;
    if(contentId&&id&&id!==contentId)return; // getVideoData may describe the ad.
    if(!contentId&&typeof id==="string"&&/^[\w-]{11}$/.test(id))contentId=id;
    if(Number.isFinite(video.currentTime)&&video.currentTime>=0)contentTime=video.currentTime;
    if(Number.isFinite(video.duration)&&video.duration>0)contentDuration=video.duration;
  }catch(e){}
}
function videoChanged(){
  try{
    const next=player&&player.querySelector("video");
    if(next===video)return;
    if(video)video.removeEventListener("timeupdate",trackContent);
    video=next;
    if(video)video.addEventListener("timeupdate",trackContent,{passive:true});
  }catch(e){}
}
function observePlayer(records){
  try{
    for(let i=0;i<records.length;i++){
      const record=records[i];
      if(record.type==="childList"){
        videoChanged();
        if(slot||adClass(player)){schedule(40);return;}
      }else if(record.target===player||record.target===playerWrapper||
               slot||record.target.matches(SKIP)){
        schedule(40);return;
      }
    }
  }catch(e){}
}
function findPlayer(){
  try{
    if(!refresh&&player&&player.isConnected)return;
    refresh=false;
    const active=document.querySelector(AD_PLAYER);
    const next=(active&&(active.querySelector("#movie_player,.html5-video-player")||active))||
               document.getElementById("movie_player")||
               document.querySelector(".html5-video-player")||document.querySelector("ytd-player,ytm-player");
    const wrapper=next&&next.closest("ytd-player,ytm-player");
    if(next!==player||wrapper!==playerWrapper){
      if(playerObserver)playerObserver.disconnect();
      if(next!==player)slot=null;
      player=next;playerWrapper=wrapper;
      if(player){
        if(!playerObserver)playerObserver=new MutationObserver(observePlayer);
        // Class changes are observed ONLY inside the player, not across the
        // entire YouTube document. Ignore style/progress-bar animation churn.
        playerObserver.observe(player,{childList:true,subtree:true,attributes:true,
          attributeFilter:["class","disabled","aria-disabled","hidden"]});
        if(playerWrapper&&playerWrapper!==player)
          playerObserver.observe(playerWrapper,{attributes:true,attributeFilter:["class"]});
      }
    }
    videoChanged();
  }catch(e){}
}
function clickSkip(now){
  try{
    if(now-lastClick<700)return false;
    // One selector query per scope, one click per run, with a per-button
    // cooldown. A persistent/nonfunctional button cannot starve END/RECOVER.
    const scopes=[player,document];
    for(let i=0;i<scopes.length;i++){
      if(!scopes[i])continue;
      const nodes=scopes[i].querySelectorAll(SKIP);
      for(let j=0;j<nodes.length;j++){
        try{
          const candidate=nodes[j];
          const button=candidate.closest("button,[role='button']")||candidate;
          const owner=button.closest(PLAYER);
          if(!button.isConnected||(owner&&owner!==player&&owner!==playerWrapper)||button.disabled||
             button.closest("[disabled],[aria-disabled='true']")||
             now-(clicked.get(button)??-Infinity)<1000)continue;
          if(typeof button.click!=="function")continue;
          // Hidden-but-enabled controls are deliberately clickable: CSS must
          // not take away YouTube's own exit, and no forced layout is needed.
          clicked.set(button,now);
          lastClick=now;
          button.click();
          return true;
        }catch(e){}
      }
    }
  }catch(e){}
  return false;
}
function seekAd(now){
  try{
    if(!slot||slot.seeks>=3||now-slot.started<400||now-slot.lastSeek<1200||
       (video&&video.seeking)||typeof player.getProgressState!=="function"||
       typeof player.seekTo!=="function")return false;
    const state=player.getProgressState(),end=state&&state.duration;
    // Only the AD's explicit progress clock is usable. Never fall back to
    // getDuration(), video.duration or seekableEnd: those can be content/live
    // clocks. An implausible/unchanged content clock is safer to leave alone.
    if(!Number.isFinite(end)||end<=0||end>600||
       (contentDuration>0&&Math.abs(end-contentDuration)<0.1))return false;
    slot.lastSeek=now;slot.seeks++;
    player.seekTo(end,true);
    return true;
  }catch(e){return false;}
}
function recover(now){
  try{
    if(!slot||now-slot.started<5500||recoveryAttempts>=1||now-lastReload<30000||
       !contentId||!video||video.paused||typeof player.loadVideoById!=="function")return;
    const data=typeof player.getVideoData==="function"?player.getVideoData():null;
    if(data&&(data.isLive||data.isLiveContent))return;
    // Mark BEFORE calling (the API can throw or synchronously fire events).
    lastReload=now;recoveryAttempts++;
    player.loadVideoById(contentId,Math.max(0,contentTime-0.5));
  }catch(e){}
}
function tick(now){
  try{
    syncRoute();
    ensureVisible();
    // Periodic discovery is only an idle fallback for player replacements
    // that did not emit one of YouTube's normal navigation events.
    if(!slot)refresh=true;
    findPlayer();
    if(!player||!isAd(player)){slot=null;trackContent();return;}
    if(!slot)slot={started:now,lastSeek:-Infinity,seeks:0};
    if(clickSkip(now)||now-lastClick<200)return; // DISMISS: let the native handler settle.
    if(seekAd(now))return;    // END: bounded and spaced, never playbackRate.
    recover(now);            // RECOVER: once per content video, with cooldown.
  }catch(e){}
}
function run(){
  timer=0;due=Infinity;
  let delay=IDLE_MS;
  try{
    const now=performance.now();
    if(now-lastRun<MIN_RUN_MS){schedule(MIN_RUN_MS-(now-lastRun));return;}
    lastRun=now;
    tick(now);
    delay=slot?AD_MS:document.hidden?HIDDEN_MS:IDLE_MS;
  }catch(e){}
  // No permanent 300 ms poll: idle/hidden pages mostly sleep; an observed
  // class/control change preempts this timer immediately.
  schedule(delay);
}
function schedule(delay){
  try{
    const next=performance.now()+delay;
    if(timer&&due<=next)return;
    if(timer)clearTimeout(timer);
    due=next;timer=setTimeout(run,delay);
  }catch(e){}
}
function navigation(){
  try{syncRoute();refresh=true;pruneInitial();schedule(0);}catch(e){}
}
try{
  // Discovery observes additions/removals, NOT page-wide attributes. The
  // cached player's isConnected flag and YouTube navigation events avoid a
  // selector scan on every DOM mutation.
  const discovery=new MutationObserver(function(){
    try{
      if(!guard||!guard.isConnected)ensureVisible();
      if(!player||!player.isConnected){refresh=true;schedule(40);}
    }catch(e){}
  });
  discovery.observe(document,{childList:true,subtree:true});
  document.addEventListener("yt-navigate-start",navigation,true);
  document.addEventListener("yt-navigate-finish",navigation,true);
  document.addEventListener("yt-player-updated",navigation,true);
  document.addEventListener("DOMContentLoaded",navigation,{once:true});
  document.addEventListener("visibilitychange",navigation,true);
  window.addEventListener("pageshow",navigation,true);
  window.addEventListener("pagehide",function(event){
    try{if(!event.persisted&&timer){clearTimeout(timer);timer=0;due=Infinity;}}catch(e){}
  },true);
}catch(e){}
navigation();
}catch(e){}})();
)JS";

// Aurora stores paths in std::string as UTF-8, while std::filesystem and CEF
// use UTF-16 natively on Windows. Avoid routing a native Windows path through
// path::string(), whose narrow encoding is locale-dependent.
std::filesystem::path path_from_utf8(const std::string& path) {
#if defined(OS_WIN)
    return std::filesystem::path(CefString(path).ToWString());
#else
    return std::filesystem::path(path);
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
#if defined(OS_WIN)
    return CefString(path.wstring()).ToString();
#else
    return path.string();
#endif
}

CefString path_to_cef(const std::filesystem::path& path) {
#if defined(OS_WIN)
    return CefString(path.wstring());
#else
    return CefString(path.string());
#endif
}

#if defined(OS_WIN)
bool copy_text_to_clipboard(CefWindowHandle owner, const CefString& value) {
    const std::wstring text = value.ToWString();
    if (!OpenClipboard(owner)) return false;

    bool copied = false;
    if (EmptyClipboard()) {
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            auto* output = static_cast<wchar_t*>(GlobalLock(memory));
            if (output) {
                std::copy(text.begin(), text.end(), output);
                output[text.size()] = L'\0';
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory)) {
                    copied = true;
                    memory = nullptr;  // The clipboard now owns the allocation.
                }
            }
            if (memory) GlobalFree(memory);
        }
    }
    CloseClipboard();
    return copied;
}

bool copy_image_to_clipboard(CefWindowHandle owner, CefRefPtr<CefImage> image) {
    if (!image || image->IsEmpty()) return false;

    int width = 0;
    int height = 0;
    auto bitmap = image->GetAsBitmap(
        1.0F, CEF_COLOR_TYPE_BGRA_8888, CEF_ALPHA_TYPE_PREMULTIPLIED,
        width, height);
    if (!bitmap || width <= 0 || height <= 0) return false;

    const size_t row_bytes = static_cast<size_t>(width) * 4;
    if (row_bytes / 4 != static_cast<size_t>(width) ||
        static_cast<size_t>(height) >
            (std::numeric_limits<size_t>::max() - sizeof(BITMAPINFOHEADER)) /
                row_bytes) {
        return false;
    }
    const size_t pixel_bytes = row_bytes * static_cast<size_t>(height);
    if (pixel_bytes > std::numeric_limits<DWORD>::max() ||
        bitmap->GetSize() < pixel_bytes) {
        return false;
    }

    std::vector<unsigned char> pixels(pixel_bytes);
    if (bitmap->GetData(pixels.data(), pixel_bytes, 0) != pixel_bytes)
        return false;

    const SIZE_T allocation_size = sizeof(BITMAPINFOHEADER) + pixel_bytes;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, allocation_size);
    if (!memory) return false;

    auto* output = static_cast<unsigned char*>(GlobalLock(memory));
    if (!output) {
        GlobalFree(memory);
        return false;
    }

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = width;
    header.biHeight = height;  // CF_DIB scanlines are stored bottom-up.
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(pixel_bytes);
    std::copy_n(reinterpret_cast<const unsigned char*>(&header), sizeof(header),
                output);
    auto* dib_pixels = output + sizeof(header);
    for (int y = 0; y < height; ++y) {
        const auto* source =
            pixels.data() + static_cast<size_t>(height - 1 - y) * row_bytes;
        std::copy_n(source, row_bytes,
                    dib_pixels + static_cast<size_t>(y) * row_bytes);
    }
    GlobalUnlock(memory);

    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    bool copied = false;
    if (EmptyClipboard() && SetClipboardData(CF_DIB, memory)) {
        copied = true;
        memory = nullptr;  // The clipboard now owns the allocation.
    }
    CloseClipboard();
    if (memory) GlobalFree(memory);
    return copied;
}
#endif

class CopyImageCallback : public CefDownloadImageCallback {
public:
    // `owner` is the native window used for the Win32 clipboard; `tab` and
    // `to_host` carry the image to the host's clipboard everywhere else.
    CopyImageCallback(CefWindowHandle owner, TabId tab,
                      std::function<void(TabId, webview::WebViewController::ContextActionRequest)> to_host)
        : owner_(owner), tab_(tab), to_host_(std::move(to_host)) {}

    void OnDownloadImageFinished(const CefString& image_url,
                                 int http_status_code,
                                 CefRefPtr<CefImage> image) override {
        const bool ok = image.get() &&
                        (http_status_code == 0 ||
                         (http_status_code >= 200 && http_status_code < 400));
#if defined(OS_WIN)
        (void)tab_;
        (void)to_host_;
        if (ok && copy_image_to_clipboard(owner_, image)) return;
#else
        (void)owner_;
        if (ok && to_host_) {
            // CEF has no clipboard API, so hand the host a PNG and let it
            // put the image on the system clipboard.
            int w = 0, h = 0;
            CefRefPtr<CefBinaryValue> png =
                image->GetAsPNG(1.0f, true, w, h);
            if (png.get() && png->GetSize() > 0) {
                webview::WebViewController::ContextActionRequest req;
                req.action = webview::WebViewController::ContextAction::CopyImage;
                req.data.resize(png->GetSize());
                png->GetData(req.data.data(), req.data.size(), 0);
                to_host_(tab_, std::move(req));
                return;
            }
        }
#endif
        AURORA_LOG(util::LogLevel::Warn, "cef")
            << "could not copy context-menu image from "
            << image_url.ToString();
    }

private:
    CefWindowHandle owner_;
    TabId tab_;
    std::function<void(TabId, webview::WebViewController::ContextActionRequest)> to_host_;
    IMPLEMENT_REFCOUNTING(CopyImageCallback);
};

// Per-tab CEF client. Owns the handlers for load state, life span, context
// menus, and JS-to-native messages. It translates CEF callbacks into the
// PageInfo struct the core expects.
class TabClient : public CefClient,
                  public CefLifeSpanHandler,
                  public CefLoadHandler,
                  public CefDisplayHandler,
                  public CefContextMenuHandler,
                  public CefDownloadHandler,
                  public CefAudioHandler,
                  public CefKeyboardHandler,
                  public CefPermissionHandler,
                  public CefRequestHandler,
                  public CefResourceRequestHandler {
public:
    using PageInfoEmitter =
        std::function<void(TabId, const webview::PageInfo&)>;
    using CloseEmitter = std::function<void(TabId)>;
    // Invoked from OnBeforePopup with the URL the page wants to open in a new
    // window. The host opens it as a new in-app tab; we return true from
    // OnBeforePopup to cancel CEF's own popup window.
    using NewTabEmitter = std::function<bool(std::string)>;
    // Surfaces download lifecycle events (start/progress/complete) to the host
    // so it can record them in DownloadService and refresh the Downloads page.
    using DownloadEmitter =
        std::function<void(const webview::DownloadEvent&)>;
    // Web content entered/left fullscreen. Alloy-style browsers leave the
    // window transition to the client, which is what we want: Aurora hides its
    // own chrome and takes the window fullscreen itself.
    using FullscreenEmitter = std::function<void(TabId, bool)>;
    // Synchronous "does the host's chrome claim this key as an accelerator?"
    // query, asked from OnPreKeyEvent while the native child window owns the
    // keyboard. True = the key is consumed (the page never sees it); the host
    // performs the action itself, posted to the Qt event loop.
    using KeyEmitter =
        std::function<bool(TabId, const webview::EngineKeyEvent&)>;
    // A .crx download finished: hand the file to the host for installation
    // instead of letting it sit in the Downloads folder.
    using ExtensionEmitter =
        std::function<void(TabId, const webview::ExtensionInstallRequest&)>;
    // A page asked for a capability (mic/camera/notification). The request
    // carries an exactly-once, default-deny responder; the host answers it
    // from a prompt (or immediately from a remembered decision).
    // Right-click actions the host performs (clipboard, search, inspect...).
    using ContextActionEmitter =
        std::function<void(TabId, webview::WebViewController::ContextActionRequest)>;
    using PermissionEmitter =
        std::function<void(webview::PermissionRequest)>;
    // Supplies a document-start script only for synthetic extension popups,
    // and — with a non-empty second argument — services one bridge request
    // sent by an already-running popup. Keeping the lookup lazy means the
    // target browser identifier and URL are read at navigation time rather
    // than guessed by the Qt layer.
    //
    // Two calls, one function object, deliberately: the popup bridge is the
    // only channel a synthetic popup has to the browser process, and routing
    // it through the same provider avoids threading a second emitter through
    // every TabClient (tabs that are not extension popups get an empty string
    // back for both calls and pay nothing).
    //   provider(id, "")      -> the document-start shim, or "" for a normal tab
    //   provider(id, request) -> JavaScript to run back in the popup as the
    //                            reply, or "" when there is nothing to answer
    using PopupShimProvider =
        std::function<std::string(TabId, std::string_view)>;

    // Prefix a popup uses to address the browser process over console output.
    // Alloy-style browsers expose no extension message pipe, and CEF gives the
    // browser process no "evaluate and return" entry point, so a tagged console
    // record is the one channel available without a render-process handler.
    // Only browsers registered as extension popups are listened to.
    static constexpr const char* kBridgeTag = "AURORA_EXT_BRIDGE:";

    // Reply half of the same channel, printed by the OWNER tab after it ran a
    // delivered call: "AURORA_EXT_REPLY:<bridge id>|<percent-encoded JSON>".
    // ExecuteJavaScript is fire-and-forget, so this record is the only way the
    // value a content-script listener returned (sendResponse or a synchronous
    // return) can travel back to the popup that is awaiting it.
    static constexpr const char* kReplyTag = "AURORA_EXT_REPLY:";

    TabClient(TabId id, PageInfoEmitter emit, CloseEmitter close_emit,
              NewTabEmitter new_tab_emit, std::string downloads_dir,
              AskWhereProvider ask_where_provider,
              DownloadsDirProvider downloads_dir_provider,
              SavePathChooser save_path_chooser, DownloadEmitter download_emit,
              FullscreenEmitter fullscreen_emit,
              KeyEmitter key_emit, ExtensionEmitter extension_emit,
              PermissionEmitter permission_emit,
              ContextActionEmitter context_action_emit,
              PopupShimProvider popup_shim_provider, bool private_window)
        : id_(id), emit_(std::move(emit)), close_emit_(std::move(close_emit)),
          new_tab_emit_(std::move(new_tab_emit)),
          downloads_dir_(std::move(downloads_dir)),
          ask_where_provider_(std::move(ask_where_provider)),
          downloads_dir_provider_(std::move(downloads_dir_provider)),
          save_path_chooser_(std::move(save_path_chooser)),
          download_emit_(std::move(download_emit)),
          fullscreen_emit_(std::move(fullscreen_emit)),
          key_emit_(std::move(key_emit)),
          extension_emit_(std::move(extension_emit)),
          permission_emit_(std::move(permission_emit)),
          context_action_emit_(std::move(context_action_emit)),
          popup_shim_provider_(std::move(popup_shim_provider)),
          private_window_(private_window) {}

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
        return this;
    }
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override {
        return this;
    }
    CefRefPtr<CefAudioHandler> GetAudioHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
        return this;
    }
    // GetResourceRequestHandler lives on CefRequestHandler (not CefClient), so
    // the client has to hand CEF a request handler first for the resource hook
    // below to ever be consulted.
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    bool OnBeforeBrowse(CefRefPtr<CefBrowser> /*browser*/,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> /*request*/,
                        bool /*user_gesture*/, bool /*is_redirect*/) override {
        // CEF calls this on the UI thread, so reading Settings is safe.
        refresh_privacy_preferences();
        if (frame && frame->IsMain()) ++navigation_generation_;
        return false; // TLS/navigation policy is never bypassed here.
    }

    bool OnCertificateError(CefRefPtr<CefBrowser> browser,
                            cef_errorcode_t /*cert_error*/,
                            const CefString& request_url,
                            CefRefPtr<CefSSLInfo> ssl_info,
                            CefRefPtr<CefCallback> /*callback*/) override {
        refresh_privacy_preferences();
        if (ssl_info)
            audit_certificate(browser, request_url,
                              ssl_info->GetX509Certificate(), false);
        // Do not accept an invalid TLS certificate or seed one as trusted.
        // Aurora currently has no explicit certificate-accept UI; any future
        // one must call observe(..., explicit_user_accept=true) deliberately.
        return false;
    }

    void OnResourceRedirect(CefRefPtr<CefBrowser> /*browser*/,
                            CefRefPtr<CefFrame> /*frame*/,
                            CefRefPtr<CefRequest> request,
                            CefRefPtr<CefResponse> /*response*/,
                            CefString& new_url) override {
        if (!request) return;
        const auto type = request->GetResourceType();
        if (type == RT_MAIN_FRAME || type == RT_SUB_FRAME)
            new_url = core::upgrade_http_url(new_url.ToString(), g_https_only.load());
    }
    // Network filtering: the same client doubles as the resource request
    // handler, so every request this tab makes passes through the ad-block
    // check below before the network stack sees it.
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
        CefRefPtr<CefBrowser> /*browser*/,
        CefRefPtr<CefFrame> /*frame*/,
        CefRefPtr<CefRequest> /*request*/,
        bool /*is_navigation*/,
        bool /*is_download*/,
        const CefString& /*request_initiator*/,
        bool& /*disable_default_handling*/) override {
        // DIAGNOSTIC (temporary): confirms CEF actually consults the request
        // handler. If this never appears in aurora-debug.log, CEF never
        // reaches the resource hook at all.
        static std::atomic<int> s_rrh{0};
        if (int n = ++s_rrh; n <= 10) {
            AURORA_LOG(util::LogLevel::Trace, "adblock")
                << "GetResourceRequestHandler #" << n << " consulted";
        }
        return this;
    }

    // Third-party cookie blocking, HTTP layer. Runs on the IO thread for
    // every request before it is sent. "Third party" is judged against the
    // TOP-LEVEL page, the site the user sees in the omnibox — the same rule
    // Chrome applies — so an iframe of the page's own site keeps its cookies
    // and a tracker embedded anywhere does not. The filter covers BOTH
    // directions (Cookie sent, Set-Cookie saved; see ThirdPartyCookieFilter).
    // The setting is an atomic refreshed from the UI thread every 250 ms
    // (refresh_privacy_preferences), so a flip in aurora://settings reaches
    // every existing browser and request context — normal and private —
    // without a reload: this client serves all of them.
    //
    // Not covered here, by design of the CEF API (cef_resource_request_handler.h:
    // the filter applies to "resource requests" only):
    //   - document.cookie / CookieStore reads and writes from a third-party
    //     iframe's script, which never pass through a resource request;
    //   - requests without a browser (service workers, CefURLRequest).
    // Both are left to Chromium's own cookie policy, which
    // refresh_privacy_preferences() drives through the
    // "profile.cookie_controls_mode" preference.
    // The main-frame navigation itself is first-party by definition.
    CefRefPtr<CefCookieAccessFilter> GetCookieAccessFilter(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> /*frame*/,
        CefRefPtr<CefRequest> request) override {
        if (!g_block_3p_cookies.load() || !browser || !request) return nullptr;
        if (request->GetResourceType() == RT_MAIN_FRAME) return nullptr;
        const auto main = browser->GetMainFrame();
        if (!main) return nullptr;
        std::string top_site = url_cookie_site(main->GetURL());
        // A top-level page without a web site (aurora pages, about:blank,
        // file:) gives no first party to protect; do not guess.
        if (top_site.empty()) return nullptr;
        return new ThirdPartyCookieFilter(std::move(top_site));
    }

    // The ad-block hot path. Runs on CEF's IO thread (not the UI thread), so
    // it reads privacy atomics, takes g_adblock's mutex and touches no Qt/
    // SQLite objects — the shield badge update is posted to the UI thread.
    // |callback| is only for asynchronous continuation (RV_CONTINUE_ASYNC);
    // every path here answers synchronously, so it stays unused.
    CefResourceRequestHandler::ReturnValue OnBeforeResourceLoad(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request,
        CefRefPtr<CefCallback> /*callback*/) override {
        // DIAGNOSTIC (temporary): confirms the hook fires and shows the
        // request URL. If this never appears, CEF isn't calling
        // OnBeforeResourceLoad despite the wiring.
        static std::atomic<int> s_obrl{0};
        const int n = ++s_obrl;
        if (request && n <= 100) {
            AURORA_LOG(util::LogLevel::Trace, "adblock")
                << "OnBeforeResourceLoad #" << n << " entered url="
                << request->GetURL().ToString();
        }
        if (!request) return RV_CONTINUE;
        // Privacy applies BEFORE the top-level/adblock/exception early exits,
        // and is independent of Aurora Shields and its per-site exceptions.
        // CEF excludes Referer from HeaderMap: SetReferrer is the supported
        // way to rewrite it. Preserve Chromium's policy (including no-referrer
        // and downgrade suppression), and never synthesize an absent header.
        if (g_referer_trim.load()) {
            const auto referrer = request->GetReferrerURL();
            if (!referrer.empty()) {
                request->SetReferrer(CefString(web_origin(referrer)),
                                     request->GetReferrerPolicy());
            }
        }
        // Do Not Track: one header on every request, documents and
        // sub-resources alike (what Chrome's own toggle does). Overwrite so a
        // page's fetch() cannot send a contradicting value under our name.
        if (g_send_dnt.load()) {
            request->SetHeaderByName("DNT", "1", /*overwrite=*/true);
        }
        const cef_resource_type_t type = request->GetResourceType();
        if (type == RT_MAIN_FRAME || type == RT_SUB_FRAME) {
            const auto original = request->GetURL().ToString();
            const auto upgraded = core::upgrade_http_url(original, g_https_only.load());
            if (upgraded != original) request->SetURL(CefString(upgraded));
        }
        if (!browser) return RV_CONTINUE;
        // Never touch the top-level document: cancelling the page itself (not
        // a sub-resource) breaks navigation, and the blocker's "document"
        // category exists for exception rules, not for blocking.
        if (type == RT_MAIN_FRAME) return RV_CONTINUE;

        // The per-site exception is decided by the TOP-LEVEL page the user is
        // looking at (the site the shield shows), never by whichever subframe
        // happens to issue the request — shields down on example.com must
        // also exempt resources pulled in by example.com's ad iframes.
        std::string page_url;
        if (browser->GetMainFrame()) {
            page_url = browser->GetMainFrame()->GetURL().ToString();
        }
        if (page_url.empty() && frame) {
            page_url = frame->GetURL().ToString();
        }
        if (page_url.empty()) return RV_CONTINUE;
        // Twitch uses XHR/fetch for HLS manifests and segments. They can
        // otherwise match a broad third-party filter and leave a black player
        // even though its document and JavaScript loaded successfully.
        if (is_twitch_media_resource(page_url, request->GetURL().ToString()))
            return RV_CONTINUE;
        const std::string site = adblock::site_key(page_url);
        if (site.empty()) return RV_CONTINUE;

        // The engine, though, gets the actual initiator: a rule's specificity
        // (third-party checks, $domain=) depends on the real source URL.
        std::string source = page_url;
        if (frame) {
            const std::string frame_url = frame->GetURL().ToString();
            if (!frame_url.empty()) source = frame_url;
        }

        bool block = false;
        uint64_t total = 0;
        std::function<void(TabId, uint64_t)> notify;
        {
            std::lock_guard<std::mutex> lock(g_adblock.mutex);
            if (g_adblock.enabled && g_adblock.engine &&
                g_adblock.engine->available() &&
                g_adblock.exceptions.find(site) == g_adblock.exceptions.end()) {
                block = g_adblock.engine->should_block(
                    request->GetURL().ToString(), source, adblock_type_for(type));
                if (n <= 100) {
                    AURORA_LOG(util::LogLevel::Trace, "adblock")
                        << "OnBeforeResourceLoad #" << n
                        << " should_block=" << (block ? 1 : 0)
                        << " type=" << adblock_type_for(type);
                }
            }
            if (block) {
                total = ++g_adblock.blocked[id_];
                notify = g_adblock.notify;
            }
        }
        if (block && notify) {
            const TabId tab = id_;
            // The callback touches Qt widgets; it must run on the UI thread.
            CefPostTask(TID_UI, new AdblockTask([notify, tab, total] {
                notify(tab, total);
            }));
        }
        return block ? RV_CANCEL : RV_CONTINUE;
    }

    // Installs the YouTube ad-skipping scriptlet (kYoutubeAdskipScript) on the
    // main frame. Called from both load hooks: OnLoadStart attempts an early
    // install so hooks and observers can intercept initial player data;
    // OnLoadEnd repeats it for the case where
    // the frame was not yet scriptable at start. The scriptlet's own guard
    // makes the second call a no-op.
    //
    // Gated exactly like the cosmetic injection in OnLoadEnd: youtube.com
    // only, blocking enabled, an engine available, and shields not down for
    // the site — all read under g_adblock's mutex.
    void inject_youtube_adskip(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame) {
        if (!frame || !frame->IsMain()) return;
        std::string page_url = frame->GetURL().ToString();
        if (page_url.empty() && browser && browser->GetMainFrame()) {
            page_url = browser->GetMainFrame()->GetURL().ToString();
        }
        if (!page_url.starts_with("https://") && !page_url.starts_with("http://")) return;
        const std::string site = adblock::site_key(page_url);
        if (site.empty() || !is_youtube_site(site)) return;
        {
            std::lock_guard<std::mutex> lock(g_adblock.mutex);
            if (!g_adblock.enabled || !g_adblock.engine ||
                !g_adblock.engine->available() ||
                g_adblock.exceptions.find(site) != g_adblock.exceptions.end()) {
                return;
            }
        }
        AURORA_LOG(util::LogLevel::Trace, "adblock")
            << "youtube ad-skip scriptlet injected for " << site;
        frame->ExecuteJavaScript(CefString(std::string(kYoutubeAdskipScript)),
                                 CefString("aurora://youtube-adskip.js"), 1);
    }

    // HTTP(S) subframes get their own domain-matched resources, never the
    // parent's selectors. A shields-down top-level site also exempts its
    // embeds. Opaque/about:blank, extension and aurora:// frames are skipped.
    void inject_cosmetic_filters(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame) {
        if (!browser || !frame || !frame->IsValid()) return;
        const auto main = browser->GetMainFrame();
        if (!main) return;
        const std::string page_url = frame->GetURL().ToString();
        const std::string top_url = main->GetURL().ToString();
        const auto web_url = [](const std::string& url) {
            return url.starts_with("https://") || url.starts_with("http://");
        };
        if (!web_url(page_url) || !web_url(top_url)) return;
        const std::string site = adblock::site_key(page_url);
        const std::string top_site = adblock::site_key(top_url);
        if (site.empty() || top_site.empty()) return;
        adblock::CosmeticResources resources;
        bool enabled = false;
        {
            std::lock_guard<std::mutex> lock(g_adblock.mutex);
            enabled = g_adblock.enabled && g_adblock.engine &&
                g_adblock.engine->available() &&
                !g_adblock.exceptions.contains(site) &&
                !g_adblock.exceptions.contains(top_site);
            if (enabled) resources = g_adblock.engine->cosmetic_resources(page_url);
        }
        const bool youtube = enabled && is_youtube_site(site);
        std::vector<std::string> watched;
        if (youtube) {
            watched.assign(std::begin(kYoutubeCosmeticSelectors),
                           std::end(kYoutubeCosmeticSelectors));
        }
        // Even an empty/disabled snapshot is pushed: it removes stale CSS and
        // disconnects actions left over from the previous SPA URL or settings.
        AURORA_LOG(util::LogLevel::Trace, "adblock")
            << "cosmetic resources for " << site << ": "
            << resources.json.size() << " JSON bytes, "
            << resources.scripts.size() << " scriptlet bytes";
        // Separate ExecuteJavaScript calls isolate a malformed scriptlet from
        // the stylesheet. No eval()/script tag means no page CSP dependency.
        if (!resources.scripts.empty()) {
            frame->ExecuteJavaScript(
                CefString(cosmetic_scriptlet_script(resources.scripts)),
                CefString("aurora://adblock-scriptlets.js"), 1);
        }
        frame->ExecuteJavaScript(
            CefString(cosmetic_filter_script(resources, watched, youtube)),
            CefString("aurora://adblock-cosmetic-filters.js"), 1);
    }

    void OnLoadStart(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     TransitionType /*transition_type*/) override {
        // Best-effort early hooks; the load-end calls repeat them safely if
        // this frame is not scriptable yet.
        inject_youtube_adskip(browser, frame);
        inject_cosmetic_filters(browser, frame);
        if (frame && frame->IsMain()) {
            renderer_discarded_ = false;
            webview::PageInfo info;
            info.has_audible_state = true;
            info.renderer_recovered = true;
            emit_(id_, info);
        }
        if (!frame || !frame->IsMain() || !popup_shim_provider_) return;
        const std::string script = popup_shim_provider_(id_, std::string_view{});
        if (script.empty()) return;
        // OnLoadStart runs after commit and before frame contents load. This is
        // the earliest browser-process hook where CEF 152 exposes a valid main
        // frame, so the wrapper is installed before popup.js calls tabs.query.
        frame->ExecuteJavaScript(
            CefString(script),
            CefString("aurora://extension-popup-tabs-shim.js"), 1);
    }

    void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override {
        if (!browser || !frame) return;
        if (frame->IsMain() && httpStatusCode > 0) {
            refresh_privacy_preferences();
            const auto entry = browser->GetHost()->GetVisibleNavigationEntry();
            // Ignore stale/failed entries and never associate a prior page's
            // certificate with a newly requested origin.
            if (entry && entry->GetURL() == frame->GetURL()) {
                const auto ssl = entry->GetSSLStatus();
                if (ssl && ssl->IsSecureConnection())
                    audit_certificate(browser, entry->GetURL(),
                                      ssl->GetX509Certificate(),
                                      ssl->GetCertStatus() == 0);
            }
        }
        inject_youtube_adskip(browser, frame);
        inject_cosmetic_filters(browser, frame);
    }

    // The popup half of the extension bridge. An extension popup running the
    // shim installed above emits a tagged console record when it calls one of
    // the chrome.* entry points that must reach the invoking tab
    // (tabs.sendMessage, tabs/scripting.executeScript). The provider performs
    // the work on the owning browser and hands back the JavaScript that
    // resolves the popup's pending promise.
    //
    // Records without the tag are left completely alone (returning false keeps
    // Chromium's normal console logging), and a browser that is not a
    // registered extension popup gets an empty reply, so ordinary pages cannot
    // reach this path by printing the tag themselves.
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t /*level*/,
                          const CefString& message,
                          const CefString& /*source*/,
                          int /*line*/) override {
        if (!popup_shim_provider_) return false;
        const std::string text = message.ToString();
        // A malformed or oversized record is dropped rather than parsed: the
        // bridge carries small control envelopes only.
        constexpr size_t kMaxBridgeBytes = 256u * 1024u;
        const std::string_view reply_tag(kReplyTag);
        if (text.compare(0, reply_tag.size(), reply_tag) == 0) {
            if (text.size() > kMaxBridgeBytes) return true;
            // This browser is the OWNER tab, not a popup, so the provider (which
            // owns the popup -> owner map) does the routing itself and nothing
            // is executed back in this frame. An empty answer means the record
            // did not belong to any live popup: fall through to normal logging.
            std::string envelope("reply|");
            envelope += text.substr(reply_tag.size());
            return !popup_shim_provider_(id_, envelope).empty();
        }
        const std::string_view tag(kBridgeTag);
        if (text.compare(0, tag.size(), tag) != 0) return false;
        if (text.size() > kMaxBridgeBytes) return true;
        const std::string reply =
            popup_shim_provider_(id_, std::string_view(text).substr(tag.size()));
        // No reply means this browser is not a registered extension popup (or
        // the envelope was malformed). Fall through to normal logging so an
        // ordinary page's output is never swallowed and a broken envelope
        // stays visible in devtools.
        if (reply.empty()) return false;
        if (browser) {
            if (auto frame = browser->GetMainFrame(); frame) {
                frame->ExecuteJavaScript(
                    CefString(reply),
                    CefString("aurora://extension-popup-bridge.js"), 1);
            }
        }
        return true;  // the envelope is Aurora's, not page output
    }

    // Exactly-once answer to a pending permission request. The host may
    // answer much later (from a Qt prompt) or never (window closed, tab
    // gone); the shared state makes both safe, and dropping the request
    // unanswered DENIES — the default-deny invariant cannot be bypassed by
    // accident.
    struct MediaAnswer {
        CefRefPtr<CefMediaAccessCallback> callback;
        uint32_t granted = 0;  // what "allow" means (never more than asked)
        bool answered = false;
        ~MediaAnswer() {
            if (callback && !answered) callback->Continue(0);
        }
    };
    // Non-media prompts answer through CefPermissionPromptCallback, whose
    // Continue() takes a cef_permission_request_result_t (there is no bool
    // overload — see include/cef_permission_handler.h). Dropping the request
    // unanswered therefore denies explicitly rather than leaving the page's
    // promise pending forever, which CEF_PERMISSION_RESULT_IGNORE would do.
    struct PromptAnswer {
        CefRefPtr<CefPermissionPromptCallback> callback;
        bool answered = false;
        ~PromptAnswer() {
            if (callback && !answered)
                callback->Continue(CEF_PERMISSION_RESULT_DENY);
        }
    };

    // getUserMedia. Never auto-grants: the request is always passed to the
    // host, which prompts (default-deny on dismiss) or answers from a
    // remembered per-origin decision. Desktop capture (screen share) is
    // denied outright — a prompt that cannot preview what is being shared
    // must not offer it.
    bool OnRequestMediaAccessPermission(
        CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/,
        const CefString& requesting_url, uint32_t requested_permissions,
        CefRefPtr<CefMediaAccessCallback> callback) override {
        constexpr uint32_t kDesktop =
            CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE |
            CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE;
        const uint32_t media =
            requested_permissions & (CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE |
                                     CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE);
        if (!callback) return true;
        const auto deny = [&]() {
            AURORA_LOG(util::LogLevel::Warn, "cef")
                << "denying media access (perms=" << requested_permissions
                << ") from " << requesting_url.ToString();
            callback->Continue(0);
            return true;
        };
        if ((requested_permissions & kDesktop) != 0 || media == 0)
            return deny();
        if (!permission_emit_) return deny();

        webview::PermissionRequest req;
        req.tab = id_;
        req.origin = permissions::origin_of(requesting_url.ToString());
        if (req.origin.empty()) return deny();  // unkeyable origin
        req.audio = (media & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE) != 0;
        req.video = (media & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE) != 0;
        auto answer = std::make_shared<MediaAnswer>();
        answer->callback = callback;
        answer->granted = media;
        req.respond = [answer](bool allow) {
            if (answer->answered || !answer->callback) return;
            answer->answered = true;
            answer->callback->Continue(allow ? answer->granted : 0);
        };
        permission_emit_(std::move(req));
        return true;
    }

    // Non-media permission prompts. Only notifications get the host's prompt;
    // everything else (geolocation, MIDI, clipboard, ...) returns false so
    // CEF applies its default — deny.
    bool OnShowPermissionPrompt(
        CefRefPtr<CefBrowser> /*browser*/, uint64_t /*prompt_id*/,
        const CefString& requesting_url, uint32_t requested_permissions,
        CefRefPtr<CefPermissionPromptCallback> callback) override {
        if ((requested_permissions & CEF_PERMISSION_TYPE_NOTIFICATIONS) == 0)
            return false;
        if (!permission_emit_ || !callback) return false;

        webview::PermissionRequest req;
        req.tab = id_;
        req.origin = permissions::origin_of(requesting_url.ToString());
        if (req.origin.empty()) {
            callback->Continue(CEF_PERMISSION_RESULT_DENY);
            return true;
        }
        req.notification = true;
        auto answer = std::make_shared<PromptAnswer>();
        answer->callback = callback;
        req.respond = [answer](bool allow) {
            if (answer->answered || !answer->callback) return;
            answer->answered = true;
            answer->callback->Continue(allow ? CEF_PERMISSION_RESULT_ACCEPT
                                             : CEF_PERMISSION_RESULT_DENY);
        };
        permission_emit_(std::move(req));
        return true;
    }

    void OnTitleChange(CefRefPtr<CefBrowser> /*browser*/,
                       const CefString& title) override {
        webview::PageInfo info;
        info.title = title.ToString();
        emit_(id_, info);
    }

    // Alloy invokes this when the WebContents becomes audible, including
    // cross-origin frames and Web Audio. Decline capture: accepting it would
    // mute Chromium's normal output and require us to play the PCM ourselves.
    // CEF has no passive silence callback, so keep the document protected until
    // its next main-frame navigation rather than risk suspending ongoing audio.
    bool GetAudioParameters(CefRefPtr<CefBrowser> /*browser*/,
                            CefAudioParameters& /*params*/) override {
        webview::PageInfo info;
        info.has_audible_state = true;
        info.audible = true;
        emit_(id_, info);
        return false;
    }
    void OnAudioStreamStarted(CefRefPtr<CefBrowser> /*browser*/,
                              const CefAudioParameters& /*params*/,
                              int /*channels*/) override {}
    void OnAudioStreamPacket(CefRefPtr<CefBrowser> /*browser*/,
                             const float** /*data*/, int /*frames*/,
                             int64_t /*pts*/) override {}
    void OnAudioStreamStopped(CefRefPtr<CefBrowser> /*browser*/) override {}
    void OnAudioStreamError(CefRefPtr<CefBrowser> /*browser*/,
                            const CefString& /*message*/) override {}

    void set_visible(CefRefPtr<CefBrowser> browser, bool visible) {
        visible_ = visible;
        apply_hidden(browser);
    }

    void set_suspended(CefRefPtr<CefBrowser> browser, bool suspended) {
        const bool waking = suspended_ && !suspended;
        suspended_ = suspended;
        apply_hidden(browser);
        // WasHidden itself never discards or reloads a document. Only a
        // renderer actually killed while suspended needs reconstruction;
        // crashes/OOM keep the explicit crash/reload UI instead.
        if (waking && renderer_discarded_) {
            renderer_discarded_ = false;
            browser->Reload();
        }
    }

    void OnLoadingStateChange(CefRefPtr<CefBrowser> /*browser*/,
                              bool is_loading, bool can_go_back,
                              bool can_go_forward) override {
        webview::PageInfo info;
        info.has_nav_state = true;  // this callback owns the real nav state
        info.loading = is_loading;
        info.load_progress = is_loading ? 0 : 100;
        info.can_go_back = can_go_back;
        info.can_go_forward = can_go_forward;
        emit_(id_, info);
    }

    // The page's favicon changed. CEF hands us candidate icon URLs; we take
    // the first and forward it to the host, which loads the image and shows it
    // on the tab. (History/bookmarks derive their icons from the URL later.)
    void OnFaviconURLChange(CefRefPtr<CefBrowser> /*browser*/,
                            const std::vector<CefString>& icon_urls) override {
        if (icon_urls.empty()) return;
        webview::PageInfo info;
        info.favicon_url = icon_urls.front().ToString();
        if (emit_) emit_(id_, info);
    }

    // The renderer's answer to CefAdapter::request_memory().
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> /*browser*/,
                                  CefRefPtr<CefFrame> /*frame*/,
                                  CefProcessId /*source_process*/,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (!message.get() || message->GetName() != "aurora.memory.reply") {
            return false;
        }
        CefRefPtr<CefListValue> args = message->GetArgumentList();
        if (!args.get() || args->GetSize() < 1) return true;
        webview::PageInfo info;
        info.memory_bytes = static_cast<int64_t>(args->GetDouble(0));
        if (emit_) emit_(id_, info);
        return true;
    }

    // A video's fullscreen button, or any Fullscreen API call, lands here.
    // The engine will size the content to our host surface; making the window
    // itself fullscreen and hiding the tab strip / toolbar is our job.
    void OnFullscreenModeChange(CefRefPtr<CefBrowser> /*browser*/,
                                bool fullscreen) override {
        if (fullscreen_emit_) fullscreen_emit_(id_, fullscreen);
    }

    // Keyboard handling. Two layers, in order:
    //
    // 1. Escape and F11 while CONTENT fullscreen. They cannot wait for the
    //    host: in fullscreen the engine's native child window holds the
    //    keyboard focus, so the key never reaches the Qt widget tree and the
    //    window stays stuck fullscreen. Intercepting here — before the page
    //    sees the key — is what Chrome itself does.
    //
    // 2. Every other browser accelerator. While a page has keyboard focus the
    //    native child window owns the keyboard and Qt sees nothing, so NONE
    //    of the MainWindow QShortcuts (Ctrl+T/W/N/J/H/D/F, Ctrl+Shift+I/T,
    //    Ctrl+Plus/Minus/0, F5, Alt+Left/Right, ...) can fire. We therefore
    //    offer each accelerator-LOOKING key (a modifier held, a function key,
    //    or Escape) to the host, which answers synchronously whether one of
    //    its window shortcuts claims the combo; only then is the key consumed
    //    (the host performs the action itself, posted to the Qt event loop —
    //    never inline, since slots like Ctrl+T/W create and close browsers).
    //    Plain typing is never forwarded: it goes to the page untouched.
    //
    // The event stays windows-key-code based (event.windows_key_code), which
    // CEF populates identically on every platform; the host owns the small
    // VK-to-Qt table.
    bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event,
                       CefEventHandle /*os_event*/,
                       bool* /*is_keyboard_shortcut*/) override {
        if (event.type != KEYEVENT_RAWKEYDOWN && event.type != KEYEVENT_KEYDOWN) {
            return false;
        }
        constexpr int kVkEscape = 0x1B;
        constexpr int kVkF11 = 0x7A;
        const bool is_exit_key = event.windows_key_code == kVkEscape ||
                                 event.windows_key_code == kVkF11;
        if (is_exit_key && browser) {
            // Only act while actually fullscreen, so Escape keeps its normal
            // meaning (stop load, dismiss a find bar) the rest of the time.
            auto host = browser->GetHost();
            if (host && host->IsFullscreen()) {
                host->ExitFullscreen(/*will_cause_resize=*/true);
                return true;  // consumed
            }
        }

        // Layer 2: offer accelerator-looking keys to the host chrome.
        if (!key_emit_) return false;
        const auto mods = event.modifiers;
        const bool modifier_held =
            (mods & (EVENTFLAG_CONTROL_DOWN | EVENTFLAG_ALT_DOWN |
                     EVENTFLAG_COMMAND_DOWN)) != 0;
        // F1..F24 (0x70..0x87) are accelerators even without a modifier
        // (F5 reload, F11 fullscreen, F12 devtools). Escape participates in
        // the chrome's accelerator chain (omnibox -> find bar -> fullscreen).
        const bool bare_function_key =
            event.windows_key_code >= 0x70 && event.windows_key_code <= 0x87;
        if (!modifier_held && !bare_function_key && !is_exit_key) return false;

        webview::EngineKeyEvent fwd;
        fwd.key_code = event.windows_key_code;
        fwd.ctrl = (mods & (EVENTFLAG_CONTROL_DOWN | EVENTFLAG_COMMAND_DOWN)) != 0;
        fwd.shift = (mods & EVENTFLAG_SHIFT_DOWN) != 0;
        fwd.alt = (mods & EVENTFLAG_ALT_DOWN) != 0;
        fwd.key_down = true;
        return key_emit_(id_, fwd);  // true = host consumed it
    }

    void OnLoadingProgressChange(CefRefPtr<CefBrowser> /*browser*/,
                                 double progress) override {
        webview::PageInfo info;
        info.load_progress = static_cast<int>(progress * 100.0);
        emit_(id_, info);
    }

    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> /*browser*/,
                                   TerminationStatus status,
                                   int /*error_code*/,
                                   const CefString& /*error_string*/) override {
        if (suspended_ && status == TS_PROCESS_WAS_KILLED) {
            renderer_discarded_ = true;
            return;
        }
        renderer_discarded_ = false;
        webview::PageInfo info;
        info.renderer_crashed = true;
        info.renderer_out_of_memory = status == TS_PROCESS_OOM;
        emit_(id_, info);
    }

    void OnAddressChange(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         const CefString& url) override {
        // CEF reports history.pushState/replaceState and fragment navigation
        // here too; these do not necessarily produce either load callback.
        inject_cosmetic_filters(browser, frame);
        // Only the main frame's URL belongs in the omnibox / history / tab
        // model. Sub-frame address changes (iframes, ads) must not stomp the
        // address bar. Emitting the main-frame URL here — with an empty url
        // on every title/loading callback — is what lets the host distinguish
        // a real address change from a no-op and stop clearing the omnibox.
        if (!frame || !frame->IsMain()) return;
        webview::PageInfo info;
        info.url = url.ToString();
        info.security_origin = info.url;
        emit_(id_, info);
    }

    // The page's own default entries are replaced by a menu Aurora builds, so
    // that every platform gets the same, complete set (Chromium's default is
    // sparse and several of its entries are inert in an embedded browser).
    // CEF has a single MEDIA type flag; GetMediaType distinguishes images from
    // video. Edit commands use CEF's built-in ids, which CEF executes itself;
    // everything else is an Aurora command handled in OnContextMenuCommand.
    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> /*frame*/,
                             CefRefPtr<CefContextMenuParams> params,
                             CefRefPtr<CefMenuModel> model) override {
        if (!params || !model) return;
        const auto flags = params->GetTypeFlags();
        const int edit = params->GetEditStateFlags();
        const bool is_media = (flags & CM_TYPEFLAG_MEDIA) != 0;
        const bool is_image =
            is_media && params->GetMediaType() == CM_MEDIATYPE_IMAGE;
        const bool is_video =
            is_media && params->GetMediaType() == CM_MEDIATYPE_VIDEO;
        const bool is_link = (flags & CM_TYPEFLAG_LINK) != 0;
        const bool is_editable = (flags & CM_TYPEFLAG_EDITABLE) != 0;
        const std::string selection =
            tidy_selection(params->GetSelectionText().ToString());
        const bool has_selection = !selection.empty();

        model->Clear();
        // A separator only ever goes between two non-empty sections.
        auto section = [&model] {
            if (model->GetCount() > 0) model->AddSeparator();
        };

        if (is_editable) {
            model->AddItem(MENU_ID_UNDO, CefString("Undo"));
            model->SetEnabled(MENU_ID_UNDO, (edit & CM_EDITFLAG_CAN_UNDO) != 0);
            model->AddItem(MENU_ID_REDO, CefString("Redo"));
            model->SetEnabled(MENU_ID_REDO, (edit & CM_EDITFLAG_CAN_REDO) != 0);
            section();
            model->AddItem(MENU_ID_CUT, CefString("Cut"));
            model->SetEnabled(MENU_ID_CUT, (edit & CM_EDITFLAG_CAN_CUT) != 0);
            model->AddItem(MENU_ID_COPY, CefString("Copy"));
            model->SetEnabled(MENU_ID_COPY, (edit & CM_EDITFLAG_CAN_COPY) != 0);
            model->AddItem(MENU_ID_PASTE, CefString("Paste"));
            model->SetEnabled(MENU_ID_PASTE, (edit & CM_EDITFLAG_CAN_PASTE) != 0);
            model->AddItem(MENU_ID_DELETE, CefString("Delete"));
            model->SetEnabled(MENU_ID_DELETE, (edit & CM_EDITFLAG_CAN_DELETE) != 0);
            section();
            model->AddItem(MENU_ID_SELECT_ALL, CefString("Select all"));
            model->SetEnabled(MENU_ID_SELECT_ALL,
                              (edit & CM_EDITFLAG_CAN_SELECT_ALL) != 0);
        } else if (has_selection) {
            model->AddItem(MENU_ID_COPY, CefString("Copy"));
            model->SetEnabled(MENU_ID_COPY, (edit & CM_EDITFLAG_CAN_COPY) != 0);
        }
        if (has_selection) {
            section();
            model->AddItem(kSearchSelection,
                           CefString("Search for \"" +
                                     menu_snippet(selection, 32) + "\""));
        }

        if (is_link) {
            section();
            model->AddItem(kOpenLinkNewTab, CefString("Open link in new tab"));
            model->AddItem(kCopyLinkAddress, CefString("Copy link address"));
        }
        if (is_image) {
            section();
            model->AddItem(kSaveImage, CefString(u"Save image as\u2026"));
            model->AddItem(kCopyImage, CefString("Copy image"));
            model->AddItem(kCopyImageAddress, CefString("Copy image address"));
            model->AddItem(kOpenImageNewTab, CefString("Open image in new tab"));
        }
        if (is_video) {
            section();
            model->AddItem(kSaveVideo, CefString(u"Save video as\u2026"));
            model->AddItem(kCopyVideoAddress, CefString("Copy video address"));
            model->AddItem(kOpenVideoNewTab, CefString("Open video in new tab"));
        }

        // Plain page: navigation and page-level commands, like every browser.
        if (!is_editable && !is_link && !is_media && !has_selection) {
            section();
            model->AddItem(kPageBack, CefString("Back"));
            model->SetEnabled(kPageBack, browser && browser->CanGoBack());
            model->AddItem(kPageForward, CefString("Forward"));
            model->SetEnabled(kPageForward, browser && browser->CanGoForward());
            model->AddItem(kPageReload, CefString("Reload"));
            section();
            model->AddItem(kPageSavePdf, CefString(u"Save as PDF\u2026"));
            model->AddItem(kPagePrint, CefString(u"Print\u2026"));
            model->AddItem(kPageViewSource, CefString("View page source"));
        }

        section();
        model->AddItem(kInspect, CefString("Inspect"));
    }

    void emit_action(webview::WebViewController::ContextAction action,
                     std::string text = {}) {
        if (!context_action_emit_) return;
        webview::WebViewController::ContextActionRequest req;
        req.action = action;
        req.text = std::move(text);
        context_action_emit_(id_, std::move(req));
    }

    bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                              CefRefPtr<CefFrame> /*frame*/,
                              CefRefPtr<CefContextMenuParams> params,
                              int command_id,
                              EventFlags /*event_flags*/) override {
        if (!browser || !params) return false;
        auto host = browser->GetHost();
        if (!host) return false;
        using Action = webview::WebViewController::ContextAction;

        switch (command_id) {
        case kSaveImage: {
            const CefString url = params->GetSourceUrl();
            if (!url.empty()) {
                ++force_save_dialog_urls_[url.ToString()];
                host->StartDownload(url);
            }
            return true;
        }
        case kSaveVideo: {
            const CefString url = params->GetSourceUrl();
            if (!url.empty()) host->StartDownload(url);
            return true;
        }
        case kCopyImage: {
            const CefString url = params->GetSourceUrl();
            if (!url.empty() && params->HasImageContents()) {
                host->DownloadImage(
                    url, false, 0, false,
                    CefRefPtr<CopyImageCallback>(new CopyImageCallback(
                        host->GetWindowHandle(), id_, context_action_emit_)));
            }
            return true;
        }
        case kCopyImageAddress:
        case kCopyVideoAddress:
            copy_text(params->GetSourceUrl(), host);
            return true;
        case kCopyLinkAddress:
            copy_text(params->GetUnfilteredLinkUrl(), host);
            return true;
        case kOpenImageNewTab:
        case kOpenVideoNewTab: {
            const std::string url = params->GetSourceUrl().ToString();
            if (!url.empty() && new_tab_emit_) new_tab_emit_(url);
            return true;
        }
        case kOpenLinkNewTab: {
            const std::string url = params->GetLinkUrl().ToString();
            if (!url.empty() && new_tab_emit_) new_tab_emit_(url);
            return true;
        }
        case kSearchSelection: {
            const std::string text =
                tidy_selection(params->GetSelectionText().ToString());
            if (!text.empty()) emit_action(Action::SearchSelection, text);
            return true;
        }
        case kPageBack:    emit_action(Action::Back); return true;
        case kPageForward: emit_action(Action::Forward); return true;
        case kPageReload:  emit_action(Action::Reload); return true;
        case kPageSavePdf: emit_action(Action::SavePageAsPdf); return true;
        case kPagePrint:   emit_action(Action::Print); return true;
        case kInspect:     emit_action(Action::Inspect); return true;
        case kPageViewSource: {
            // Only real web documents have source worth showing; the host
            // opens it as a "view-source:" tab.
            const std::string page = params->GetPageUrl().ToString();
            if (!page.empty() && new_tab_emit_) new_tab_emit_("view-source:" + page);
            return true;
        }
        default:
            // CEF's own ids (Cut/Copy/Paste/Undo/Select all...) fall through to
            // CEF, which executes them.
            return false;
        }
    }

    // Copies text to the system clipboard. Win32 uses the native clipboard
    // directly; every other platform asks the host (Qt owns the clipboard).
    void copy_text(const CefString& value,
                   [[maybe_unused]] CefRefPtr<CefBrowserHost> host) {
        if (value.empty()) return;
#if defined(OS_WIN)
        copy_text_to_clipboard(host->GetWindowHandle(), value);
#else
        emit_action(webview::WebViewController::ContextAction::CopyText,
                    value.ToString());
#endif
    }

    // Closing a *tab* must never close the host window. CEF's default DoClose
    // implementation sends a close message to the browser's top-level window —
    // which, because our browsers are children of the Qt window, is the whole
    // browser window. Returning true here tells CEF we handle the close
    // ourselves: the browser is destroyed and OnBeforeClose still fires, but no
    // WM_CLOSE / delete-event reaches the Qt window. The window is closed only
    // by MainWindow, once the last tab is gone.
    bool DoClose(CefRefPtr<CefBrowser> /*browser*/) override { return true; }

    // Fired on the UI thread (during a CefDoMessageLoopWork pump) once a
    // browser is fully closed. We must drop the CefRefPtr<CefBrowser> we hold
    // in CefAdapter::browsers_ here: as long as we hold it, CEF keeps the
    // browser host — and its GPU/renderer subprocess connection — alive, so the
    // GPU process would never exit before CefShutdown. This is the exact
    // “something is still running” that prevented deleting the app folder.
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        g_cosmetic_refreshers[id_] = [this, browser] {
            std::vector<CefString> identifiers;
            browser->GetFrameIdentifiers(identifiers);
            for (const auto& identifier : identifiers)
                inject_cosmetic_filters(browser, browser->GetFrameByIdentifier(identifier));
        };
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) override {
        closed_ = true;
        g_cosmetic_refreshers.erase(id_);
        if (close_emit_) close_emit_(id_);
    }

    // Returns true if `url` is a page Chromium wants to open for itself rather
    // than something the user asked for: its own welcome/onboarding surfaces,
    // its extension-management and settings UI, and the Web Store install /
    // remove / detail / support flows.
    //
    // Aurora owns those experiences (aurora://extensions, aurora://settings,
    // the puzzle-piece popup), so a Chromium page for any of them is always a
    // regression from the user's point of view: installing or uninstalling an
    // extension used to pop chrome://extensions, a Chrome welcome tab, or the
    // Web Store's "removed from Chrome" page on top of Aurora's own UI.
    // Cancelling the popup here means CEF never creates a window or a tab for
    // them.
    //
    // Note what is deliberately NOT matched: the chrome:// list is an explicit
    // allow-list of Chromium-owned surfaces, not a blanket "chrome://" match,
    // so a URL Aurora does route through the engine on purpose still works.
    static bool is_chromium_owned_navigation(const std::string& url) {
        if (url.empty()) return false;
        // Lower-case copy for case-insensitive matching.
        std::string lower(url);
        for (auto& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // Chromium's own UI pages. Prefix matches, so query strings and
        // sub-paths (chrome://welcome/?variant=..., chrome://settings/extensions,
        // chrome://onboarding-welcome) are covered too.
        static constexpr std::string_view kChromiumPages[] = {
            "chrome://newtab",
            "chrome://new-tab-page",
            "chrome://welcome",
            "chrome://onboarding",
            "chrome://extensions",
            "chrome://settings",
            "chrome://chrome-urls",
            "chrome://management",
            "chrome://whats-new",
            "chrome://apps",
        };
        for (const auto& page : kChromiumPages) {
            if (lower.starts_with(page)) return true;
        }

        // Chrome Web Store flows: the install/remove confirmation pages and the
        // detail/support pages Chromium opens after an extension changes state.
        // Both the legacy chrome.google.com/webstore host and the current
        // chromewebstore.google.com one are matched.
        const bool webstore =
            lower.find("chrome.google.com/webstore") != std::string::npos ||
            lower.find("chromewebstore.google.com") != std::string::npos;
        if (webstore) {
            static constexpr std::string_view kWebstoreFlows[] = {
                "/detail", "/install", "/remove", "/uninstall",
                "/support", "/report", "/inline",
            };
            for (const auto& flow : kWebstoreFlows) {
                if (lower.find(flow) != std::string::npos) return true;
            }
        }
        return false;
    }

    // Returns true if `url` is an extension-owned page that CHROMIUM opens by
    // itself right after an extension is installed — typically
    // chrome-extension://<id>/options.html, or a welcome/thank-you/getting
    // started page declared by the extension.
    //
    // This is intentionally narrow, and the caller only consults it for
    // navigations that carry no user gesture: a blanket chrome-extension://
    // block would also kill the surfaces Aurora hosts on purpose (the
    // default_popup window opened by MainWindow::open_extension_popup(), and
    // the options page the user opens from the puzzle-piece popup — neither of
    // which comes through OnBeforePopup at all).
    static bool is_extension_auto_opened_page(const std::string& url) {
        if (url.empty()) return false;
        std::string lower(url);
        for (auto& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!lower.starts_with("chrome-extension://")) return false;
        static constexpr std::string_view kInstallPages[] = {
            "options", "welcome", "install", "onboard", "getting_started",
            "getting-started", "thankyou", "thank_you", "thank-you",
            "firstrun", "first_run", "first-run", "intro",
        };
        for (const auto& page : kInstallPages) {
            if (lower.find(page) != std::string::npos) return true;
        }
        return false;
    }

    // Intercept every "open in new window" attempt (target=_blank,
    // window.open, middle-click that dispositions to a new window, etc.) and
    // route the target URL to the host as a new in-app tab instead. Returning
    // true cancels CEF's own popup browser creation, so nothing escapes to the
    // OS default browser. The host callback defers actual tab creation to the
    // next event-loop turn so we don't re-enter CEF browser creation from
    // inside this callback.
    bool OnBeforePopup(CefRefPtr<CefBrowser> /*browser*/,
                       CefRefPtr<CefFrame> /*frame*/,
                       int /*popup_id*/,
                       const CefString& target_url,
                       const CefString& /*target_frame_name*/,
                       // Qualified: CefRequestHandler declares an identically
                       // named typedef, so the unqualified name is ambiguous
                       // now that both are base classes.
                       CefLifeSpanHandler::WindowOpenDisposition
                           /*target_disposition*/,
                       bool user_gesture,
                       const CefPopupFeatures& /*popupFeatures*/,
                       CefWindowInfo& /*windowInfo*/,
                       CefRefPtr<CefClient>& /*client*/,
                       CefBrowserSettings& settings,
                       CefRefPtr<CefDictionaryValue>& /*extra_info*/,
                       bool* no_javascript_access) override {
        if (no_javascript_access) *no_javascript_access = false;
        const std::string url = target_url.ToString();
        // Suppress Chromium's own pages (welcome/onboarding/newtab, the
        // extension-management and settings UI, Web Store install/remove
        // flows). Aurora owns those experiences, so installing or uninstalling
        // an extension must never open a Chromium page.
        if (is_chromium_owned_navigation(url)) {
            AURORA_LOG(util::LogLevel::Info, "cef")
                << "suppressing Chromium-owned page: " << url;
            return true;  // cancel the popup
        }
        // An extension's options/welcome page that Chromium opens for itself on
        // install: no user gesture, so it is not the user asking for it. The
        // user can still open the options page from Aurora's puzzle-piece
        // popup, and extension default_popup windows never reach this handler.
        if (!user_gesture && is_extension_auto_opened_page(url)) {
            AURORA_LOG(util::LogLevel::Info, "cef")
                << "suppressing auto-opened extension page: " << url;
            return true;  // cancel the popup
        }
        // Ensure JavaScript is explicitly enabled in the popup browser.
        // CEF defaults to JavaScript enabled, but extension popups sometimes
        // inherit a disabled state from command-line switches; setting it
        // explicitly here guarantees popup HTML with scripts works.
        settings.javascript = STATE_ENABLED;
        // Cancel CEF only after the real host has accepted the request. The
        // previous unconditional `true` turned a missing/unwired host callback
        // into the exact observed failure mode: CEF did not create a popup and
        // Aurora did not create a tab.
        if (new_tab_emit_ && new_tab_emit_(url)) return true;
        AURORA_LOG(util::LogLevel::Warn, "cef")
            << "no host accepted site-initiated popup: "
            << (url.empty() ? std::string("<blank>") : url);
        return false;
    }

    // A download is a packaged Chrome extension when the server says so via
    // MIME type, the suggested file name ends in .crx / .crx3, or it came from
    // the Chrome Web Store update endpoint (whose response metadata is not
    // always specific enough to identify the package).
    static bool is_extension_package(const std::string& mime,
                                     const std::string& file_name,
                                     const CefString& url) {
        if (mime == "application/x-chrome-extension") return true;
        std::string lower(file_name);
        for (auto& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.size() > 4 &&
            (lower.ends_with(".crx") || lower.ends_with(".crx3"))) {
            return true;
        }

        CefURLParts parts;
        if (!CefParseURL(url, parts)) return false;
        std::string host = CefString(&parts.host).ToString();
        for (auto& c : host)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string path = CefString(&parts.path).ToString();
        for (auto& c : path)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // CEF/Chromium component & extension-update payloads are served from
        // clients2.googleusercontent.com (the redirect target of the legacy
        // clients2.google.com/service/update2/crx endpoint), with the .crx
        // file at the end of the URL path. The earlier check only matched the
        // original endpoint host+path, so the redirected payload URLs slipped
        // through and appeared as stuck 0% user downloads in the popover.
        if ((host == "clients2.googleusercontent.com" ||
             host == "clients2.google.com") &&
            (path.ends_with(".crx") || path.ends_with(".crx3"))) {
            return true;
        }
        return host == "clients2.google.com" &&
               path == "/service/update2/crx";
    }

    // Downloads. We handle them ourselves (return true) so CEF does not open
    // the OS default browser or a Chrome-style download shelf. Files are saved
    // under the configured Downloads folder with collision avoidance; the
    // lifecycle is reported to the host for DownloadService + the Downloads page.
    //
    // Packaged extensions (.crx / .crx3, by MIME type or file name) are NOT
    // downloads the user wants in their Downloads folder — they are install
    // attempts. The file is rerouted to a private temp path and, once it
    // completes, handed to the host for ExtensionService::install_from_file.
    bool OnBeforeDownload(CefRefPtr<CefBrowser> /*browser*/,
                          CefRefPtr<CefDownloadItem> item,
                          const CefString& suggested_name,
                          CefRefPtr<CefBeforeDownloadCallback> callback) override {
        track_download(item->GetId(), true);
        namespace fs = std::filesystem;
        std::error_code ec;
        std::string effective_downloads_dir = downloads_dir_;
        if (downloads_dir_provider_) {
            const std::string configured_dir = downloads_dir_provider_();
            if (!configured_dir.empty()) {
                effective_downloads_dir = configured_dir;
            }
        }
        // The host normally sets downloads_dir_ from QStandardPaths. If it did
        // not, or if that directory cannot be created/written, we must NOT
        // hand CEF a path it cannot open: Chromium reports that as a failed
        // download ("Interrupted") rather than falling back on its own. An
        // empty download_path passed to Continue() means "use the suggested
        // name in the default temp directory", which always works — so an
        // unusable directory degrades to a saved file instead of a failure.
        fs::path dir;
        bool dir_usable = false;
        if (!effective_downloads_dir.empty()) {
            dir = path_from_utf8(effective_downloads_dir);
            dir.make_preferred();
            fs::create_directories(dir, ec);          // no-op when it exists
            dir_usable = fs::is_directory(dir, ec) && !ec;
            if (dir_usable) {
                // is_directory() says nothing about create/write permission.
                // Probe the operation Chromium must perform before giving it
                // an explicit target; otherwise CEF reports FILE_ACCESS_DENIED
                // as an immediately interrupted download.
                const fs::path probe =
                    dir / (".aurora-write-test-" + std::to_string(id_.value()) + "-" +
                           std::to_string(item->GetId()) + ".tmp");
                std::ofstream output(probe, std::ios::binary | std::ios::trunc);
                output.put('\0');
                output.close();
                dir_usable = output.good();
                std::error_code cleanup_ec;
                fs::remove(probe, cleanup_ec);
            }
        }
        if (!dir_usable) {
            AURORA_LOG(util::LogLevel::Warn, "cef")
                << "downloads dir unusable ('" << effective_downloads_dir
                << "') — letting CEF choose the destination";
        }
        std::string name = suggested_name.ToString();
        bool force_save_dialog =
            take_force_save_dialog(item->GetURL().ToString());
        if (!force_save_dialog) {
            // A media URL may redirect before CEF creates the download item.
            // The original URL still identifies the context-menu request.
            force_save_dialog =
                take_force_save_dialog(item->GetOriginalUrl().ToString());
        }
        if (is_extension_package(item->GetMimeType().ToString(), name,
                                 item->GetURL())) {
            // Download to the temp dir under the id-stable name; completion
            // (or failure/cancel) is handled in OnDownloadUpdated, which
            // either hands the file to the host or deletes it. No
            // DownloadEvent is emitted: this is an install attempt, not a
            // download, so it must not appear on the Downloads page.
            fs::path tmp;
            if (extension_emit_) {
                tmp = fs::temp_directory_path(ec) /
                      ("aurora-ext-" + std::to_string(item->GetId()) + ".crx");
            }
            if (extension_emit_ && !ec) {
                PendingCrx pending;
                pending.path = path_to_utf8(tmp);
                pending.url = item->GetURL().ToString();
                pending.file_name = name;
                pending_crx_[item->GetId()] = std::move(pending);
                if (callback)
                    callback->Continue(path_to_cef(tmp), false);
                return true;
            }
            // No host to install into: refuse the download outright rather
            // than silently saving a .crx the user cannot use.
            // CefBeforeDownloadCallback has no Cancel() — only
            // Continue(download_path, show_dialog) (see
            // include/cef_download_handler.h). Returning true *without*
            // invoking the callback is how a download is refused: CEF treats
            // the un-continued request as cancelled and never writes a file.
            AURORA_LOG(util::LogLevel::Warn, "cef")
                << "refusing .crx download " << name
                << " (no extension-install handler)";
            track_download(item->GetId(), false);
            return true;
        }
        if (name.empty()) name = "download";
        // Keep the file inside the Downloads folder even if the suggested name
        // contains path separators.
        fs::path leaf = path_from_utf8(name);
        bool ask_where = force_save_dialog;
        if (!ask_where && ask_where_provider_) {
            ask_where = ask_where_provider_();
        }
        if (ask_where) {
            if (!callback || !save_path_chooser_) {
                AURORA_LOG(util::LogLevel::Warn, "cef")
                    << "refusing download " << name
                    << " because no save-path chooser is available";
                track_download(item->GetId(), false);
                return true;
            }

            const fs::path initial_target =
                dir_usable ? dir / leaf.filename() : leaf.filename();
            const uint32_t download_id = item->GetId();
            const std::string url = item->GetURL().ToString();
            const std::string mime = item->GetMimeType().ToString();
            const int64_t total_bytes = item->GetTotalBytes();
            const int64_t received = item->GetReceivedBytes();
            CefRefPtr<TabClient> self(this);

            // The Qt host queues the native QFileDialog onto its event loop,
            // so this CEF callback returns immediately. Keep only immutable
            // download metadata: CEF explicitly forbids retaining |item|
            // beyond OnBeforeDownload. The CefRefPtr callback and self keep
            // the asynchronous continuation and client alive.
            save_path_chooser_(
                path_to_utf8(initial_target),
                [self, callback, download_id, url, mime, total_bytes,
                 received](std::string chosen_path) {
                    if (chosen_path.empty()) {
                        // CefBeforeDownloadCallback has no Cancel(). Not
                        // continuing it cancels the pending download.
                        self->track_download(download_id, false);
                        return;
                    }
                    fs::path target = path_from_utf8(chosen_path);
                    target.make_preferred();
                    const std::string target_path = path_to_utf8(target);
                    self->target_paths_[download_id] = target_path;
                    callback->Continue(path_to_cef(target), false);

                    webview::DownloadEvent ev;
                    ev.id = download_id;
                    ev.url = url;
                    ev.file_name = path_to_utf8(target.filename());
                    ev.target_path = target_path;
                    ev.mime = mime;
                    ev.total_bytes = total_bytes;
                    ev.received = received;
                    ev.state = 0;
                    if (self->download_emit_) self->download_emit_(ev);
                });
            return true;
        }
        if (!dir_usable) {
            // Let CEF pick the destination (suggested name, its own temp dir)
            // and report the download with the name we know. GetFullPath() in
            // OnDownloadUpdated will carry the real path once it exists.
            if (callback) callback->Continue(CefString(), false);
            webview::DownloadEvent ev;
            ev.id = item->GetId();
            ev.url = item->GetURL().ToString();
            ev.file_name = path_to_utf8(leaf.filename());
            ev.mime = item->GetMimeType().ToString();
            ev.total_bytes = item->GetTotalBytes();
            ev.received = item->GetReceivedBytes();
            ev.state = 0;
            if (download_emit_) download_emit_(ev);
            return true;
        }
        fs::path target = dir / leaf.filename();
        if (target.has_parent_path() && target.parent_path() != dir) {
            target = dir / leaf.filename();
        }
        // Collision avoidance: name (1).ext, name (2).ext, ...
        if (fs::exists(target, ec)) {
            const auto stem = path_to_utf8(target.stem());
            const auto ext = path_to_utf8(target.extension());
            for (int n = 1;; ++n) {
                fs::path candidate =
                    dir / path_from_utf8(
                        stem + " (" + std::to_string(n) + ")" + ext);
                if (!fs::exists(candidate, ec)) { target = candidate; break; }
            }
        }
        target.make_preferred();
        const std::string path_str = path_to_utf8(target);
        // Remembered so OnDownloadUpdated can report a stable destination:
        // while the transfer runs, Chromium's GetFullPath() is empty or points
        // at the ".crdownload" temporary, which made the Downloads page show a
        // blank or temporary name for the whole download.
        target_paths_[item->GetId()] = path_str;
        if (callback) callback->Continue(path_to_cef(target), false);
        webview::DownloadEvent ev;
        ev.id = item->GetId();
        ev.url = item->GetURL().ToString();
        ev.file_name = path_to_utf8(target.filename());
        ev.target_path = path_str;
        ev.mime = item->GetMimeType().ToString();
        ev.total_bytes = item->GetTotalBytes();
        ev.received = item->GetReceivedBytes();
        ev.state = 0;
        if (download_emit_) download_emit_(ev);
        return true;
    }

    void OnDownloadUpdated(CefRefPtr<CefBrowser> /*browser*/,
                           CefRefPtr<CefDownloadItem> item,
                           CefRefPtr<CefDownloadItemCallback> callback) override {
        // Interrupted transfers may resume on the same id; paused transfers
        // and pending Save As dialogs also remain protected until terminal.
        track_download(item->GetId(), !item->IsComplete() && !item->IsCanceled());
        // Rerouted .crx first: it never becomes a DownloadEvent. On success
        // the host gets the file to install; on cancel/interruption we just
        // clean up the temp copy.
        if (auto it = pending_crx_.find(item->GetId()); it != pending_crx_.end()) {
            if (item->IsComplete()) {
                if (extension_emit_) {
                    webview::ExtensionInstallRequest req;
                    req.tab = id_;
                    req.url = it->second.url;
                    req.file_name = it->second.file_name;
                    req.path = it->second.path;
                    extension_emit_(id_, req);
                }
            } else if (item->IsCanceled() || item->IsInterrupted()) {
                std::error_code ec;
                std::filesystem::remove(it->second.path, ec);
            } else {
                return;  // still downloading
            }
            track_download(item->GetId(), false);
            pending_crx_.erase(it);
            return;
        }
        // Remember (and refresh) this download's cancel handle so the host can
        // stop the transfer later; CEF only ever offers it here. A cancel the
        // host asked for before the first update lands now.
        const uint32_t download_id = item->GetId();
        if (callback) {
            g_download_callbacks[download_id] = callback;
            if (g_pending_cancels.erase(download_id) > 0) {
                callback->Cancel();
                // Cancel wins over any queued pause/resume: the transfer is
                // being torn down, and a Pause()/Resume() on it afterwards is
                // at best ignored by the engine and at worst undefined. The
                // terminal "cancelled" event below erases the rest.
                g_pending_pauses.erase(download_id);
                g_pending_resumes.erase(download_id);
            } else if (g_pending_pauses.erase(download_id) > 0) {
                // Pause/resume requests that raced the first update land here
                // (only one of the two can be queued: each erases the other).
                callback->Pause();
            } else if (g_pending_resumes.erase(download_id) > 0) {
                callback->Resume();
            }
        }
        webview::DownloadEvent ev;
        ev.id = download_id;
        ev.url = item->GetURL().ToString();
        ev.target_path = item->GetFullPath().ToString();
        if (ev.target_path.empty()) {
            // Not written yet (or still the .crdownload temporary): report the
            // destination we chose in OnBeforeDownload.
            const auto known = target_paths_.find(item->GetId());
            if (known != target_paths_.end()) ev.target_path = known->second;
        }
        ev.mime = item->GetMimeType().ToString();
        ev.total_bytes = item->GetTotalBytes();
        ev.received = item->GetReceivedBytes();
        // Pause state rides along every update, both ways: 1 while the engine
        // holds the transfer paused, 0 while it runs. The 0 is what unwinds a
        // stale paused row — e.g. our optimistic pause raced the engine
        // finishing the last bytes, or the engine cancelled a paused transfer
        // (the terminal event below then finishes the row).
        ev.paused = item->IsPaused() ? 1 : 0;
        if (item->IsComplete()) {
            ev.state = 1;
        } else if (item->IsCanceled()) {
            ev.state = 2;
        } else if (item->IsInterrupted()) {
            // Chromium interrupts a download for recoverable reasons too
            // (network hiccup, server drop) and resumes it by itself, firing
            // further updates afterwards. The event says "interrupted" so the
            // UI can show it, but the download is NOT torn down here — a
            // later Complete for the same id must be able to correct the row.
            // Log the precise interrupt reason so access-denied / path issues
            // are diagnosable from logs instead of a bare "interrupted".
            AURORA_LOG(util::LogLevel::Warn, "cef")
                << "download " << item->GetId() << " interrupted (reason "
                << item->GetInterruptReason() << ")";
            ev.state = 3;
        } else {
            ev.state = 0;
        }
        if (ev.state == 1 || ev.state == 2) {
            target_paths_.erase(download_id);
            // Terminal: the handle can no longer cancel/pause/resume anything,
            // and a stale pending entry would only leak.
            g_download_callbacks.erase(download_id);
            g_pending_cancels.erase(download_id);
            g_pending_pauses.erase(download_id);
            g_pending_resumes.erase(download_id);
        }
        if (download_emit_) download_emit_(ev);
    }

private:
    void audit_certificate(CefRefPtr<CefBrowser> browser, const CefString& url,
                           CefRefPtr<CefX509Certificate> certificate,
                           bool trusted) {
        if (closed_ || !browser || !g_cert_audit.load() || !g_certificate_audit)
            return;
        const auto origin = web_origin(url);
        if (!origin.starts_with("https://")) return;
        const auto fingerprint = certificate_fingerprint(certificate);
        const core::IncognitoPolicy policy{private_window_};
        const auto result = g_certificate_audit->observe(
            origin, fingerprint, policy, trusted);
        if (result == privacy::CertificateObservation::StorageError) {
            AURORA_LOG(util::LogLevel::Warn, "privacy")
                << "certificate audit storage unavailable for " << origin;
            return;
        }
        if (result != privacy::CertificateObservation::Changed) return;
        AURORA_LOG(util::LogLevel::Warn, "privacy")
            << "certificate fingerprint changed for " << origin
            << "; observed SHA-256=" << fingerprint;

        // Minimal audit alarm: it fires after navigation, not as HPKP or an
        // alternative TLS validator. No continue/accept action is automatic.
        const std::string html =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'\">"
            "<title>Certificate fingerprint changed</title></head><body>"
            "<h1>Certificate fingerprint changed</h1>"
            "<p>Aurora has previously seen a different TLS certificate for this origin. "
            "This can be a legitimate certificate renewal, or a sign of interception.</p>"
            "<p>The saved fingerprint has not been replaced. Verify the change with "
            "the site owner before disabling certificate auditing and retrying.</p>"
            "<p>Observed leaf-certificate SHA-256:</p><pre>" + fingerprint +
            "</pre></body></html>";
        const std::string warning_url = "data:text/html;base64," +
            CefBase64Encode(html.data(), html.size()).ToString();
        const auto generation = navigation_generation_;
        CefRefPtr<TabClient> self(this);
        CefPostTask(TID_UI, new AdblockTask([self, browser, generation, warning_url] {
            // A delayed warning must never overwrite a newer navigation.
            if (self->closed_ || self->navigation_generation_ != generation ||
                !browser->IsValid()) return;
            if (const auto frame = browser->GetMainFrame())
                frame->LoadURL(CefString(warning_url));
        }));
    }

    void apply_hidden(CefRefPtr<CefBrowser> browser) {
        if (browser && browser->GetHost())
            browser->GetHost()->WasHidden(!visible_ || suspended_);
    }

    bool visible_ = true;
    bool suspended_ = false;
    bool renderer_discarded_ = false;
    std::unordered_set<uint32_t> pending_downloads_;

    void track_download(uint32_t id, bool pending) {
        const bool changed = pending ? pending_downloads_.insert(id).second
                                     : pending_downloads_.erase(id) != 0;
        if (!changed) return;
        webview::PageInfo info;
        info.pending_downloads = static_cast<int>(pending_downloads_.size());
        emit_(id_, info);
    }

    enum ContextMenuCommand {
        kSaveImage = MENU_ID_USER_FIRST,
        kCopyImage,
        kCopyImageAddress,
        kOpenImageNewTab,
        kSaveVideo,
        kCopyVideoAddress,
        kOpenVideoNewTab,
        kCopyLinkAddress,
        kOpenLinkNewTab,
        kSearchSelection,
        kPageBack,
        kPageForward,
        kPageReload,
        kPageSavePdf,
        kPagePrint,
        kPageViewSource,
        kInspect,
    };

    bool take_force_save_dialog(const std::string& url) {
        const auto it = force_save_dialog_urls_.find(url);
        if (it == force_save_dialog_urls_.end()) return false;
        if (it->second <= 1) {
            force_save_dialog_urls_.erase(it);
        } else {
            --it->second;
        }
        return true;
    }

    // StartDownload is asynchronous. Remember the source URL for each explicit
    // Save Image As command so the matching OnBeforeDownload forces a dialog
    // even when the global ask-where preference is off.
    std::unordered_map<std::string, size_t> force_save_dialog_urls_;

    // Destination chosen in OnBeforeDownload, per CEF download id, so progress
    // updates can report it before the file exists on disk. Erased when the
    // download completes or is cancelled. CEF download callbacks all fire on
    // the UI thread, so no lock.
    std::unordered_map<uint32_t, std::string> target_paths_;

    // A .crx download rerouted away from the Downloads folder, remembered by
    // CEF download id until it completes (handed to the host) or fails
    // (deleted). CEF download callbacks fire on the UI thread, so no lock.
    struct PendingCrx {
        std::string path;       // temp file the bytes go to
        std::string url;        // where the .crx came from
        std::string file_name;  // suggested name, for dialogs
    };

    TabId id_;
    PageInfoEmitter emit_;
    CloseEmitter close_emit_;
    NewTabEmitter new_tab_emit_;
    std::string downloads_dir_;
    AskWhereProvider ask_where_provider_;
    DownloadsDirProvider downloads_dir_provider_;
    SavePathChooser save_path_chooser_;
    DownloadEmitter download_emit_;
    FullscreenEmitter fullscreen_emit_;
    KeyEmitter key_emit_;
    ExtensionEmitter extension_emit_;
    PermissionEmitter permission_emit_;
    ContextActionEmitter context_action_emit_;
    PopupShimProvider popup_shim_provider_;
    bool private_window_ = false;
    bool closed_ = false;
    uint64_t navigation_generation_ = 0;
    std::unordered_map<uint32_t, PendingCrx> pending_crx_;
    IMPLEMENT_REFCOUNTING(TabClient);
};

CefRefPtr<CefBrowser> create_browser(TabId tab, std::string_view url,
                                      void* native_parent,
                                      bool preferred_dark,
                                      const CefAdapter::PageInfoCallback& cb,
                                      const TabClient::CloseEmitter& on_close,
                                      const TabClient::NewTabEmitter& on_new_tab,
                                      std::string downloads_dir,
                                      AskWhereProvider ask_where_provider,
                                      DownloadsDirProvider downloads_dir_provider,
                                      SavePathChooser save_path_chooser,
                                      const TabClient::DownloadEmitter& on_download,
                                      const TabClient::FullscreenEmitter& on_fullscreen,
                                      const TabClient::KeyEmitter& on_key,
                                      const TabClient::ExtensionEmitter& on_extension,
                                      const TabClient::PermissionEmitter& on_permission,
                                      const TabClient::ContextActionEmitter& on_context_action,
                                      const TabClient::PopupShimProvider& popup_shim,
                                      CefRefPtr<CefRequestContext> private_context) {
    CefWindowInfo info;
#if defined(OS_WIN)
    info.SetAsChild(static_cast<HWND>(native_parent), CefRect(0, 0, 0, 0));
#elif defined(OS_LINUX)
    // On Linux CefWindowHandle is an X11 window id, not a widget pointer; the
    // host widget passes Qt's winId() through as an opaque void*.
    info.SetAsChild(reinterpret_cast<CefWindowHandle>(native_parent),
                    CefRect(0, 0, 0, 0));
#elif defined(OS_MAC)
    // On macOS parenting is done via NSView: QWidget::winId() is the NSView*
    // and CefWindowHandle is an opaque void* to it, so no Objective-C is
    // needed here. The view is fitted to the parent once CEF creates it (see
    // resize_native_child).
    info.SetAsChild(static_cast<CefWindowHandle>(native_parent),
                    CefRect(0, 0, 0, 0));
#endif
    // Alloy style, explicitly. This is the single most consequential line in
    // the CEF layer, so it is worth spelling out why. CEF hosts a browser in
    // one of two styles, and the default for a client-provided parent window is
    // Chrome style, which routes browser behaviour through Chrome's own UI
    // layer. Under Chrome style:
    //   * CefLifeSpanHandler::DoClose is never called, so closing a tab falls
    //     back to "send the close notification to the top-level window" — the
    //     Qt window — which closed the entire browser whenever one tab of many
    //     was closed.
    //   * downloads are handed to Chrome's download shelf, which does not exist
    //     inside an embedded child window, so nothing was ever saved.
    //   * popups are opened by Chrome as its own windows.
    // Alloy style keeps the same Chromium engine (extensions still load through
    // the profile) but gives the embedder the client callbacks: DoClose, the
    // download handler, OnBeforePopup, and client-driven fullscreen.
    info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    // Explicitly enable JavaScript for all browser instances, including
    // extension popup pages that may inherit a disabled state from
    // command-line switches.
    settings.javascript = STATE_ENABLED;
    // Pass the request context explicitly after applying the host's desired
    // color scheme. SetChromeColorScheme updates Blink's prefers-color-scheme
    // value and also performs the renderer notification needed for live pages.
    //
    // When `private_context` is set the adapter is serving an incognito window
    // and must never touch the global (regular-profile) context. The private
    // context has an empty cache_path so Chromium keeps it purely in memory,
    // and CefAdapter holds the only strong reference, so it dies with the
    // adapter (i.e. with the private window).
    auto request_context = private_context ? private_context
                                         : CefRequestContext::GetGlobalContext();
    if (request_context) {
        request_context->SetChromeColorScheme(
            preferred_dark ? CEF_COLOR_VARIANT_DARK : CEF_COLOR_VARIANT_LIGHT,
            /*user_color=*/0);
    }
    auto client = CefRefPtr<TabClient>(new TabClient(
        tab,
        [cb](TabId id, const webview::PageInfo& pi) {
            if (cb) cb(id, pi);
        },
        on_close,
        on_new_tab,
        std::move(downloads_dir),
        std::move(ask_where_provider),
        std::move(downloads_dir_provider),
        std::move(save_path_chooser),
        on_download,
        on_fullscreen,
        on_key,
        on_extension,
        on_permission,
        on_context_action,
        popup_shim, private_context != nullptr));
    return CefBrowserHost::CreateBrowserSync(
        info, client, CefString(core::upgrade_http_url(url, g_https_only.load())),
        settings, nullptr,
        request_context);
}

// Minimal client for a docked DevTools browser. ShowDevTools is async — the
// DevTools CefBrowser is handed to us in OnAfterCreated, which we forward to
// CefAdapter so it can resize/close the DevTools window. OnBeforeClose clears
// it. A session id guards the closures so a stale close (from a previous
// DevTools browser we replaced) cannot clobber the current one.
class DevToolsClient : public CefClient,
                      public CefLifeSpanHandler {
public:
    using CreatedCb = std::function<void(CefRefPtr<CefBrowser>)>;
    using ClosedCb = std::function<void(uint64_t session)>;

    DevToolsClient(uint64_t session, CreatedCb on_created, ClosedCb on_closed)
        : session_(session),
          on_created_(std::move(on_created)),
          on_closed_(std::move(on_closed)) {}

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        if (on_created_) on_created_(browser);
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) override {
        if (on_closed_) on_closed_(session_);
    }

private:
    uint64_t session_;
    CreatedCb on_created_;
    ClosedCb on_closed_;
    IMPLEMENT_REFCOUNTING(DevToolsClient);
};

} // namespace

namespace {

// Chromium's own third-party cookie switch, the second half of "Block
// third-party cookies" (the first is TabClient::GetCookieAccessFilter).
//
// Why both: CefCookieAccessFilter only sees cookies travelling in HTTP
// headers of resource requests. Script access from a third-party iframe
// (document.cookie, the CookieStore API) and service-worker requests go
// through Chromium's RestrictedCookieManager / network-service CookieSettings
// instead, which consult this profile preference and nothing in the CEF API.
//
// What is and is not verified:
//   - The API is real: CefRequestContext inherits CefPreferenceManager
//     (include/cef_preference.h: CanSetPreference / SetPreference /
//     GetPreference, UI thread only; a null value restores the default).
//   - The NAME is a Chromium pref, so it is not declared in any CEF header:
//     prefs::kCookieControlsMode = "profile.cookie_controls_mode"
//     (components/content_settings/core/common/pref_names.h), an int holding
//     content_settings::CookieControlsMode {kOff = 0, kBlockThirdParty = 1,
//     kIncognitoOnly = 2 (the registered default)}; CookieSettings observes it
//     and blocks third-party cookies when it is kBlockThirdParty.
//   - Whether this CEF build registers it is checked at runtime with
//     CanSetPreference, and a successful write is read back with
//     GetPreference, so a silent rejection is logged rather than assumed to
//     have worked. Without a running CEF build that is the only evidence.
//
// Only the global (persistent) context is written. Private windows use an
// in-memory "incognito mode" context, where Chromium's CookieSettings blocks
// third-party cookies unconditionally (is_incognito_), whatever the pref says.
//
// Turning the setting OFF restores the registered default instead of writing
// kOff, so Aurora leaves no trace of its own in the profile's preferences.
void apply_cookie_controls_preference(bool block_3p) {
    static constexpr const char* kCookieControlsPref =
        "profile.cookie_controls_mode";
    static constexpr int kBlockThirdParty = 1;
    // -1: not applied yet (or must be retried). Only advanced once the
    // outcome is definitive, so a call that lands before CEF has a global
    // context is retried on the next 250 ms tick instead of being lost.
    static int applied = -1;
    static bool reported_unavailable = false;
    const int wanted = block_3p ? 1 : 0;
    if (wanted == applied || !CefCurrentlyOn(TID_UI)) return;
    const auto context = CefRequestContext::GetGlobalContext();
    if (!context) return;  // engine not up yet: retry later
    if (!context->CanSetPreference(kCookieControlsPref)) {
        // Not registered in this build, or policy/command-line managed. The
        // header filter still applies; say so once instead of failing silent.
        if (!reported_unavailable) {
            AURORA_LOG(util::LogLevel::Warn, "privacy")
                << kCookieControlsPref
                << " is not settable in this CEF build; third-party cookies "
                   "are blocked in HTTP headers only (not document.cookie)";
            reported_unavailable = true;
        }
        applied = wanted;
        return;
    }
    CefRefPtr<CefValue> value;  // null: restore Chromium's default
    if (block_3p) {
        value = CefValue::Create();
        value->SetInt(kBlockThirdParty);
    }
    CefString error;
    if (!context->SetPreference(kCookieControlsPref, value, error)) {
        AURORA_LOG(util::LogLevel::Warn, "privacy")
            << "could not set " << kCookieControlsPref << ": "
            << error.ToString();
        applied = wanted;  // definitive failure; do not hammer every tick
        return;
    }
    if (block_3p) {
        const auto read_back = context->GetPreference(kCookieControlsPref);
        const bool took = read_back && read_back->GetType() == VTYPE_INT &&
                          read_back->GetInt() == kBlockThirdParty;
        if (!took) {
            AURORA_LOG(util::LogLevel::Warn, "privacy")
                << kCookieControlsPref
                << " was accepted but did not read back as kBlockThirdParty";
        }
    }
    AURORA_LOG(util::LogLevel::Info, "privacy")
        << kCookieControlsPref << (block_3p ? " = kBlockThirdParty" : " reset to default");
    applied = wanted;
}

} // namespace

void refresh_privacy_preferences() {
    const auto read = [](std::string_view key, bool fallback) {
        return g_privacy_prefs ? g_privacy_prefs->get_bool(key, fallback) : fallback;
    };
    g_https_only.store(read(settings::PrefKeys::kHttpsOnly,
                           settings::PrefDefaults::kHttpsOnly));
    g_referer_trim.store(read(settings::PrefKeys::kRefererTrim,
                             settings::PrefDefaults::kRefererTrim));
    g_cert_audit.store(read(settings::PrefKeys::kCertAudit,
                           settings::PrefDefaults::kCertAudit));
    g_send_dnt.store(read(settings::PrefKeys::kSendDoNotTrack,
                          settings::PrefDefaults::kSendDoNotTrack));
    const bool block_3p = read(settings::PrefKeys::kBlockThirdPartyCookies,
                               settings::PrefDefaults::kBlockThirdPartyCookies);
    g_block_3p_cookies.store(block_3p);

    apply_cookie_controls_preference(block_3p);
}

void configure_privacy_preferences(settings::Settings* prefs,
                                   privacy::CertificateAudit* audit) {
    g_privacy_prefs = prefs;
    g_certificate_audit = audit;
    refresh_privacy_preferences();
}

bool clear_profile_cookies() {
    const auto context = CefRequestContext::GetGlobalContext();
    const auto manager = context ? context->GetCookieManager(nullptr) : nullptr;
    if (!manager) return false;
    const auto state = std::make_shared<CookieClearState>();
    // Empty URL and name clear every cookie (session and persistent).
    if (!manager->DeleteCookies(CefString(), CefString(),
                                new CookieDeleteCallback(manager, state)))
        return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!state->done.load() && std::chrono::steady_clock::now() < deadline) {
        pump_message_loop();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return state->done.load() && state->success.load();
}

void configure_download_preferences(AskWhereProvider ask_where_provider,
                                    DownloadsDirProvider downloads_dir_provider,
                                    SavePathChooser save_path_chooser) {
    g_ask_where_provider = std::move(ask_where_provider);
    g_downloads_dir_provider = std::move(downloads_dir_provider);
    g_save_path_chooser = std::move(save_path_chooser);
}

CefAdapter::~CefAdapter() {
    // If shutdown() ran (normal exit), every browser is already closed and
    // CefShutdown() has been called by the host — we must NOT touch CEF again.
    // If shutdown() never ran (abnormal exit), we cannot pump the message loop
    // here (no event loop) and must not call CefShutdown either; just release
    // our CefRefPtrs so the browser hosts can be torn down by whomever still
    // holds CEF. Either way: drop refs, never call CloseBrowser after
    // CefShutdown.
    std::lock_guard<std::mutex> lock(browsers_mutex_);
    for (auto& [id, browser] : browsers_) {
        (void)id;
        browser = nullptr;
    }
    browsers_.clear();
    closing_tabs_.clear();
}

CefRefPtr<CefBrowser> CefAdapter::browser_for(TabId tab) {
    std::lock_guard<std::mutex> lock(browsers_mutex_);
    auto it = browsers_.find(tab);
    if (it == browsers_.end()) return nullptr;
    return it->second;
}

void CefAdapter::emit(TabId tab, const webview::PageInfo& info) {
    // A non-empty url is only ever set by OnAddressChange for the MAIN frame
    // (UI thread): the commit point where a per-site zoom has to be applied,
    // before the host sees the new address.
    if (!info.url.empty()) apply_site_zoom(tab, info.url);
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (callback_) callback_(tab, info);
}

void CefAdapter::on_browser_closed(TabId tab) {
    {
        std::lock_guard<std::mutex> lock(browsers_mutex_);
        browsers_.erase(tab);
        closing_tabs_.erase(tab);
    }
    // Tell the host the close is complete so it can finally delete the widget
    // that owned the native parent window. Done outside the lock: the host
    // deletes Qt objects here and must not run inside our mutex.
    TabClosedCallback cb;
    {
        std::lock_guard<std::mutex> lock(tab_closed_mutex_);
        cb = tab_closed_callback_;
    }
    if (cb) cb(tab);
}

void CefAdapter::set_tab_closed_callback(TabClosedCallback cb) {
    std::lock_guard<std::mutex> lock(tab_closed_mutex_);
    tab_closed_callback_ = std::move(cb);
}

void CefAdapter::set_fullscreen_callback(FullscreenCallback cb) {
    std::lock_guard<std::mutex> lock(fullscreen_mutex_);
    fullscreen_callback_ = std::move(cb);
}

void CefAdapter::on_fullscreen_change(TabId tab, bool fullscreen) {
    FullscreenCallback cb;
    {
        std::lock_guard<std::mutex> lock(fullscreen_mutex_);
        cb = fullscreen_callback_;
    }
    if (cb) cb(tab, fullscreen);
}

void CefAdapter::exit_fullscreen(TabId tab) {
    auto b = browser_for(tab);
    if (!b.get()) return;
    auto host = b->GetHost();
    if (host.get()) host->ExitFullscreen(true);
}

void CefAdapter::create(TabId tab, std::string_view initial_url,
                        void* native_parent) {
    {
        std::lock_guard<std::mutex> lock(browsers_mutex_);
        // A suspended tab reuses its TabId. Its replacement is created only
        // after the old browser's OnBeforeClose removes closing_tabs_; refuse
        // any accidental early/duplicate request rather than overwrite a live
        // CefBrowser handle and strand its renderer.
        if (browsers_.find(tab) != browsers_.end() ||
            closing_tabs_.find(tab) != closing_tabs_.end()) {
            AURORA_LOG(util::LogLevel::Warn, "cef")
                << "ignored duplicate/early browser create for tab "
                << tab.value();
            return;
        }
    }
    std::string dl_dir;
    webview::WebViewController::DownloadCallback dl_cb;
    {
        std::lock_guard<std::mutex> lock(downloads_mutex_);
        dl_dir = downloads_dir_;
        dl_cb = download_callback_;
    }
    auto browser = create_browser(tab, initial_url, native_parent,
        preferred_dark_.load(),
        [this](TabId id, const webview::PageInfo& pi) { emit(id, pi); },
        [this](TabId id) { on_browser_closed(id); },
        [this](std::string url) { return open_in_new_tab(std::move(url)); },
        std::move(dl_dir),
        g_ask_where_provider,
        g_downloads_dir_provider,
        g_save_path_chooser,
        [dl_cb](const webview::DownloadEvent& ev) {
            if (dl_cb) dl_cb(ev);
        },
        [this](TabId id, bool fullscreen) { on_fullscreen_change(id, fullscreen); },
        [this](TabId id, const webview::EngineKeyEvent& ev) {
            return forward_key_event(id, ev);
        },
        [this](TabId id, const webview::ExtensionInstallRequest& req) {
            on_extension_downloaded(id, req);
        },
        [this](webview::PermissionRequest req) {
            on_permission_request(std::move(req));
        },
        [this](TabId id, webview::WebViewController::ContextActionRequest req) {
            on_context_action(id, std::move(req));
        },
        [this](TabId popup, std::string_view bridge_request) {
            return extension_popup_script(popup, bridge_request);
        },
        private_context_);
    if (!browser) {
        // CreateBrowserSync failure has no OnBeforeClose callback. Drop any
        // popup association now and do not leave a null browser entry that
        // shutdown() would otherwise wait on.
        std::lock_guard<std::mutex> lock(extension_popup_mutex_);
        extension_popup_owners_.erase(tab);
        return;
    }
    std::lock_guard<std::mutex> lock(browsers_mutex_);
    browsers_[tab] = browser;
}

void CefAdapter::set_preferred_color_scheme(TabId tab, bool dark) {
    (void)tab;
    preferred_dark_.store(dark);

    // All Aurora tab browsers currently share the global request context. Use
    // an existing browser to obtain it so an initial call made before create()
    // only records state and does not touch CEF before a context exists.
    CefRefPtr<CefRequestContext> request_context;
    {
        std::lock_guard<std::mutex> lock(browsers_mutex_);
        if (!browsers_.empty()) {
            const auto& browser = browsers_.begin()->second;
            if (browser && browser->GetHost()) {
                request_context = browser->GetHost()->GetRequestContext();
            }
        }
    }
    if (request_context) {
        request_context->SetChromeColorScheme(
            dark ? CEF_COLOR_VARIANT_DARK : CEF_COLOR_VARIANT_LIGHT,
            /*user_color=*/0);
    }
}

void CefAdapter::destroy(TabId tab) {
    {
        // Also handles a popup whose synchronous browser creation failed.
        std::lock_guard<std::mutex> lock(extension_popup_mutex_);
        extension_popup_owners_.erase(tab);
    }
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(browsers_mutex_);
        auto it = browsers_.find(tab);
        if (it == browsers_.end()) return;
        browser = it->second;
        browsers_.erase(it);
        // A real browser is not closed yet — CloseBrowser is async and
        // OnBeforeClose fires later. Track only real browsers: synchronous
        // creation failure leaves a null map entry and no OnBeforeClose can
        // ever arrive to clear it.
        if (browser) closing_tabs_.insert(tab);
    }
    if (browser.get()) {
        // CloseBrowser is asynchronous: the renderer takes a few seconds to
        // tear the page down, and a playing video keeps being heard for that
        // whole time. Silence it at the audio mixer first (instant, browser
        // process side), stop any load, and stop rendering, then close.
        const CefRefPtr<CefBrowserHost> host = browser->GetHost();
        host->SetAudioMuted(true);
        browser->StopLoad();
        host->WasHidden(true);
        host->CloseBrowser(true);
    }
}

void CefAdapter::set_visible(TabId tab, bool visible) {
    if (auto browser = browser_for(tab); browser && browser->GetHost()) {
        const auto client = browser->GetHost()->GetClient();
        static_cast<TabClient*>(client.get())->set_visible(browser, visible);
    }
}

void CefAdapter::set_suspended(TabId tab, bool suspended) {
    if (auto browser = browser_for(tab); browser && browser->GetHost()) {
        // Only TabClient browsers are registered in browsers_ (DevTools has its
        // own handle and never goes through the tab controller).
        const auto client = browser->GetHost()->GetClient();
        static_cast<TabClient*>(client.get())->set_suspended(browser, suspended);
    }
}

void CefAdapter::focus(TabId tab) {
    // SetFocus(true) both moves native focus to the browser's child window
    // and tells the renderer it is focused. Without this an extension popup
    // hosted in a never-activated Qt::Tool window renders but ignores all
    // clicks and keys — no other code path ever focuses a synthetic popup's
    // CEF child (a real tab gets focused by the user clicking the page inside
    // an already-active main window).
    const auto b = browser_for(tab);
    if (!b.get() || !b->IsValid()) return;
    const auto host = b->GetHost();
    if (host.get()) host->SetFocus(true);
}

namespace {

// Move CEF's child window to fill (0,0,w,h) of its parent. Sizes are in
// *device* pixels: native windows know nothing about Qt's logical coordinate
// system, which is why the caller scales by the device pixel ratio. Getting
// this wrong is what made pages render at a fraction of the window on a
// scaled display, and what left a shrunken page behind after leaving
// fullscreen.
void resize_native_child(CefRefPtr<CefBrowserHost> host, int width, int height) {
    if (!host.get()) return;
#if defined(OS_WIN)
    CefWindowHandle hwnd = host->GetWindowHandle();
    if (hwnd) {
        SetWindowPos(hwnd, nullptr, 0, 0, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
#elif defined(OS_LINUX)
    CefWindowHandle window = host->GetWindowHandle();
    XDisplay* display = cef_get_xdisplay();
    if (window && display) {
        XMoveResizeWindow(display, window, 0, 0,
                          static_cast<unsigned int>(width),
                          static_cast<unsigned int>(height));
        XFlush(display);
    }
#elif defined(OS_MAC)
    // NSView frames are in points, not the device pixels passed in; fit the
    // browser view to its parent instead (AppKit then keeps it sized).
    (void)width;
    (void)height;
    mac_fit_child_view(host->GetWindowHandle());
#endif
    host->WasResized();
}

} // namespace

void CefAdapter::resize(TabId tab, int width, int height) {
    if (width <= 0 || height <= 0) return;
    auto b = browser_for(tab);
    if (!b.get()) return;
    resize_native_child(b->GetHost(), width, height);
}

void CefAdapter::load(TabId tab, const core::Url& url) {
    if (auto b = browser_for(tab); b.get()) {
        refresh_privacy_preferences();
        b->GetMainFrame()->LoadURL(CefString(
            core::resolve_navigation(url, g_https_only.load())));
    }
}

void CefAdapter::back(TabId tab) {
    if (auto b = browser_for(tab); b.get()) b->GoBack();
}
void CefAdapter::forward(TabId tab) {
    if (auto b = browser_for(tab); b.get()) b->GoForward();
}
void CefAdapter::reload(TabId tab, bool bypass_cache) {
    if (auto b = browser_for(tab); b.get()) {
        if (bypass_cache) b->ReloadIgnoreCache();
        else b->Reload();
    }
}
void CefAdapter::stop(TabId tab) {
    if (auto b = browser_for(tab); b.get()) b->StopLoad();
}

void CefAdapter::find(TabId tab, std::string_view query, bool forward) {
    if (auto b = browser_for(tab); b.get()) {
        b->GetHost()->Find(CefString(std::string(query)), forward, false, true);
    }
}
void CefAdapter::stop_find(TabId tab) {
    if (auto b = browser_for(tab); b.get()) b->GetHost()->StopFinding(true);
}
void CefAdapter::zoom(TabId tab, int delta) {
    if (auto b = browser_for(tab); b.get()) {
        double level = b->GetHost()->GetZoomLevel();
        level += delta * 0.1;
        b->GetHost()->SetZoomLevel(level);
        // Remember it for the page's origin (not the tab): the next visit to
        // the site, in any tab, comes back at the same size. A level that
        // lands on 100% clears the entry (SiteZoomStore drops defaults).
        ZoomStore store;
        {
            std::lock_guard<std::mutex> lock(zoom_mutex_);
            store = zoom_store_;
        }
        const auto origin = b->GetMainFrame()
                                ? web_origin(b->GetMainFrame()->GetURL())
                                : std::string();
        if (store.save && !origin.empty()) store.save(origin, level);
    }
}
void CefAdapter::zoom_reset(TabId tab) {
    if (auto b = browser_for(tab); b.get()) {
        b->GetHost()->SetZoomLevel(0.0);
        // "Reset" means "this site has no custom zoom", so forget it too —
        // otherwise the next navigation would bring the old level back.
        ZoomStore store;
        {
            std::lock_guard<std::mutex> lock(zoom_mutex_);
            store = zoom_store_;
        }
        const auto origin = b->GetMainFrame()
                                ? web_origin(b->GetMainFrame()->GetURL())
                                : std::string();
        if (store.clear && !origin.empty()) store.clear(origin);
    }
}

void CefAdapter::set_zoom_store(ZoomStore store) {
    std::lock_guard<std::mutex> lock(zoom_mutex_);
    zoom_store_ = std::move(store);
}

void CefAdapter::apply_site_zoom(TabId tab, const std::string& url) {
    ZoomStore store;
    {
        std::lock_guard<std::mutex> lock(zoom_mutex_);
        store = zoom_store_;
    }
    if (!store.load) return;  // no persistence: leave Chromium's behaviour
    auto b = browser_for(tab);
    if (!b.get() || !b->GetHost()) return;
    const std::string origin = web_origin(CefString(url));
    // Pages without a web origin (about:, data:, chrome-extension:) keep the
    // tab's current zoom rather than being snapped to a stored default.
    if (origin.empty()) return;
    double level = 0.0;
    if (!store.load(origin, level)) level = 0.0;
    // Compare before setting: pushState/fragment changes report an address
    // change too, and re-setting an identical level would still make
    // Chromium relayout the page.
    if (std::abs(b->GetHost()->GetZoomLevel() - level) > 1e-6)
        b->GetHost()->SetZoomLevel(level);
}

bool CefAdapter::print(TabId tab) {
    auto b = browser_for(tab);
    if (!b.get() || !b->GetHost()) return false;
    // Windows and macOS show the platform print dialog. On Linux, Aurora
    // deliberately does NOT implement CefClient::GetPrintHandler: in CEF 156
    // (branch 8078, libcef/browser/printing/print_dialog_linux.cc) a browser
    // without a CefPrintHandler falls back to Chromium's own Linux dialog
    // selection — the XDG desktop print portal, else the GTK LinuxUi dialog —
    // which prints to real printers. Returning a handler would REPLACE that
    // with an Aurora-drawn dialog plus Aurora-side job submission (CUPS/GTK),
    // i.e. new third-party dependencies for a worse result. The comment on
    // CefClient::GetPrintHandler ("printing will not be supported") predates
    // that fallback. If the desktop has neither a portal nor GTK, Chromium
    // logs "Null interface in CreatePrintDialog" and nothing is shown; the
    // host tells the user about Save as PDF for that case.
    b->GetHost()->Print();
    return true;
}

bool CefAdapter::print_to_pdf(TabId tab, std::string_view path,
                              PdfCallback done) {
    auto b = browser_for(tab);
    if (!b.get() || !b->GetHost() || path.empty()) return false;
    // CEF defaults (US Letter, default margins, no background graphics) are
    // what Chrome's Save as PDF starts with; Aurora has no options UI yet.
    // Linux needs no CefPrintHandler for this either: CEF 156 implements
    // PrintToPDF on the DevTools Page.printToPDF path
    // (libcef/browser/printing/print_util.cc), which takes the paper size from
    // these settings (Letter when zero) and never calls
    // CefPrintHandler::GetPdfPaperSize, despite what the PrintToPDF comment in
    // cef_browser.h still says.
    CefPdfPrintSettings settings;
    b->GetHost()->PrintToPDF(CefString(std::string(path)), settings,
                             new PdfPrintCallback(std::move(done)));
    return true;
}
void CefAdapter::open_devtools_docked(TabId tab, void* native_parent,
                                      int w, int h) {
    // One DevTools browser at a time: if one is already live, close it first.
    // Bump the session so the old browser's async OnAfterCreated/OnBeforeClose
    // closures become stale no-ops and cannot touch devtools_browser_.
    CefRefPtr<CefBrowser> old;
    {
        std::lock_guard<std::mutex> lock(devtools_mutex_);
        old = devtools_browser_;
        ++devtools_session_;
        devtools_browser_ = nullptr;
        devtools_tab_ = tab;
        if (old.get()) {
            // We are about to drop our ref and tell CEF to close it; track it so
            // shutdown() (if it races) still waits for the real close.
            devtools_pending_.fetch_add(1);
        }
    }
    if (old.get() && old->GetHost().get()) {
        old->GetHost()->CloseBrowser(true);
    }

    auto b = browser_for(tab);
    if (!b.get() || !native_parent) return;

    CefWindowInfo wi;
#if defined(OS_WIN)
    wi.SetAsChild(static_cast<HWND>(native_parent), CefRect(0, 0, w, h));
#elif defined(OS_LINUX)
    wi.SetAsChild(reinterpret_cast<CefWindowHandle>(native_parent),
                  CefRect(0, 0, w, h));
#elif defined(OS_MAC)
    wi.SetAsChild(static_cast<CefWindowHandle>(native_parent),
                  CefRect(0, 0, w, h));
#endif
    // Same reasoning as the tab browsers: Alloy style so the docked DevTools
    // window behaves as an embedded child and its close cannot reach the Qt
    // window. See create_browser().
    wi.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings bs;
    const uint64_t session = [&] {
        std::lock_guard<std::mutex> lock(devtools_mutex_);
        return devtools_session_;
    }();
    auto client = CefRefPtr<DevToolsClient>(new DevToolsClient(
        session,
        [this, session](CefRefPtr<CefBrowser> db) {
            std::lock_guard<std::mutex> lock(devtools_mutex_);
            // Only adopt the browser if our session is still current — a stale
            // OnAfterCreated from the DevTools browser we just replaced must
            // not overwrite the new one.
            if (session == devtools_session_) devtools_browser_ = db;
        },
        [this, session](uint64_t closing_session) {
            bool notify = false;
            {
                std::lock_guard<std::mutex> lock(devtools_mutex_);
                // Always account for the pending close we tracked when we called
                // CloseBrowser — but only clear the live DevTools browser if this
                // close belongs to the current session. A stale OnBeforeClose from
                // a replaced DevTools browser must not null out the new one (which
                // would leave an orphaned CEF browser and a blank dock).
                if (closing_session == devtools_session_) {
                    devtools_browser_ = nullptr;
                    devtools_tab_ = core::TabId{0};
                    notify = true;
                }
                if (devtools_pending_.load() > 0) devtools_pending_.fetch_sub(1);
            }
            // Notify the host that the DevTools browser has closed so it can
            // hide the dock panel. Without this, closing DevTools from inside
            // its own panel leaves an empty dock visible.
            if (notify) {
                DevToolsClosedCallback cb;
                {
                    std::lock_guard<std::mutex> lock(devtools_closed_mutex_);
                    cb = devtools_closed_callback_;
                }
                if (cb) cb();
            }
        }));
    b->GetHost()->ShowDevTools(wi, client, bs, CefPoint(0, 0));
}

void CefAdapter::resize_devtools(TabId tab, int w, int h) {
    if (w <= 0 || h <= 0) return;
    CefRefPtr<CefBrowser> b;
    {
        std::lock_guard<std::mutex> lock(devtools_mutex_);
        if (devtools_tab_ != tab) return;
        b = devtools_browser_;
    }
    if (!b.get()) return;
    resize_native_child(b->GetHost(), w, h);
}

void CefAdapter::close_devtools(TabId tab) {
    CefRefPtr<CefBrowser> old;
    {
        std::lock_guard<std::mutex> lock(devtools_mutex_);
        if (devtools_tab_ != tab) return;
        old = devtools_browser_;
        // Stale this session's closures so the async OnBeforeClose of the
        // browser we're closing cannot clobber a future DevTools browser.
        ++devtools_session_;
        devtools_browser_ = nullptr;
        if (old.get()) devtools_pending_.fetch_add(1);
    }
    // CloseBrowser can re-enter CEF on the UI thread; call it outside the lock.
    if (old.get() && old->GetHost().get()) {
        old->GetHost()->CloseBrowser(true);
    }
}

void CefAdapter::set_devtools_closed_callback(DevToolsClosedCallback cb) {
    std::lock_guard<std::mutex> lock(devtools_closed_mutex_);
    devtools_closed_callback_ = std::move(cb);
}

void CefAdapter::set_muted(TabId tab, bool muted) {
    if (auto b = browser_for(tab); b.get()) {
        b->GetHost()->SetAudioMuted(muted);
    }
}

void CefAdapter::request_memory(TabId tab) {
    // The renderer answers with "aurora.memory.reply" (see AuroraApp in
    // CefLifecycle.cpp); TabClient::OnProcessMessageReceived turns that into
    // PageInfo::memory_bytes.
    auto b = browser_for(tab);
    if (!b.get()) return;
    CefRefPtr<CefFrame> frame = b->GetMainFrame();
    if (!frame.get()) return;
    frame->SendProcessMessage(PID_RENDERER,
                              CefProcessMessage::Create("aurora.memory.request"));
}

void CefAdapter::set_new_tab_callback(NewTabCallback cb) {
    std::lock_guard<std::mutex> lock(new_tab_mutex_);
    new_tab_callback_ = std::move(cb);
}

bool CefAdapter::set_private_context(bool on) {
    std::lock_guard<std::mutex> lock(private_context_mutex_);
    if (!on) {
        private_context_ = nullptr;
        return true;
    }
    if (private_context_.get()) return true;  // idempotent
    // An empty cache_path makes CEF keep the context entirely in memory: no
    // disk cache, no cookie store on disk, no service-worker storage. The
    // last CefRefPtr is this one, so the context (and everything Chromium
    // stored in it) dies when the adapter does — i.e. with the private window.
    CefRequestContextSettings settings;
    CefString(&settings.cache_path) = std::string();
    private_context_ = CefRequestContext::CreateContext(settings, nullptr);
    if (!private_context_.get()) {
        AURORA_LOG(util::LogLevel::Error, "cef")
            << "failed to create private request context; refusing private mode";
        return false;
    }
    AURORA_LOG(util::LogLevel::Info, "cef")
        << "private request context created (in-memory)";
    return true;
}

void CefAdapter::set_downloads_dir(std::string_view dir) {
    std::lock_guard<std::mutex> lock(downloads_mutex_);
    downloads_dir_ = std::string(dir);
}

void CefAdapter::set_download_callback(DownloadCallback cb) {
    std::lock_guard<std::mutex> lock(downloads_mutex_);
    download_callback_ = std::move(cb);
}

bool CefAdapter::cancel_download(uint32_t download_id) {
    // Called from the host's UI thread, which is the CEF UI thread here
    // (multi_threaded_message_loop is off), so the registry needs no lock.
    const auto it = g_download_callbacks.find(download_id);
    if (it != g_download_callbacks.end() && it->second) {
        it->second->Cancel();
        // Cancel outranks a queued pause/resume for the same transfer: drop
        // them so the first update (if it is still owed) does not fire them
        // at a transfer already being torn down.
        g_pending_pauses.erase(download_id);
        g_pending_resumes.erase(download_id);
        // The entry stays until CEF reports the terminal cancelled state:
        // Cancel() is asynchronous, and a second click must not fall through
        // to the pending path and queue a cancel for an already-dead id.
        return true;
    }
    // CEF has not handed us a cancel handle for this download yet (the request
    // can land between OnBeforeDownload and the first OnDownloadUpdated). Queue
    // it: the first update for this id cancels immediately. Nothing is lost, so
    // the request is accepted either way.
    g_pending_cancels.insert(download_id);
    return true;
}

bool CefAdapter::pause_download(uint32_t download_id) {
    // Same thread reasoning as cancel_download.
    const auto it = g_download_callbacks.find(download_id);
    if (it != g_download_callbacks.end() && it->second) {
        it->second->Pause();
        return true;
    }
    g_pending_pauses.insert(download_id);
    // A queued resume for the same id would now fight the pause; the host
    // only offers one of the two, but keep the queue self-consistent anyway.
    g_pending_resumes.erase(download_id);
    return true;
}

bool CefAdapter::resume_download(uint32_t download_id) {
    const auto it = g_download_callbacks.find(download_id);
    if (it != g_download_callbacks.end() && it->second) {
        it->second->Resume();
        return true;
    }
    g_pending_resumes.insert(download_id);
    g_pending_pauses.erase(download_id);
    return true;
}

bool CefAdapter::start_download(std::string_view url) {
    // The retry path: a failed/cancelled row is retried by re-requesting its
    // stored URL. CefBrowserHost::StartDownload routes the transfer through
    // the normal CefDownloadHandler flow (OnBeforeDownload + updates), so it
    // lands in DownloadService like any fresh download — the old row is the
    // host's business, not ours. Runs on the CEF UI thread like every host
    // call.
    if (url.empty()) return false;
    std::lock_guard<std::mutex> lock(browsers_mutex_);
    for (const auto& [tab, browser] : browsers_) {
        (void)tab;
        if (browser && browser->GetHost()) {
            browser->GetHost()->StartDownload(CefString(std::string(url)));
            return true;
        }
    }
    return false;
}

void CefAdapter::set_adblock_rules(std::string_view rules, bool enabled) {
    // UI thread. Rebuilding the engine under the lock means an in-flight
    // request check waits a few hundred ms at worst — rules change rarely
    // (startup, settings toggle), and correctness beats a stale engine.
    {
        std::lock_guard<std::mutex> lock(g_adblock.mutex);
        g_adblock.engine = std::make_unique<adblock::AdblockEngine>(rules);
        g_adblock.enabled = enabled;
        AURORA_LOG(util::LogLevel::Info, "adblock")
            << "engine "
            << (g_adblock.engine->available() ? "ready" : "UNAVAILABLE (built "
                                                   "without the Rust engine)")
            << ", blocking " << (enabled ? "on" : "off");
    }
    refresh_cosmetic_pages();
}

void CefAdapter::set_adblock_exceptions(const std::vector<std::string>& sites) {
    {
        std::lock_guard<std::mutex> lock(g_adblock.mutex);
        g_adblock.exceptions.clear();
        g_adblock.exceptions.insert(sites.begin(), sites.end());
    }
    refresh_cosmetic_pages();
}

void CefAdapter::set_adblock_enabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(g_adblock.mutex);
        g_adblock.enabled = enabled;
    }
    refresh_cosmetic_pages();
}

void CefAdapter::reset_adblock_count(TabId tab) {
    std::lock_guard<std::mutex> lock(g_adblock.mutex);
    g_adblock.blocked.erase(tab);
}

void CefAdapter::set_adblock_blocked_callback(AdblockBlockedCallback cb) {
    std::lock_guard<std::mutex> lock(g_adblock.mutex);
    g_adblock.notify = std::move(cb);
}

void CefAdapter::set_key_event_callback(KeyEventCallback cb) {
    std::lock_guard<std::mutex> lock(key_event_mutex_);
    key_event_callback_ = std::move(cb);
}

bool CefAdapter::forward_key_event(TabId tab,
                                   const webview::EngineKeyEvent& ev) {
    KeyEventCallback cb;
    {
        std::lock_guard<std::mutex> lock(key_event_mutex_);
        cb = key_event_callback_;
    }
    return cb ? cb(tab, ev) : false;
}

void CefAdapter::set_extension_install_callback(ExtensionInstallCallback cb) {
    std::lock_guard<std::mutex> lock(extension_install_mutex_);
    extension_install_callback_ = std::move(cb);
}

void CefAdapter::set_extension_popup_owner(TabId popup, TabId owner,
                                           std::string_view title,
                                           std::string_view url) {
    std::lock_guard<std::mutex> lock(extension_popup_mutex_);
    if (!popup.valid() || !owner.valid()) {
        extension_popup_owners_.erase(popup);
        return;
    }
    extension_popup_owners_[popup] =
        ExtensionPopupOwner{owner, std::string(title), std::string(url)};
}

namespace {

// Percent-decoding for the popup bridge. The popup encodes its payload with
// encodeURIComponent(), so the browser process only ever splits on '|' and
// decodes — it never has to parse JavaScript source out of a JSON document.
std::string percent_decode(std::string_view value) {
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%' || i + 2 >= value.size()) {
            out.push_back(value[i]);
            continue;
        }
        const int hi = hex(value[i + 1]);
        const int lo = hex(value[i + 2]);
        if (hi < 0 || lo < 0) {
            out.push_back(value[i]);
            continue;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return out;
}

// Receiver stub installed in an owning page immediately before the popup
// bridge delivers anything to it. Idempotent by construction (it only defines
// what is missing), so re-injecting it on every call costs nothing and
// survives navigations, which is why it is not tracked per tab.
//
// It gives a page a message sink an extension can address:
//   * window.__auroraExtOnMessage.addListener(fn) — what a content script (or
//     an executeScript-injected helper) registers,
//   * window.__auroraExtDeliver(message, sender) — what the popup bridge
//     calls; the first listener that returns a value wins, matching Chrome's
//     chrome.runtime.onMessage semantics closely enough for popup→page control
//     messages.
// chrome.runtime.onMessage is aliased onto it when the page has no extension
// API of its own, so an injected content script written for Chrome works
// unmodified.
//
// When CEF's own extension system (--enable-extensions) already created a
// native chrome.runtime.onMessage in this page, that event is NOT replaced:
// bridge-delivered messages never travel the native pipe, so a content script
// registered only with the native event would never hear a popup's control
// message (the Volume Master "volume does nothing" failure). Instead the
// native event is remembered and __auroraExtDeliver also (a) invokes its
// dispatch() when the bindings expose one — which reaches every natively
// registered listener — and (b) mirrors every registration made after this
// bootstrap ran, as the fallback when dispatch() is unavailable. The two
// native paths are mutually exclusive per delivery so no listener fires twice.
constexpr char kOwnerBootstrap[] = R"JS(
(() => {
  const g = globalThis;
  if (!g.__auroraExtOnMessage) {
    const listeners = [];
    // Listeners mirrored from a wrapped native chrome.runtime.onMessage (see
    // below). Kept separate so they are only invoked directly when the native
    // event cannot dispatch to them itself — never twice.
    g.__auroraExtNativeMirror = [];
    g.__auroraExtOnMessage = {
      addListener(fn) {
        if (typeof fn === 'function' && listeners.indexOf(fn) < 0) {
          listeners.push(fn);
        }
      },
      removeListener(fn) {
        const at = listeners.indexOf(fn);
        if (at >= 0) listeners.splice(at, 1);
      },
      hasListener(fn) { return listeners.indexOf(fn) >= 0; },
      __listeners: listeners,
    };
    g.__auroraExtDeliver = (message, sender) => {
      let answer;
      const invoke = (fn) => {
        try {
          const value = fn(message, sender || {}, (reply) => { answer = reply; });
          if (value !== undefined && answer === undefined) answer = value;
        } catch (e) {
          // One bad listener must not stop the others.
        }
      };
      for (const fn of listeners.slice()) invoke(fn);
      // Also reach listeners held by the engine's own runtime.onMessage.
      // Chromium's JS event bindings expose dispatch(); when present it
      // reaches every natively registered listener, including ones added
      // before this bootstrap could wrap addListener. Without dispatch(),
      // the mirrored registrations are the fallback.
      const native = g.__auroraExtNativeOnMessage;
      let dispatched = false;
      if (native && typeof native.dispatch === 'function') {
        try {
          native.dispatch(message, sender || {},
                          (reply) => { answer = reply; });
          dispatched = true;
        } catch (e) {}
      }
      if (!dispatched) {
        const mirrored = g.__auroraExtNativeMirror || [];
        for (const fn of mirrored.slice()) invoke(fn);
      }
      return answer;
    };
  }
  const chrome = (g.chrome = g.chrome || {});
  const runtime = (chrome.runtime = chrome.runtime || {});
  if (!runtime.onMessage) {
    runtime.onMessage = g.__auroraExtOnMessage;
  } else if (runtime.onMessage !== g.__auroraExtOnMessage &&
             !runtime.onMessage.__auroraExtWrapped) {
    // The engine provided a native onMessage. Remember it for the dispatch
    // path above and mirror every FUTURE registration so bridge-delivered
    // messages reach those listeners even when dispatch() is unavailable.
    const native = runtime.onMessage;
    g.__auroraExtNativeOnMessage = native;
    const mirrored =
        g.__auroraExtNativeMirror || (g.__auroraExtNativeMirror = []);
    const nativeAdd = typeof native.addListener === 'function'
        ? native.addListener.bind(native) : null;
    const nativeRemove = typeof native.removeListener === 'function'
        ? native.removeListener.bind(native) : null;
    try {
      native.addListener = function (fn) {
        if (typeof fn === 'function' && mirrored.indexOf(fn) < 0) {
          mirrored.push(fn);
        }
        if (nativeAdd) { try { nativeAdd(fn); } catch (e) {} }
      };
      native.removeListener = function (fn) {
        const at = mirrored.indexOf(fn);
        if (at >= 0) mirrored.splice(at, 1);
        if (nativeRemove) { try { nativeRemove(fn); } catch (e) {} }
      };
      native.__auroraExtWrapped = true;
    } catch (e) {
      // A frozen/sealed native event cannot be wrapped; dispatch() (when
      // available) remains the native delivery path.
    }
  }
  // --- chrome.storage.local in the page ----------------------------------
  // A content script written for Chrome reads its settings from
  // chrome.storage.local; with no extension bindings in the page that call
  // throws and the script dies before it ever registers a listener. The area
  // is backed by the page origin's localStorage (persistent, CEF runs with a
  // cache path) with an in-memory map when localStorage is refused, values are
  // always JSON encoded, and the area objects are created once so
  // chrome.storage.local === chrome.storage.local holds.
  if (!g.__auroraExtStorage) {
    const settle = (callback, value) => {
      if (typeof callback === 'function') {
        queueMicrotask(() => { try { callback(value); } catch (e) {} });
        return undefined;
      }
      return Promise.resolve(value);
    };
    const fallback = new Map();
    let backend = null;
    try {
      const probe = 'aurora.ext.probe';
      g.localStorage.setItem(probe, '1');
      g.localStorage.removeItem(probe);
      backend = g.localStorage;
    } catch (e) {
      backend = null;
    }
    const store = backend ? {
      get: (k) => backend.getItem(k),
      set: (k, v) => { backend.setItem(k, v); },
      del: (k) => { backend.removeItem(k); },
      keys: () => {
        const out = [];
        for (let i = 0; i < backend.length; i++) out.push(backend.key(i));
        return out;
      },
    } : {
      get: (k) => (fallback.has(k) ? fallback.get(k) : null),
      set: (k, v) => { fallback.set(k, String(v)); },
      del: (k) => { fallback.delete(k); },
      keys: () => Array.from(fallback.keys()),
    };
    const changeListeners = [];
    const makeArea = (area) => {
      const prefix = 'aurora.ext.storage.' + area + '.';
      const readKey = (key) => {
        let raw = null;
        try { raw = store.get(prefix + key); } catch (e) {}
        if (raw === null || raw === undefined) return undefined;
        try { return JSON.parse(raw); } catch (e) { return undefined; }
      };
      const ownKeys = () => {
        const out = [];
        try {
          for (const k of store.keys()) {
            if (k && k.indexOf(prefix) === 0) out.push(k.slice(prefix.length));
          }
        } catch (e) {}
        return out;
      };
      // Asynchronous like Chrome's, and silent for writes that changed nothing.
      const notify = (changes) => {
        if (!Object.keys(changes).length) return;
        const targets = changeListeners.slice();
        queueMicrotask(() => {
          for (const fn of targets) {
            try { fn(changes, area); } catch (e) {}
          }
        });
      };
      return {
        get(query, callback) {
          const cb = typeof query === 'function' ? query : callback;
          const q = typeof query === 'function' ? null : query;
          const out = {};
          if (q === null || q === undefined) {
            for (const k of ownKeys()) out[k] = readKey(k);
          } else if (typeof q === 'string') {
            const v = readKey(q);
            if (v !== undefined) out[q] = v;
          } else if (Array.isArray(q)) {
            for (const k of q) {
              const v = readKey(k);
              if (v !== undefined) out[k] = v;
            }
          } else if (typeof q === 'object') {
            // Object form supplies defaults for never-stored keys.
            for (const k of Object.keys(q)) {
              const v = readKey(k);
              out[k] = v === undefined ? q[k] : v;
            }
          }
          return settle(cb, out);
        },
        set(items, callback) {
          const changes = {};
          for (const k of Object.keys(items || {})) {
            const oldValue = readKey(k);
            try {
              const encoded = JSON.stringify(items[k]);
              let previous;
              try { previous = store.get(prefix + k); } catch (e) {}
              if (previous === encoded) continue;
              store.set(prefix + k, encoded);
              changes[k] = { oldValue: oldValue, newValue: items[k] };
            } catch (e) {}
          }
          notify(changes);
          return settle(callback, undefined);
        },
        remove(keys, callback) {
          const list = Array.isArray(keys) ? keys : [keys];
          const changes = {};
          for (const k of list) {
            const oldValue = readKey(k);
            if (oldValue === undefined) continue;
            try { store.del(prefix + k); } catch (e) {}
            changes[k] = { oldValue: oldValue };
          }
          notify(changes);
          return settle(callback, undefined);
        },
        clear(callback) {
          const changes = {};
          for (const k of ownKeys()) {
            changes[k] = { oldValue: readKey(k) };
            try { store.del(prefix + k); } catch (e) {}
          }
          notify(changes);
          return settle(callback, undefined);
        },
        getBytesInUse(keys, callback) {
          const cb = typeof keys === 'function' ? keys : callback;
          const list = (keys === null || keys === undefined ||
                        typeof keys === 'function')
              ? ownKeys() : (Array.isArray(keys) ? keys : [keys]);
          let bytes = 0;
          for (const k of list) {
            let raw = null;
            try { raw = store.get(prefix + k); } catch (e) {}
            if (raw === null || raw === undefined) continue;
            bytes += raw.length + String(k).length;
          }
          return settle(cb, bytes);
        },
      };
    };
    g.__auroraExtStorage = {
      local: makeArea('local'),
      sync: makeArea('sync'),
      session: makeArea('session'),
      onChanged: {
        addListener(fn) {
          if (typeof fn === 'function' && changeListeners.indexOf(fn) < 0) {
            changeListeners.push(fn);
          }
        },
        removeListener(fn) {
          const at = changeListeners.indexOf(fn);
          if (at >= 0) changeListeners.splice(at, 1);
        },
        hasListener(fn) { return changeListeners.indexOf(fn) >= 0; },
      },
    };
  }
  {
    const storage = (chrome.storage = chrome.storage || {});
    const shim = g.__auroraExtStorage;
    for (const area of ['local', 'sync', 'session']) {
      if (!storage[area] || typeof storage[area].get !== 'function') {
        storage[area] = shim[area];
      }
    }
    if (!storage.onChanged) storage.onChanged = shim.onChanged;
  }

  // --- Volume boost: Volume Master compatibility -------------------------
  // Volume Master (Peta Sittek, MV3) amplifies tab audio through a service
  // worker + chrome.offscreen document + chrome.tabCapture.getMediaStreamId.
  // CEF's extension system hosts none of that, so the popup's gain-change
  // message would reach no listener and the volume would never change. This
  // agent is the compatibility layer: it applies the requested gain to the
  // page's own <audio>/<video> elements through a Web Audio GainNode — the
  // technique a content-script volume booster uses. It listens on the
  // onMessage bridge installed above, so the popup's popup-gain-change
  // reaches it unchanged. Limitation: only main-document media elements are
  // amplified; cross-origin iframe media and non-element Web Audio are not.
  if (!g.__auroraVolumeBoost) {
    const AC = g.AudioContext || g.webkitAudioContext;
    const agent = (g.__auroraVolumeBoost = {
      _ctx: null, _gain: null, _wired: new WeakSet(), _hasWired: false,
      _running() { return !!(this._ctx && this._ctx.state === 'running'); },
      _ensure() {
        if (this._ctx) return true;
        if (typeof AC !== 'function') return false;
        try {
          this._ctx = new AC();
          this._gain = this._ctx.createGain();
          this._gain.gain.value = 1;
          this._gain.connect(this._ctx.destination);
          // createMediaElementSource reroutes an element's audio through this
          // graph, so wiring into a suspended context would mute the tab. Only
          // wire once the context is actually running.
          try {
            this._ctx.addEventListener('statechange', () => {
              if (this._running()) this._wireAll();
            });
          } catch (e) {}
          if (this._ctx.state === 'suspended') {
            try { this._ctx.resume(); } catch (e) {}
          }
        } catch (e) { this._ctx = null; this._gain = null; return false; }
        return true;
      },
      _wireOne(el) {
        if (!el || this._wired.has(el) || !this._running()) return;
        if (typeof el.play !== 'function') return;
        try {
          this._ctx.createMediaElementSource(el).connect(this._gain);
        } catch (e) {
          // Element already connected to another MediaElementSource.
        } finally {
          this._wired.add(el);
          this._hasWired = true;
        }
      },
      _wireAll() {
        if (!this._running()) return;
        const root = g.document;
        if (!root || !root.querySelectorAll) return;
        try {
          root.querySelectorAll('video, audio').forEach((el) => this._wireOne(el));
        } catch (e) {}
      },
      setGain(fraction) {
        const v = Math.max(0, Math.min(6, Number(fraction) || 0));
        // No boost requested and no graph exists yet: don't create an
        // AudioContext or reroute any media element — the page already plays
        // at 100%.
        if (v === 1 && !this._ctx) return;
        if (!this._ensure()) return;
        try {
          const now = this._ctx.currentTime;
          this._gain.gain.cancelScheduledValues(now);
          this._gain.gain.setValueAtTime(this._gain.gain.value, now);
          this._gain.gain.linearRampToValueAtTime(v, now + 0.02);
        } catch (e) {
          try { this._gain.gain.value = v; } catch (e2) {}
        }
        if (v !== 1 || this._hasWired) this._wireAll();
      },
    });
    if (g.document && typeof g.MutationObserver === 'function') {
      try {
        const mo = new g.MutationObserver(() => {
          if (agent._running()) agent._wireAll();
        });
        mo.observe(g.document, { childList: true, subtree: true });
      } catch (e) {}
    }
    if (runtime.onMessage && typeof runtime.onMessage.addListener === 'function') {
      runtime.onMessage.addListener((message, sender, sendResponse) => {
        if (!message || typeof message !== 'object') return;
        if (message.action === 'popup-gain-change' &&
            message.target === 'service-worker' &&
            message.volumeValue != null) {
          agent.setGain(Number(message.volumeValue) / 100);
        }
      });
    }
  }
})();
)JS";

} // namespace

bool CefAdapter::run_in_owner_tab(TabId tab, const std::string& source) {
    const auto browser = browser_for(tab);
    if (!browser || !browser->IsValid()) return false;
    const auto frame = browser->GetMainFrame();
    if (!frame) return false;
    // Bootstrap first: ExecuteJavaScript is ordered per frame, so the receiver
    // is always defined by the time the delivered call runs.
    frame->ExecuteJavaScript(CefString(std::string(kOwnerBootstrap)),
                             CefString("aurora://extension-bridge-host.js"), 1);
    frame->ExecuteJavaScript(CefString(source),
                             CefString("aurora://extension-bridge-call.js"), 1);
    return true;
}

std::string CefAdapter::extension_popup_script(TabId popup,
                                               std::string_view bridge_request) {
    // --- Reply envelope from an OWNER tab ----------------------------------
    // "reply|<bridge id>|<percent-encoded JSON>", printed by the owner tab
    // after __auroraExtDeliver returned. `popup` is the OWNER tab here, not a
    // registered popup, so this must run BEFORE the popup-owner lookup below.
    // The awaiting promise lives in the popup(s) this tab owns; the value is
    // handed to them directly and the caller gets a non-empty acknowledgement
    // (nothing is executed back in the owner page).
    constexpr std::string_view kReplyPrefix("reply|");
    if (bridge_request.size() > kReplyPrefix.size() &&
        bridge_request.compare(0, kReplyPrefix.size(), kReplyPrefix) == 0) {
        const std::string_view rest =
            bridge_request.substr(kReplyPrefix.size());
        const size_t split = rest.find('|');
        if (split == std::string_view::npos) return {};
        const std::string_view id = rest.substr(0, split);
        if (id.empty() ||
            id.find_first_not_of("0123456789") != std::string_view::npos) {
            return {};
        }
        // The payload is JSON, never executable source: it is re-parsed in the
        // popup from a quoted string literal so a hostile page that prints the
        // tag itself cannot inject code into an extension popup.
        const std::string value_json = percent_decode(rest.substr(split + 1));
        std::vector<TabId> popups;
        {
            std::lock_guard<std::mutex> lock(extension_popup_mutex_);
            for (const auto& entry : extension_popup_owners_) {
                if (entry.second.tab == popup) popups.push_back(entry.first);
            }
        }
        if (popups.empty()) return {};
        std::string resolve = "(function(){var v;try{v=JSON.parse(";
        resolve += javascript_string(value_json);
        resolve += ");}catch(e){v=null;}if(window.__auroraExtBridge)";
        resolve += "window.__auroraExtBridge.__resolve(";
        resolve += std::string(id);
        resolve += ", v);})();";
        bool routed = false;
        for (const TabId target_popup : popups) {
            const auto browser = browser_for(target_popup);
            if (!browser || !browser->IsValid()) continue;
            const auto frame = browser->GetMainFrame();
            if (!frame) continue;
            frame->ExecuteJavaScript(
                CefString(resolve),
                CefString("aurora://extension-popup-bridge.js"), 1);
            routed = true;
        }
        return routed ? std::string("1") : std::string();
    }

    ExtensionPopupOwner owner;
    {
        std::lock_guard<std::mutex> lock(extension_popup_mutex_);
        const auto it = extension_popup_owners_.find(popup);
        if (it == extension_popup_owners_.end()) return {};
        owner = it->second;
    }

    // --- Bridge request from a running popup -------------------------------
    // Envelope: "<id>|<verb>|<percent-encoded source>". Only registered popups
    // reach this (the lookup above already returned for everything else), and
    // only the one verb is accepted.
    if (!bridge_request.empty()) {
        const size_t first = bridge_request.find('|');
        if (first == std::string_view::npos) return {};
        const size_t second = bridge_request.find('|', first + 1);
        if (second == std::string_view::npos) return {};
        const std::string_view id = bridge_request.substr(0, first);
        const std::string_view verb =
            bridge_request.substr(first + 1, second - first - 1);
        if (id.empty() ||
            id.find_first_not_of("0123456789") != std::string_view::npos) {
            return {};
        }
        bool ok = false;
        if (verb == "run") {
            ok = run_in_owner_tab(
                owner.tab, percent_decode(bridge_request.substr(second + 1)));
            // The delivered source itself prints AURORA_EXT_REPLY:<id>|<json>
            // from the owner tab once __auroraExtDeliver returned, and that
            // record (handled above) resolves the popup's promise WITH THE
            // VALUE. Resolving here would race it and settle the promise on a
            // bare boolean, so a successful dispatch stays silent; the popup's
            // own 4s timeout is the fallback when no reply ever arrives.
            // ExecuteJavaScript still returned nothing to report, so the
            // envelope is acknowledged (swallowed) with a harmless statement.
            if (ok) return "void 0;";
        }
        std::string reply = "window.__auroraExtBridge && ";
        reply += "window.__auroraExtBridge.__resolve(";
        reply += std::string(id);
        reply += ok ? ", true);" : ", false);";
        return reply;
    }

    const auto target = browser_for(owner.tab);
    if (!target || !target->IsValid()) return {};

    // CefBrowser::GetIdentifier is explicitly the tabId used by extension APIs.
    // Returning it (rather than Aurora's unrelated TabId) makes native
    // tabs.sendMessage/update/executeScript calls address the invoking CEF host.
    const int cef_tab_id = target->GetIdentifier();
    if (cef_tab_id <= 0) return {};
    const auto main_frame = target->GetMainFrame();
    if (main_frame) {
        const std::string current_url = main_frame->GetURL().ToString();
        if (!current_url.empty()) owner.url = current_url;
    }

    // The document-start shim. Its job is NOT to reimplement Chrome's
    // extension system: it is the compatibility floor an Alloy-style CEF 152
    // browser leaves behind. Alloy-style browsers have no extension bindings
    // at all, so an unguarded popup script dies on its first chrome.storage or
    // chrome.tabs call — the popup renders, nothing it does sticks, and the
    // extension looks broken. Every entry point below is feature-detected and
    // only filled in when the engine did not provide it, so a build (or a
    // future CEF) where the real bindings exist keeps using them.
    //
    // Storage is backed by localStorage in the popup's own
    // chrome-extension://<id> origin: that origin is per-extension, and CEF is
    // initialized with a persistent cache_path, so values survive both a popup
    // reopen and an application restart.
    std::string script = R"JS(
(() => {
  try {
  const g = globalThis;
  const chrome = (g.chrome = g.chrome || {});
  const BRIDGE_TAG = "AURORA_EXT_BRIDGE:";
  const OWNER = Object.freeze({
    id: )JS";
    script += std::to_string(cef_tab_id);
    script += ", index: 0, windowId: 1, active: true, highlighted: true, "
              "selected: true, pinned: false, incognito: false, audible: true, "
              "mutedInfo: { muted: false }, status: "
              "'complete', title: ";
    script += javascript_string(owner.title);
    script += ", url: ";
    script += javascript_string(owner.url);
    script += R"JS(
  });

  // --- callback / promise duality ----------------------------------------
  // MV2 extensions pass a callback; MV3 code awaits a promise. Chrome supports
  // both shapes on the same function, so the shim does too.
  const settle = (callback, value) => {
    if (typeof callback === 'function') {
      queueMicrotask(() => { try { callback(value); } catch (e) {} });
      return undefined;
    }
    return Promise.resolve(value);
  };

  // --- browser-process bridge --------------------------------------------
  // Alloy-style browsers expose no extension message pipe and CEF's browser
  // process cannot evaluate-and-return, so an outgoing request is a tagged
  // console record (swallowed by Aurora's display handler) and the reply is a
  // script Aurora runs back in this frame.
  // `call` accepts either a ready source string or a factory taking the bridge
  // id, because the delivered source has to embed that id: the owner tab prints
  // AURORA_EXT_REPLY:<id>|<json> once the listener answered, and Aurora routes
  // that value back into __resolve below. __valueAware distinguishes this
  // bridge from an older boolean-only one left on the window.
  let bridge = g.__auroraExtBridge;
  if (!bridge || !bridge.__valueAware) {
    bridge = (g.__auroraExtBridge = {
      __valueAware: true,
      seq: 1,
      pending: new Map(),
      call(source) {
        const id = bridge.seq++;
        let text;
        try {
          text = typeof source === 'function' ? source(id) : source;
        } catch (e) {
          return Promise.resolve(false);
        }
        const done = new Promise((resolve) => bridge.pending.set(id, resolve));
        try {
          console.info(BRIDGE_TAG + id + "|run|" + encodeURIComponent(text));
        } catch (e) {
          bridge.pending.delete(id);
          return Promise.resolve(false);
        }
        // A reply that never arrives (the owning tab navigated away or closed
        // mid-flight) must not leave the extension awaiting forever. false is
        // kept as the no-reply value so existing callers see what they always
        // saw when the round trip fails.
        setTimeout(() => bridge.__resolve(id, false), 4000);
        return done;
      },
      // value is whatever the owner tab's listener answered (sendResponse or a
      // synchronous return), or false when the timeout above fired first.
      __resolve(id, value) {
        const resolve = bridge.pending.get(id);
        if (!resolve) return;
        bridge.pending.delete(id);
        resolve(value);
      },
    });
  }

  // --- persistent key/value store ----------------------------------------
  // localStorage in the popup's own chrome-extension://<id> origin is what
  // makes settings survive a reopen (CEF runs with a persistent cache path).
  // Some origins/profiles can refuse it outright, and a popup that throws on
  // its first storage read is exactly the "extension does nothing" failure
  // this shim exists to prevent, so an in-memory map takes over on failure:
  // degraded (session-only) instead of broken.
  const fallback = new Map();
  let backend = null;
  try {
    const probe = 'aurora.ext.probe';
    g.localStorage.setItem(probe, '1');
    g.localStorage.removeItem(probe);
    backend = g.localStorage;
  } catch (e) {
    backend = null;
  }
  const store = backend ? {
    get: (k) => backend.getItem(k),
    set: (k, v) => backend.setItem(k, v),
    del: (k) => backend.removeItem(k),
    keys: () => {
      const out = [];
      for (let i = 0; i < backend.length; i++) out.push(backend.key(i));
      return out;
    },
  } : {
    get: (k) => (fallback.has(k) ? fallback.get(k) : null),
    set: (k, v) => { fallback.set(k, String(v)); },
    del: (k) => { fallback.delete(k); },
    keys: () => Array.from(fallback.keys()),
  };

  // --- chrome.storage -----------------------------------------------------
  const storage = (chrome.storage = chrome.storage || {});
  const changeListeners = [];
  const makeArea = (area) => {
    const prefix = "aurora.ext.storage." + area + ".";
    const readKey = (key) => {
      let raw = null;
      try { raw = store.get(prefix + key); } catch (e) {}
      if (raw === null || raw === undefined) return undefined;
      try { return JSON.parse(raw); } catch (e) { return undefined; }
    };
    const ownKeys = () => {
      const out = [];
      try {
        for (const k of store.keys()) {
          if (k && k.indexOf(prefix) === 0) out.push(k.slice(prefix.length));
        }
      } catch (e) {}
      return out;
    };
    // Chrome dispatches storage.onChanged asynchronously, and never for a
    // write that did not actually change anything, so callers can use it as a
    // change signal rather than a write signal.
    const notify = (changes) => {
      if (!Object.keys(changes).length) return;
      const targets = changeListeners.slice();
      queueMicrotask(() => {
        for (const fn of targets) {
          try { fn(changes, area); } catch (e) {}
        }
      });
    };
    return {
      get(query, callback) {
        // get(callback) is a legal Chrome shape (fetch everything).
        const cb = typeof query === 'function' ? query : callback;
        const q = typeof query === 'function' ? null : query;
        const out = {};
        if (q === null || q === undefined) {
          for (const k of ownKeys()) out[k] = readKey(k);
        } else if (typeof q === 'string') {
          const v = readKey(q);
          if (v !== undefined) out[q] = v;
        } else if (Array.isArray(q)) {
          for (const k of q) {
            const v = readKey(k);
            if (v !== undefined) out[k] = v;
          }
        } else if (typeof q === 'object') {
          // Object form supplies defaults for keys that were never stored,
          // which is exactly how an extension seeds its first run.
          for (const k of Object.keys(q)) {
            const v = readKey(k);
            out[k] = v === undefined ? q[k] : v;
          }
        }
        return settle(cb, out);
      },
      set(items, callback) {
        const changes = {};
        for (const k of Object.keys(items || {})) {
          const oldValue = readKey(k);
          try {
            const encoded = JSON.stringify(items[k]);
            // A no-op write must not raise onChanged (Chrome's contract), and
            // the stored form is the honest comparison because that is what a
            // later get() will return.
            let previous;
            try { previous = store.get(prefix + k); } catch (e) {}
            if (previous === encoded) continue;
            store.set(prefix + k, encoded);
            changes[k] = { oldValue: oldValue, newValue: items[k] };
          } catch (e) {}
        }
        notify(changes);
        return settle(callback, undefined);
      },
      remove(keys, callback) {
        const list = Array.isArray(keys) ? keys : [keys];
        const changes = {};
        for (const k of list) {
          const oldValue = readKey(k);
          if (oldValue === undefined) continue;
          try { store.del(prefix + k); } catch (e) {}
          changes[k] = { oldValue: oldValue };
        }
        notify(changes);
        return settle(callback, undefined);
      },
      clear(callback) {
        const changes = {};
        for (const k of ownKeys()) {
          changes[k] = { oldValue: readKey(k) };
          try { store.del(prefix + k); } catch (e) {}
        }
        notify(changes);
        return settle(callback, undefined);
      },
      getBytesInUse(keys, callback) {
        const cb = typeof keys === 'function' ? keys : callback;
        // Omitted/null keys means the whole area.
        const list = (keys === null || keys === undefined ||
                      typeof keys === 'function')
            ? ownKeys() : (Array.isArray(keys) ? keys : [keys]);
        let bytes = 0;
        for (const k of list) {
          let raw = null;
          try { raw = store.get(prefix + k); } catch (e) {}
          if (raw === null || raw === undefined) continue;
          bytes += raw.length + String(k).length;
        }
        return settle(cb, bytes);
      },
    };
  };
  // sync has no server behind it here; Chrome's own contract is that it is a
  // storage area that persists, and that is what an extension actually relies
  // on, so it is backed the same way under its own namespace.
  if (!storage.local || typeof storage.local.get !== 'function') {
    storage.local = makeArea('local');
  }
  if (!storage.sync || typeof storage.sync.get !== 'function') {
    storage.sync = makeArea('sync');
  }
  if (!storage.session || typeof storage.session.get !== 'function') {
    storage.session = makeArea('session');
  }
  if (!storage.onChanged) {
    storage.onChanged = {
      addListener(fn) {
        if (typeof fn === 'function' && changeListeners.indexOf(fn) < 0) {
          changeListeners.push(fn);
        }
      },
      removeListener(fn) {
        const at = changeListeners.indexOf(fn);
        if (at >= 0) changeListeners.splice(at, 1);
      },
      hasListener(fn) { return changeListeners.indexOf(fn) >= 0; },
    };
  }

  // --- chrome.runtime -----------------------------------------------------
  const runtime = (chrome.runtime = chrome.runtime || {});
  if (!runtime.id) runtime.id = location.hostname;
  if (typeof runtime.getURL !== 'function') {
    runtime.getURL = (path) =>
        new URL(String(path || ''), location.origin + '/').href;
  }
  if (!('lastError' in runtime)) runtime.lastError = undefined;
  if (!runtime.onMessage) {
    // A popup with no background page has nothing to receive from; the
    // listener registry exists so registering does not throw.
    const popupListeners = [];
    runtime.onMessage = {
      addListener(fn) {
        if (typeof fn === 'function') popupListeners.push(fn);
      },
      removeListener(fn) {
        const at = popupListeners.indexOf(fn);
        if (at >= 0) popupListeners.splice(at, 1);
      },
      hasListener(fn) { return popupListeners.indexOf(fn) >= 0; },
    };
  }
  // chrome.runtime.connect / onConnect — many MV3 extensions open a port to a
  // background service worker. Aurora has no background page, so a port is a
  // no-op stub: connect() returns a Port-like object whose postMessage and
  // disconnect are no-ops, and onConnect never fires. Without this, an
  // extension that calls connect() throws and its popup dies.
  if (typeof runtime.connect !== 'function') {
    runtime.connect = function (extensionId, connectInfo) {
      if (typeof extensionId === 'object') { connectInfo = extensionId; extensionId = undefined; }
      const name = (connectInfo && typeof connectInfo.name === 'string') ? connectInfo.name : '';
      return {
        name: name,
        sender: undefined,
        onMessage: { addListener() {}, removeListener() {}, hasListener() { return false; } },
        onDisconnect: { addListener(fn) { if (typeof fn === 'function') queueMicrotask(() => { try { fn({}); } catch (e) {} }); } },
        postMessage() {},
        disconnect() {},
      };
    };
  }
  if (!runtime.onConnect) {
    runtime.onConnect = {
      addListener() {}, removeListener() {}, hasListener() { return false; },
    };
  }
  // In Chrome, runtime.sendMessage goes to the background script, which
  // then forwards to content scripts. Aurora has no background page, so
  // we deliver directly to the owning tab's content scripts through the
  // bridge — the same path tabs.sendMessage uses. This is what makes a
  // popup's control message (e.g. Volume Master's volume-change command)
  // actually reach the injected content script.
  //
  // Installed UNCONDITIONALLY: with the engine's extension system enabled
  // (--enable-extensions) CEF defines a native sendMessage that targets the
  // nonexistent background page, so the old feature-detect skipped this shim
  // and every popup control message was silently dropped — the "setting the
  // volume higher does nothing" failure. The native implementation, when
  // present, is still invoked as well so genuine engine-side listeners keep
  // hearing the message; its "receiving end does not exist" error is
  // swallowed by checking lastError in the callback.
  const nativeRuntimeSendMessage =
      typeof runtime.sendMessage === 'function'
          ? runtime.sendMessage.bind(runtime) : null;
  runtime.sendMessage = function (...args) {
    // Signatures:
    //   sendMessage(message)
    //   sendMessage(message, callback)  — callback is a function
    //   sendMessage(extensionId, message)
    //   sendMessage(extensionId, message, callback)
    // The first arg is an extensionId only if it's a string AND there
    // are more args AND the second arg is not a function.
    let message, callback;
    if (args.length === 0) return Promise.resolve(undefined);
    if (args.length === 1) {
      message = args[0];
    } else if (args.length === 2) {
      if (typeof args[1] === 'function') {
        message = args[0];
        callback = args[1];
      } else {
        // sendMessage(extensionId, message)
        message = args[1];
      }
    } else {
      // 3+ args: sendMessage(extensionId, message, callback)
      message = args[1];
      callback = args.find((a) => typeof a === 'function');
    }
    // Volume Master (Peta Sittek, MV3) amplifies tab audio through a service
    // worker + offscreen document + chrome.tabCapture — none of which CEF's
    // extension system hosts. So popup-gain-change would reach no listener and
    // popup-audio-data-get would never get a reply. Handle both here: answer
    // audio-data-get locally from the persisted gain (so the popup's slider,
    // reset and restore UI render correctly across reopens), and persist the
    // gain on gain-change before routing it to the owner tab where a GainNode
    // agent applies it.
    const VM = message && typeof message === 'object';
    if (VM && message.action === 'popup-audio-data-get' &&
        message.target === 'offscreen-document') {
      const tabId = message.tabId != null ? message.tabId : OWNER.id;
      const key = 'aurora.volumeMaster.gain.' + tabId;
      let stored = NaN;
      try {
        const raw = g.localStorage.getItem(key);
        stored = raw == null ? NaN : Number(raw);
      } catch (e) {}
      // Reapply a persisted non-default gain: the popup reads state before
      // it attaches its slider input listener, so its own input dispatch
      // would not yet have fired gain-change. Route it directly here so the
      // owner tab's GainNode agent picks up the restored value.
      if (Number.isFinite(stored) && stored !== 100) {
        try {
          Promise.resolve(deliver({ action: 'popup-gain-change',
                                    target: 'service-worker', tabId,
                                    volumeValue: stored })).catch(() => {});
        } catch (e) {}
      }
      const resp = Number.isFinite(stored)
        ? { gain: { gain: stored / 100 },
            equalizer: { algorithm: 'highpass', frequency: 0, q: 1, gain: 0 } }
        : null;
      return settle(callback, resp);
    }
    if (VM && message.action === 'popup-gain-change' &&
        message.target === 'service-worker' && message.volumeValue != null) {
      const tabId = message.tabId != null ? message.tabId : OWNER.id;
      try {
        g.localStorage.setItem('aurora.volumeMaster.gain.' + tabId,
                               String(message.volumeValue));
      } catch (e) {}
      // Fall through: deliver() routes to the owner tab's GainNode agent.
    }
    if (nativeRuntimeSendMessage) {
      try {
        nativeRuntimeSendMessage(message,
                                 () => { void chrome.runtime.lastError; });
      } catch (e) {}
    }
    // The bridge now resolves with the value the owner tab's listener
    // answered. A listener that answered nothing arrives as null over the
    // JSON round trip; Chrome hands the caller undefined there. false is the
    // bridge's no-reply sentinel (owner tab gone / 4s timeout) and is left
    // as-is, exactly what this call resolved to before.
    const done = deliver(message).then((v) => (v === null ? undefined : v));
    if (typeof callback === 'function') {
      done.then((v) => { try { callback(v); } catch (e) {} });
      return undefined;
    }
    return done;
  };
  const extension = (chrome.extension = chrome.extension || {});
  if (typeof extension.getURL !== 'function') extension.getURL = runtime.getURL;

  // --- chrome.tabs --------------------------------------------------------
  const tabs = (chrome.tabs = chrome.tabs || {});
  const nativeQuery =
      typeof tabs.query === 'function' ? tabs.query.bind(tabs) : null;
  // Only the exact {active:true, currentWindow:true} query is synthesized.
  // Every other query shape (including ones that add filters like url) is
  // delegated to CEF's native implementation with its original arguments when
  // there is one; without native bindings the owning tab is the only tab this
  // popup can honestly describe.
  tabs.query = function (queryInfo, callback) {
    const q = queryInfo || {};
    const keys = (typeof q === 'object' && q !== null) ? Object.keys(q) : [];
    const own = Object.prototype.hasOwnProperty.bind(q);
    const wants_active =
        keys.length === 2 && own('active') && own('currentWindow') &&
        q.active === true && q.currentWindow === true;
    // Volume Master (and similar) ask for {audible: true} to list tabs
    // currently playing audio. Aurora has no real per-tab audio signal for
    // these synthetic popups, but the only tab a popup can honestly describe
    // is its owner — and a user opens a volume popup precisely because the
    // owner tab is the one making sound. Report the owner as audible for that
    // narrow query shape (alone or alongside active/currentWindow) so the
    // extension's playing-tabs list is not empty. Every other shape is still
    // delegated to the native implementation unchanged.
    const wants_audible_owner =
        own('audible') && q.audible === true &&
        keys.every((k) => k === 'audible' || k === 'active' || k === 'currentWindow') &&
        (!own('active') || q.active === true) &&
        (!own('currentWindow') || q.currentWindow === true);
    if (wants_active || wants_audible_owner || !nativeQuery) {
      return settle(callback, [OWNER]);
    }
    return nativeQuery.apply(null, arguments);
  };
  if (typeof tabs.get !== 'function') {
    tabs.get = function (tabId, callback) { return settle(callback, OWNER); };
  }
  if (typeof tabs.getCurrent !== 'function') {
    tabs.getCurrent = function (callback) { return settle(callback, OWNER); };
  }

  // Deliver a message to the owning page through the browser-process bridge.
  const deliver = (message) => {
    const sender = {
      id: runtime.id,
      url: location.href,
      origin: location.origin,
      tab: OWNER,
    };
    // The delivered source captures __auroraExtDeliver's return value (a
    // listener's sendResponse reply or its synchronous return) and prints it
    // back as AURORA_EXT_REPLY:<id>|<json>; Aurora routes that record into
    // bridge.__resolve, which is how sendMessage gets a real answer instead of
    // a bare "dispatched" boolean. ExecuteJavaScript itself returns nothing.
    const payload = JSON.stringify(message === undefined ? null : message);
    const senderJson = JSON.stringify(sender);
    return bridge.call((id) =>
        '(function(){var d=window.__auroraExtDeliver;var r;' +
        'try{r=d?d(' + payload + ',' + senderJson + '):undefined;}' +
        'catch(e){r=undefined;}' +
        'var s;try{s=JSON.stringify(r===undefined?null:r);}catch(e){}' +
        'if(s===undefined)s="null";' +
        'try{console.info("AURORA_EXT_REPLY:" + ' + id +
        ' + "|" + encodeURIComponent(s));}catch(e){}})();');
  };

  // chrome.tabs.sendMessage always routes through the bridge too. A native
  // implementation (present with --enable-extensions) addresses the invoking
  // tab correctly — OWNER.id is CefBrowser::GetIdentifier(), which CEF
  // defines as the extension tabId — but Aurora injects content scripts via
  // the bridge into the page's main world, where a native send may never
  // land. Deferring to the native version (the old feature-detect) is how a
  // popup's volume command reached nothing. The native send is still made in
  // parallel so a genuine engine-side content script keeps hearing it; its
  // "receiving end does not exist" error is swallowed via the callback.
  const nativeTabsSendMessage =
      typeof tabs.sendMessage === 'function'
          ? tabs.sendMessage.bind(tabs) : null;
  tabs.sendMessage = function (tabId, message, options, callback) {
    const cb = typeof options === 'function' ? options : callback;
    if (nativeTabsSendMessage) {
      try {
        const opts = (options && typeof options === 'object')
            ? options : undefined;
        const swallow = () => { void chrome.runtime.lastError; };
        if (opts !== undefined) {
          nativeTabsSendMessage(tabId, message, opts, swallow);
        } else {
          nativeTabsSendMessage(tabId, message, swallow);
        }
      } catch (e) {}
    }
    // The bridge now resolves with the value the owner tab's listener
    // answered. A listener that answered nothing arrives as null over the
    // JSON round trip; Chrome hands the caller undefined there. false is the
    // bridge's no-reply sentinel (owner tab gone / 4s timeout) and is left
    // as-is, exactly what this call resolved to before.
    const done = deliver(message).then((v) => (v === null ? undefined : v));
    if (typeof cb === 'function') {
      done.then((v) => { try { cb(v); } catch (e) {} });
      return undefined;
    }
    return done;
  };

  // --- chrome.tabs.onUpdated / onRemoved / onCreated --------------------
  // Many extensions register listeners for tab lifecycle events. Aurora has
  // no event source for these, so the registries exist to prevent throws; they
  // simply never fire.
  for (const ev of ['onUpdated', 'onRemoved', 'onCreated', 'onActivated', 'onReplaced']) {
    if (!tabs[ev]) {
      tabs[ev] = {
        addListener() {}, removeListener() {}, hasListener() { return false; },
      };
    }
  }
  if (typeof tabs.update !== 'function') {
    tabs.update = function (tabId, props, callback) {
      // No-op update: returns the OWNER as if the update succeeded.
      return settle(callback, OWNER);
    };
  }
  if (typeof tabs.reload !== 'function') {
    tabs.reload = function (tabId, reloadProperties, callback) {
      const cb = typeof reloadProperties === 'function' ? reloadProperties : callback;
      return settle(cb, undefined);
    };
  }

  // --- chrome.windows ----------------------------------------------------
  // Window management stubs. Some extensions query the current window to
  // get the windowId for tabs.query({currentWindow:true}) or to position
  // their popup. Only getCurrent/get are stubbed; create returns a no-op.
  if (!chrome.windows) {
    chrome.windows = {
      WINDOW_ID_CURRENT: -2,
      WINDOW_ID_NONE: -1,
      get(windowId, getInfo, callback) {
        const cb = typeof getInfo === 'function' ? getInfo : callback;
        return settle(cb, { id: 1, focused: true, state: 'normal', type: 'normal', incognito: false });
      },
      getCurrent(getInfo, callback) {
        const cb = typeof getInfo === 'function' ? getInfo : callback;
        return settle(cb, { id: 1, focused: true, state: 'normal', type: 'normal', incognito: false });
      },
      getAll(getInfo, callback) {
        const cb = typeof getInfo === 'function' ? getInfo : callback;
        return settle(cb, [{ id: 1, focused: true, state: 'normal', type: 'normal', incognito: false }]);
      },
      onCreated: { addListener() {}, removeListener() {}, hasListener() { return false; } },
      onRemoved: { addListener() {}, removeListener() {}, hasListener() { return false; } },
      onFocusChanged: { addListener() {}, removeListener() {}, hasListener() { return false; } },
    };
  }

  // --- executeScript (MV2 tabs.*, MV3 scripting.*) ------------------------
  // A packaged file is fetched from the extension's own origin (this popup is
  // served from it, so the read is same-origin) and shipped as source.
  const sourceFromInjection = async (details) => {
    const d = details || {};
    if (typeof d.func === 'function') {
      const args = JSON.stringify(Array.isArray(d.args) ? d.args : []);
      return '(' + d.func.toString() + ').apply(null,' + args + ');';
    }
    if (typeof d.code === 'string') return d.code;
    const files = d.files || (d.file ? [d.file] : []);
    let joined = '';
    for (const file of files) {
      try {
        const response = await fetch(runtime.getURL(file));
        joined += await response.text();
        joined += '\n;\n';
      } catch (e) {}
    }
    return joined;
  };
  const scripting = (chrome.scripting = chrome.scripting || {});
  if (typeof scripting.executeScript !== 'function') {
    scripting.executeScript = function (injection, callback) {
      const done = sourceFromInjection(injection).then((source) => {
        if (!source) return [];
        return bridge.call(source).then(
            (ok) => (ok ? [{ frameId: 0, result: undefined }] : []));
      });
      if (typeof callback === 'function') {
        done.then((v) => { try { callback(v); } catch (e) {} });
        return undefined;
      }
      return done;
    };
  }
  if (typeof tabs.executeScript !== 'function') {
    tabs.executeScript = function (tabId, details, callback) {
      const d = typeof tabId === 'object' ? tabId : details;
      const cb = typeof details === 'function' ? details : callback;
      const done = sourceFromInjection(d).then((source) => {
        if (!source) return [];
        return bridge.call(source).then((ok) => (ok ? [null] : []));
      });
      if (typeof cb === 'function') {
        done.then((v) => { try { cb(v); } catch (e) {} });
        return undefined;
      }
      return done;
    };
  }

  // --- toolbar-action no-ops ---------------------------------------------
  // A popup that cannot set its own badge must not die on the attempt; these
  // surfaces have no chrome in Aurora to paint into.
  for (const key of ['action', 'browserAction', 'pageAction']) {
    const api = (chrome[key] = chrome[key] || {});
    for (const fn of ['setBadgeText', 'setBadgeBackgroundColor', 'setIcon',
                      'setTitle', 'setPopup', 'enable', 'disable']) {
      if (typeof api[fn] !== 'function') {
        api[fn] = function (details, callback) {
          const cb = typeof details === 'function' ? details : callback;
          return settle(cb, undefined);
        };
      }
    }
  }
  if (!chrome.i18n) {
    chrome.i18n = {
      getMessage: () => '',
      getUILanguage: () => navigator.language || 'en',
    };
  }
  } catch (e) {
    // The shim is a courtesy layer: if any of it fails the popup must still
    // render whatever it can, so the failure is reported and swallowed.
    try { console.warn('aurora: extension shim failed', e); } catch (ignored) {}
  }
})();
)JS";
    return script;
}

void CefAdapter::on_extension_downloaded(
    TabId tab, const webview::ExtensionInstallRequest& req) {
    ExtensionInstallCallback cb;
    {
        std::lock_guard<std::mutex> lock(extension_install_mutex_);
        cb = extension_install_callback_;
    }
    if (cb) cb(req);
    (void)tab;
}

void CefAdapter::set_permission_callback(PermissionCallback cb) {
    std::lock_guard<std::mutex> lock(permission_mutex_);
    permission_callback_ = std::move(cb);
}

void CefAdapter::on_permission_request(webview::PermissionRequest req) {
    PermissionCallback cb;
    {
        std::lock_guard<std::mutex> lock(permission_mutex_);
        cb = permission_callback_;
    }
    // No host: drop the request. The responder it carries denies on
    // destruction, so the page is refused rather than left hanging.
    if (cb) cb(std::move(req));
}

void CefAdapter::set_context_action_callback(ContextActionCallback cb) {
    std::lock_guard<std::mutex> lock(context_action_mutex_);
    context_action_callback_ = std::move(cb);
}

void CefAdapter::on_context_action(TabId tab, webview::WebViewController::ContextActionRequest req) {
    ContextActionCallback cb;
    {
        std::lock_guard<std::mutex> lock(context_action_mutex_);
        cb = context_action_callback_;
    }
    // Never call host code while holding the mutex.
    if (cb) cb(tab, std::move(req));
}

bool CefAdapter::open_in_new_tab(std::string url) {
    NewTabCallback cb;
    {
        std::lock_guard<std::mutex> lock(new_tab_mutex_);
        cb = new_tab_callback_;
    }
    // Never invoke host code while holding the callback mutex. In addition to
    // avoiding lock re-entry, the owning string remains valid if the host
    // queues it beyond this CEF callback.
    return cb && cb(std::move(url));
}

void CefAdapter::set_page_info_callback(PageInfoCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(cb);
}

void CefAdapter::shutdown() {
    // Idempotent: the host calls this once after the last window closes, and
    // ~CefAdapter is a safe no-op ref release afterwards.
    if (shutdown_started_.exchange(true)) return;

    // Drop the ad-block notify hook first: it captures the (soon-to-be-destroyed)
    // window, and a request check that races the browser teardown must not be
    // able to post a task calling into it. The engine itself stays alive until
    // process exit — it is stateless once no browsers remain.
    {
        std::lock_guard<std::mutex> lock(g_adblock.mutex);
        g_adblock.notify = nullptr;
    }

    // 1. Ask every live browser to close. CloseBrowser(true) is asynchronous:
    //    the browser is destroyed only when OnBeforeClose fires (which happens
    //    on the UI thread, during the pump below). Mark each as closing so the
    //    drain loop waits for the *actual* close, not just the map emptying.
    std::vector<CefRefPtr<CefBrowser>> to_close;
    {
        std::lock_guard<std::mutex> lock(browsers_mutex_);
        to_close.reserve(browsers_.size());
        for (auto& [id, browser] : browsers_) {
            if (browser) {
                closing_tabs_.insert(id);
                to_close.push_back(browser);
            }
        }
    }
    for (auto& browser : to_close) {
        if (browser.get() && browser->GetHost().get()) {
            browser->GetHost()->CloseBrowser(true);
        }
    }
    // Also close any docked DevTools browser — it is not in browsers_ (it is a
    // separate CEF browser parented to the DevTools dock), so without this it
    // would still be alive at CefShutdown and keep a GPU/renderer subprocess
    // mapped (the exact "folder still in use" bug we fixed for normal tabs).
    CefRefPtr<CefBrowser> dt_old;
    {
        std::lock_guard<std::mutex> lock(devtools_mutex_);
        dt_old = devtools_browser_;
        ++devtools_session_;  // stale its closures
        devtools_browser_ = nullptr;
        if (dt_old.get()) devtools_pending_.fetch_add(1);
    }
    if (dt_old.get() && dt_old->GetHost().get()) {
        dt_old->GetHost()->CloseBrowser(true);
    }
    // Drop our snapshot refs so we are not the ones keeping the browser hosts
    // (and their GPU/renderer subprocess connections) alive while we wait.
    to_close.clear();
    to_close.shrink_to_fit();

    // 2. Pump CEF until every browser has actually closed (so the GPU/renderer
    //    subprocesses are torn down before CefShutdown) or we time out. Without
    //    this drain, CefShutdown would be called with browsers still open and
    //    the child processes would be left running — the reason Windows refused
    //    to delete the app folder ("something is still running").
    const auto drain_start = std::chrono::steady_clock::now();
    int initial_count = 0;
    {
        std::lock_guard<std::mutex> lock(browsers_mutex_);
        initial_count = static_cast<int>(browsers_.size() + closing_tabs_.size());
    }
    AURORA_LOG(util::LogLevel::Info, "cef")
        << "shutdown: draining " << initial_count
        << " browser(s), devtools_pending=" << devtools_pending_.load();
    const auto deadline = drain_start + std::chrono::seconds(5);
    for (;;) {
        pump_message_loop();  // CefDoMessageLoopWork — runs OnBeforeClose on this thread.
        bool done;
        {
            std::lock_guard<std::mutex> lock(browsers_mutex_);
            done = browsers_.empty() && closing_tabs_.empty()
                   && devtools_pending_.load() == 0;
        }
        if (done) break;
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // 3. If anything is still closing after the timeout, we cannot safely pump
    //    forever (the host is trying to exit); log it and let the destructor
    //    release the remaining refs. Do NOT call CloseBrowser again here —
    //    CefShutdown is about to run.
    int remaining = 0;
    {
        std::lock_guard<std::mutex> lock(browsers_mutex_);
        remaining = static_cast<int>(browsers_.size() + closing_tabs_.size());
    }
    const int dt_pending = devtools_pending_.load();
    const auto drain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - drain_start).count();
    if (remaining + dt_pending > 0) {
        AURORA_LOG(util::LogLevel::Warn, "cef")
            << "shutdown timed out after " << drain_ms << "ms with " << remaining
            << " browser(s) and " << dt_pending
            << " devtools browser(s) still closing; releasing refs";
    } else {
        AURORA_LOG(util::LogLevel::Info, "cef")
            << "all browsers closed cleanly before CefShutdown (drain took "
            << drain_ms << "ms)";
    }
}

} // namespace aurora::cef
