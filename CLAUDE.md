# meerkat: notes for Claude

Read `PLAN.md` first. It has the assessment of the draft, the design decisions,
the user's answers and the BNetzA rules. `DEVPLAN.md` has the task list.
`legacy/meerkat.cpp` is the original Gemini prototype. It's kept for reference
only and isn't built.

## Privacy (important: this repo is public)
- `private/` holds the provider's contract document. It's in `.gitignore`.
  **Never** commit it, copy it elsewhere in the repo, or quote from it.
- Never put the provider name, the city, the real IP addresses or the real
  measurement data into committed files, test fixtures, issues or PRs. Fixtures
  get anonymised first (documentation IPs `192.0.2.x`/`203.0.113.x`, ISP
  "Example ISP").
- The user's contract figures and real measurements are private too: never in
  commits, fixtures, docs, issues or PRs. Examples use a made-up tariff
  (250/200/150 down, 50/40/30 up). Values to keep out are listed in the
  git-ignored `private/denylist.txt`, which a local pre-commit hook enforces.

## C++ conventions
- C++20, CMake ≥ 3.25, Linux only (Debian first). GCC and Clang.
- **Every public type, function, member and file gets a Doxygen comment.**
  - Use `/** ... */` blocks with `@brief`, `@param`, `@return`, `@throws`,
    and `@note` / `@warning` where relevant.
  - Use `///<` for trailing member comments.
  - Each file starts with `@file` and a one-line `@brief`.
  - Each source folder (namespace) has a `@namespace` block.
- Namespaces match folders: `meerkat::core`, `meerkat::sys`, `meerkat::net`,
  `meerkat::probe`, `meerkat::store`, `meerkat::report`, `meerkat::app`.
- No shell: external programs run through `sys::Process` (argv array, timeout).
- Measurements that are missing stay empty (`std::optional`), never 0.
- Timestamps are stored in UTC, ISO 8601.
- Parsers of external tool output get tested against fixtures in
  `tests/fixtures/`. Run tools with `LC_ALL=C`, because the system locale is
  German.
- Generate docs with `doxygen Doxyfile`. They go to `build/docs/` and aren't
  committed.

## Workflow
- One branch and one PR per DEVPLAN task (`task-1.2-process`). CI has to pass.
  Ask before merging. Mark the task ✅ in `DEVPLAN.md` inside the PR.
- Every task that writes code ends with `/code-review medium`.
- Faults found during a task become GitHub issues on carloslinares-eu/meerkat
  (symptom, root cause, fix) and get closed with `Fixes #N`. Labels: `bug`
  plus one of `build`, `probe`, `store`, `report`, `packaging`.
- carbon has no passwordless sudo. Package installs are done by the user.
