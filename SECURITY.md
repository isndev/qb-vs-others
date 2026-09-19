# Security policy

This repository is a benchmark harness: it builds the frameworks under test from source, runs them
locally and writes result documents. It exposes no service, stores no credentials, and its result
documents are plain JSON and Markdown. There is no supported version to patch; the tree at `main`
is what is published.

If you find something that matters anyway — a script that executes what it fetches without a way
to read it first, a path that escapes the tree, a result document that could carry an executable
payload — report it privately through
[GitHub's private vulnerability reporting](https://github.com/isndev/qb-vs-others/security/advisories/new)
rather than an issue. A vulnerability in one of the frameworks under test belongs to that
framework's own policy; qb's is [isndev/qb/SECURITY.md](https://github.com/isndev/qb/blob/main/SECURITY.md).
