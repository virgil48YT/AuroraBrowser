// Aurora — C ABI over brave/adblock-rust.
//
// One engine instance per browser process, created from the combined rule
// list (starter rules + any user-supplied filter list). The C++ side
// (src/adblock/AdblockEngine.hpp) owns the lifetime and serializes access
// with its own mutex; the functions here are deliberately tiny and never
// free memory they did not allocate, so nothing but raw bytes crosses the
// boundary (important for the MSVC static-CRT staticlib mix).
//
// Written against adblock 0.8.12's public API:
//   - `Engine` is re-exported at the crate root (the `engine` module is
//     private), `ParseOptions` lives in `adblock::lists`.
//   - `Engine::from_rules(rules, ParseOptions)` takes the parse options.
//   - `Request::new` returns `Result<Request, RequestError>`.
//   - `check_network_request` returns `BlockerResult { matched, important,
//     exception, .. }`; the engine accounts for exception rules itself, so
//     `matched` is only true when the request should actually be blocked
//     (an `important` match blocks even when an exception rule exists).
//   - cosmetic (element-hiding) rules are spelled
//     `Engine::url_cosmetic_resources(url) -> UrlSpecificResources`, whose
//     serialized resource object is passed through intact (except scriptlet
//     source, which has a separate accessor). This also retains exceptions,
//     non-hide styles and attribute/class-removal actions, without baking a
//     particular version's Rust collection/action layout into the C ABI.
//
// Request types are the adblock-rust string spellings ("script", "image",
// "stylesheet", "xmlhttprequest", "subdocument", "media", "font",
// "websocket", "other", ...); the CEF layer maps Chromium resource types to
// them.
use std::ffi::{c_char, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use adblock::lists::ParseOptions;
use adblock::request::Request;
use adblock::Engine;
use serde_json::Value;

/// Create an engine from a newline-separated rule list. Comment lines ('!')
/// and blanks are ignored. Returns null only when `rules` is null or not
/// valid UTF-8; an empty rule list yields a valid engine that matches
/// nothing.
#[no_mangle]
pub unsafe extern "C" fn aurora_adblock_engine_new(
    rules: *const c_char,
) -> *mut Engine {
    if rules.is_null() {
        return ptr::null_mut();
    }
    let rules_str = match CStr::from_ptr(rules).to_str() {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };
    let list: Vec<String> = rules_str
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('!'))
        .map(str::to_owned)
        .collect();
    let engine = Engine::from_rules(list, ParseOptions::default());
    Box::into_raw(Box::new(engine))
}

/// Drop an engine created by aurora_adblock_engine_new. Null is a no-op.
#[no_mangle]
pub unsafe extern "C" fn aurora_adblock_engine_free(engine: *mut Engine) {
    if !engine.is_null() {
        drop(Box::from_raw(engine));
    }
}

/// Decide one network request.
///
/// Returns 1 when the request must be blocked, 0 when it is allowed, and -1
/// when the arguments were invalid (treated as "allow" by callers that check
/// for 1). `source_url` is the page the request is made from; an empty
/// source falls back to the request URL itself (same-origin) so the engine
/// still has a usable first-party URL.
#[no_mangle]
pub unsafe extern "C" fn aurora_adblock_check(
    engine: *const Engine,
    url: *const c_char,
    source_url: *const c_char,
    request_type: *const c_char,
) -> i32 {
    if engine.is_null() || url.is_null() || source_url.is_null() {
        return -1;
    }
    let url_s = match CStr::from_ptr(url).to_str() {
        Ok(s) => s,
        Err(_) => return -1,
    };
    let source_s = match CStr::from_ptr(source_url).to_str() {
        Ok(s) => s,
        Err(_) => return -1,
    };
    let type_s: &str = if request_type.is_null() {
        "other"
    } else {
        match CStr::from_ptr(request_type).to_str() {
            Ok(s) => s,
            Err(_) => "other",
        }
    };
    let source = if source_s.is_empty() { url_s } else { source_s };

    // An unparseable URL means the engine cannot reason about the request;
    // fail open (allow) rather than guess.
    let request = match Request::new(url_s, source, type_s) {
        Ok(r) => r,
        Err(_) => return 0,
    };
    let result = (*engine).check_network_request(&request);
    // The engine has already applied exception rules: `matched` is only set
    // when the request should be blocked. The `important` guard is belt and
    // braces — an important rule blocks even where an exception rule also
    // matched, which the engine reports through those two fields.
    if result.matched && (result.important || result.exception.is_none()) {
        1
    } else {
        0
    }
}

/// Cosmetic (element-hiding) selectors for one page URL.
///
/// Returns a freshly allocated, newline-separated list of the CSS selectors
/// the engine wants hidden on `url`; the caller owns the buffer and must
/// hand it back to `aurora_adblock_cosmetic_filters_free`. A page with no
/// cosmetic rules yields an empty (but valid) string; null means the
/// arguments were unusable.
///
/// Compatibility entry point for selector-only callers. The renderer uses
/// `aurora_adblock_cosmetic_resources` below, which does not discard the
/// other kinds of cosmetic resources.
#[no_mangle]
pub unsafe extern "C" fn aurora_adblock_cosmetic_filters(
    engine: *const Engine,
    url: *const c_char,
) -> *mut c_char {
    if engine.is_null() || url.is_null() {
        return ptr::null_mut();
    }
    let url_s = match CStr::from_ptr(url).to_str() {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };
    let resources = (*engine).url_cosmetic_resources(url_s);
    // Blank entries are dropped the same way engine_new drops them. Note a
    // selector never starts with '!': cosmetic rules were already split from
    // their `domain##` prefix by the parser, so nothing here is a comment.
    let mut selectors: Vec<&str> = resources
        .hide_selectors
        .iter()
        .map(|s| s.trim())
        .filter(|s| !s.is_empty())
        .collect();
    // A HashSet iterates in arbitrary order; sorting keeps the stylesheet the
    // C++ side builds stable between calls (and readable in devtools).
    selectors.sort_unstable();
    // An interior NUL cannot survive the C ABI; a rule list containing one is
    // treated as "no cosmetic rules" rather than truncated silently.
    match CString::new(selectors.join("\n")) {
        Ok(s) => s.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}

/// Free a string returned by aurora_adblock_cosmetic_filters. Null is a
/// no-op, and nothing else may be passed here — the allocation came from
/// Rust's CString and must go back to it.
#[no_mangle]
pub unsafe extern "C" fn aurora_adblock_cosmetic_filters_free(text: *mut c_char) {
    if !text.is_null() {
        drop(CString::from_raw(text));
    }
}

/// An opaque, Rust-owned snapshot. The accessors borrow its strings; C++
/// copies them before freeing the snapshot. No C++ allocation is ever
/// handed to CString::from_raw or Box::from_raw.
pub struct CosmeticResources {
    json: CString,
    scripts: CString,
}

impl CosmeticResources {
    fn append_script(scripts: &mut String, text: &str) {
        if text.trim().is_empty() {
            return;
        }
        if !scripts.is_empty() {
            // Separate complete programs even when a chunk has no trailing
            // semicolon or ends in a line comment (avoid ASI call chaining).
            scripts.push_str("\n;\n");
        }
        scripts.push_str(text);
    }

    fn from_value(mut value: Value) -> Option<Self> {
        let object = value.as_object_mut()?;
        let mut scripts = String::new();
        // adblock-rust calls its expanded scriptlet source `injected_script`.
        // Accept `scripts` too at this serialization boundary, while keeping
        // the ordering of a script array (dependencies must run first).
        for name in ["injected_script", "scripts"] {
            if let Some(source) = object.remove(name) {
                match source {
                    Value::String(text) => {
                        Self::append_script(&mut scripts, &text);
                    }
                    Value::Array(items) => {
                        for item in items {
                            if let Value::String(text) = item {
                                Self::append_script(&mut scripts, &text);
                            }
                        }
                    }
                    _ => {}
                }
            }
        }

        // HashSet iteration is deliberately unspecified. Stable set arrays
        // let a second injection skip rebuilding identical styles/actions.
        // Do not sort scriptlets or the steps within a procedural action.
        for name in ["hide_selectors", "exceptions", "procedural_actions"] {
            if let Some(Value::Array(items)) = object.get_mut(name) {
                if items.iter().all(Value::is_string) {
                    items.sort_unstable_by(|a, b| a.as_str().cmp(&b.as_str()));
                    items.dedup();
                }
            }
        }

        // Preserve all fields, including non_hide_styles, remove_attrs,
        // remove_classes and serialized procedural_actions when provided by
        // the engine. Missing fields are empty on the JS side; no rule is
        // reparsed or matched here, so engine exceptions remain authoritative.
        let json = serde_json::to_string(&value).ok()?;
        Some(Self {
            json: CString::new(json).ok()?,
            scripts: CString::new(scripts.trim_end()).ok()?,
        })
    }
}

/// Query all URL-specific cosmetic resources in one engine lookup.
///
/// The JSON accessor exposes the engine's complete serialized resource
/// object, including styles, removals, exceptions and generic-hide policy.
/// The scripts accessor exposes engine-expanded scriptlet JavaScript, NOT
/// raw filter-list arguments. Resolving scriptlets still requires the
/// corresponding resources to have been loaded into the engine.
///
/// Returns null for invalid arguments or failed serialization. The caller
/// must free a successful result with the matching snapshot free function.
#[no_mangle]
pub unsafe extern "C" fn aurora_adblock_cosmetic_resources(
    engine: *const Engine,
    url: *const c_char,
) -> *mut CosmeticResources {
    if engine.is_null() || url.is_null() {
        return ptr::null_mut();
    }
    let url_s = match CStr::from_ptr(url).to_str() {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };
    // Do not unwind across an extern "C" boundary if an engine query fails.
    let snapshot = catch_unwind(AssertUnwindSafe(|| {
        let resources = (*engine).url_cosmetic_resources(url_s);
        CosmeticResources::from_value(serde_json::to_value(resources).ok()?)
    }));
    match snapshot {
        Ok(Some(resources)) => Box::into_raw(Box::new(resources)),
        _ => ptr::null_mut(),
    }
}

/// Borrowed UTF-8 JSON, valid only until the snapshot is freed. Never free
/// this pointer separately (including with cosmetic_filters_free).
#[no_mangle]
pub unsafe extern "C" fn aurora_adblock_cosmetic_resources_json(
    resources: *const CosmeticResources,
) -> *const c_char {
    resources.as_ref().map_or(ptr::null(), |r| r.json.as_ptr())
}

/// Borrowed engine-expanded scriptlet source, with the same lifetime as
/// the JSON accessor. An empty string means there are no resolved scripts.
#[no_mangle]
pub unsafe extern "C" fn aurora_adblock_cosmetic_resources_scripts(
    resources: *const CosmeticResources,
) -> *const c_char {
    resources.as_ref().map_or(ptr::null(), |r| r.scripts.as_ptr())
}

/// Free ONLY a snapshot returned by aurora_adblock_cosmetic_resources.
/// Borrowed fields are dropped by Rust with their owner. Null is a no-op.
#[no_mangle]
pub unsafe extern "C" fn aurora_adblock_cosmetic_resources_free(
    resources: *mut CosmeticResources,
) {
    if !resources.is_null() {
        drop(Box::from_raw(resources));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn snapshot_keeps_styles_removals_exceptions_and_script_order() {
        let original = json!({
            "hide_selectors": [".z", ".a", ".a"],
            "exceptions": [".allowed"],
            "non_hide_styles": {".banner": ["opacity: 0 !important"]},
            "remove_attrs": {".banner": ["title", "data-ad"]},
            "remove_classes": {".banner": ["sponsored"]},
            "procedural_actions": ["{\"selector\":\".ad\",\"action\":\"remove\"}"],
            "generichide": true,
            "injected_script": "window.first = 'quoted\\nscript';",
            "scripts": ["window.second = true;", "window.third = true;"]
        });
        let snapshot = CosmeticResources::from_value(original.clone()).unwrap();
        let actual: Value =
            serde_json::from_str(snapshot.json.to_str().unwrap()).unwrap();
        assert_eq!(actual["hide_selectors"], json!([".a", ".z"]));
        for name in [
            "exceptions", "non_hide_styles", "remove_attrs", "remove_classes",
            "procedural_actions", "generichide",
        ] {
            assert_eq!(actual[name], original[name]);
        }
        assert!(actual.get("injected_script").is_none());
        assert!(actual.get("scripts").is_none());
        assert_eq!(
            snapshot.scripts.to_str().unwrap(),
            "window.first = 'quoted\\nscript';\n;\nwindow.second = true;\n;\nwindow.third = true;"
        );
    }

    #[test]
    fn json_preserves_controls_unicode_and_selector_boundaries() {
        let value = json!({"hide_selectors": ["[title=\"a\\b\n雪\"]", ".nul\u{0}"]});
        let snapshot = CosmeticResources::from_value(value).unwrap();
        let actual: Value =
            serde_json::from_str(snapshot.json.to_str().unwrap()).unwrap();
        assert!(actual["hide_selectors"].as_array().unwrap()
            .contains(&json!("[title=\"a\\b\n雪\"]")));
        assert!(actual["hide_selectors"].as_array().unwrap()
            .contains(&json!(".nul\u{0}")));
    }

    #[test]
    fn invalid_payloads_and_nul_script_source_fail_open() {
        assert!(CosmeticResources::from_value(json!([])).is_none());
        assert!(CosmeticResources::from_value(json!({
            "injected_script": "void 0;\u{0}"
        })).is_none());
        let empty = CosmeticResources::from_value(json!({
            "hide_selectors": [], "injected_script": ""
        })).unwrap();
        assert!(empty.scripts.as_bytes().is_empty());
    }

    #[test]
    fn snapshot_ffi_owns_its_buffers_and_handles_null() {
        unsafe {
            assert!(aurora_adblock_cosmetic_resources(ptr::null(), ptr::null()).is_null());
            assert!(aurora_adblock_cosmetic_resources_json(ptr::null()).is_null());
            assert!(aurora_adblock_cosmetic_resources_scripts(ptr::null()).is_null());
            aurora_adblock_cosmetic_resources_free(ptr::null_mut());

            let rules = CString::new("example.com##.ad\nexample.com##.sponsor").unwrap();
            let url = CString::new("https://example.com/").unwrap();
            let engine = aurora_adblock_engine_new(rules.as_ptr());
            assert!(!engine.is_null());
            let snapshot = aurora_adblock_cosmetic_resources(engine, url.as_ptr());
            assert!(!snapshot.is_null());
            // The snapshot, not the engine or the input buffers, owns the
            // strings. It remains readable after the engine is destroyed.
            aurora_adblock_engine_free(engine);
            let payload = CStr::from_ptr(aurora_adblock_cosmetic_resources_json(snapshot));
            let value: Value = serde_json::from_str(payload.to_str().unwrap()).unwrap();
            assert_eq!(value["hide_selectors"], json!([".ad", ".sponsor"]));
            aurora_adblock_cosmetic_resources_free(snapshot);
        }
    }
}
