# Public Suffix List

`public_suffix_list.dat` is the Public Suffix List from
<https://publicsuffix.org/list/public_suffix_list.dat>, used verbatim
(Mozilla Public License 2.0, see the header of the file).

Aurora compiles it into the browser as a Qt resource (`:/psl/`) and installs it
at startup (`privacy::install_public_suffix_list`, `src/privacy/CookieSite.*`)
to decide registrable domains for third-party cookie blocking. The unit tests
parse this exact file (`tests/test_cookie_site.cpp`).

Update by re-downloading from the URL above (and only from there, as the file
header asks) and running the test suite. The `VERSION` / `COMMIT` lines in the
header record which snapshot is bundled.
