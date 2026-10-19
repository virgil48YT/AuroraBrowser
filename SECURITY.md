# Security policy

## Supported versions

Only the **latest release** and the current `main` branch receive security
fixes. Aurora tracks the latest stable CEF/Chromium; an engine vulnerability is
fixed by updating CEF, so please check that the issue reproduces on the newest
release before reporting.

## Reporting a vulnerability

Please report vulnerabilities **privately** — do not open a public issue, pull
request or discussion.

1. Use GitHub's private reporting: **Security > Report a vulnerability** on
   <https://github.com/virgil48YT/AuroraBrowser/security/advisories/new>.
2. Include the Aurora version and platform, a description of the impact, and
   steps or a proof of concept to reproduce. Mark anything that must stay
   confidential.

What to expect:

- an acknowledgement within **7 days**;
- an assessment and, for confirmed issues, a fix or mitigation plan within
  **30 days** (sooner for actively exploited issues);
- credit in the release notes and the advisory, unless you prefer to stay
  anonymous.

Please give us a reasonable time to ship a fix before disclosing publicly
(we aim for 90 days at most) and do not access other people's data, degrade
service, or test against users who have not consented.

## Scope

In scope: the Aurora desktop browser (Qt UI, CEF integration, internal
`aurora://` pages, ad blocker, cookie and incognito handling, the bootstrap
launcher and packaging) and the Android edition.

Out of scope: vulnerabilities in Chromium/CEF itself that are not caused by how
Aurora embeds them (report those to the
[Chromium project](https://www.chromium.org/Home/chromium-security/reporting-security-bugs/)),
and issues that need a compromised OS or physical access to an unlocked
machine. The Tor incognito mode depends on a Tor daemon you run yourself; its
documented limits (README) are not vulnerabilities, but leaks that bypass the
proxy are.

The security model is documented in [docs/security.md](docs/security.md).
