#!/usr/bin/env python3
"""Build an offline browser regression fixture from the actual C++ JS builders.

Usage: python3 tests/build_adblock_script_fixture.py /absolute/output/directory
Requires a C++20 compiler, not CEF, Rust, npm, or any downloaded test dependency.
Open the emitted adblock-script-tests.html in Chromium. Results are also
available as window.auroraAdblockTestResults for an automated browser runner.
"""

import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
adapter = (root / "src/cef/CefAdapter.cpp").read_text()
quote = adapter[adapter.index("std::string javascript_string("):
                adapter.index("// --- Cosmetic filtering")]
cosmetic = adapter[adapter.index("std::string cosmetic_filter_script("):
                   adapter.index("// --- YouTube ad skipping")]
youtube = adapter.split('constexpr char kYoutubeAdskipScript[] = R"JS(\n', 1)[1].split(')JS";', 1)[0]

generator = r"""
#include "adblock/AdblockEngine.hpp"
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
namespace aurora::cef {
""" + quote + cosmetic + r"""
}
int main(int argc, char** argv) {
    const std::string input{std::istreambuf_iterator<char>(std::cin),
                            std::istreambuf_iterator<char>()};
    if (argc > 1 && std::string_view(argv[1]) == "scriptlets") {
        std::cout << aurora::cef::cosmetic_scriptlet_script(input);
    } else {
        aurora::adblock::CosmeticResources resources;
        resources.json = input;
        std::cout << aurora::cef::cosmetic_filter_script(resources, {}, false);
    }
}
"""
(out / "generate_cosmetic_fixture.cpp").write_text(generator)
subprocess.run([
    "g++", "-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion",
    "-Wsign-conversion", "-Wshadow", "-Werror", "-I" + str(root / "src"),
    str(out / "generate_cosmetic_fixture.cpp"), "-o", str(out / "generate_cosmetic_fixture"),
], check=True)


def generate(value, mode="cosmetics"):
    return subprocess.run([str(out / "generate_cosmetic_fixture"), mode],
                          input=value, text=True, capture_output=True,
                          check=True).stdout


fixture = {
    "cosmetic": generate(json.dumps({
        "hide_selectors": ['[data-escape="quote\\" and slash\\\\ and 雪"]'],
    })),
    "scriptlet": generate("window.__scriptletRuns=(window.__scriptletRuns||0)+1;", "scriptlets"),
    "throwingScriptlet": generate("throw new Error('isolated scriptlet');", "scriptlets"),
    "youtube": youtube,
}
for name, script in fixture.items():
    (out / (name + ".js")).write_text(script)
(out / "script-fixtures.json").write_text(json.dumps(fixture, ensure_ascii=False))
harness = (root / "tests/test_adblock_scripts.js").read_text()
payload = json.dumps(fixture, ensure_ascii=True).replace("<", "\\u003c")
html = """<!doctype html>
<meta charset="utf-8">
<title>Aurora adblock offline regression tests</title>
<h1>Aurora adblock offline regression tests</h1>
<pre id="results">Running local fixtures…</pre>
<script>
""" + harness + "\nconst fixtures=" + payload + """;
auroraRunAdblockTests(fixtures).then(function(results){
  window.auroraAdblockTestResults=results;
  document.getElementById("results").textContent=JSON.stringify(results,null,2);
});
</script>
"""
(out / "adblock-script-tests.html").write_text(html)
print(out / "adblock-script-tests.html")
