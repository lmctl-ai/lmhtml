# lmhtml

## Status

Experimental. The CLI, schema, and script API are evolving; bytecode v5 and schema v2 are required.

A single self-contained native CLI for bulk HTML work (broken links, CSS
class/id typos, structural diff, shared-part extraction and rewrite), built
on lexbor and scriptable with lmscript `_dsl_` bytecode via the vendored
cpp-dsl-sdk header. See `durable-memory/` for design notes.

## Requirements

- C++17 toolchain, CMake >= 3.24, Ninja
- An lmscript compiler that emits DSL bytecode v5: `npm i -g @lmctl-ai/lmctl`
  (Node.js 24.15 or newer) puts `lmscript` on PATH; `scripts/select-compiler.sh` probes it.
  Set `LMSCRIPT_BIN` to select an explicit compiler executable. For local
  compiler-source development, set `LMSCRIPT_SOURCE` and use
  `scripts/lmscript-source.sh` directly.
- Python 3 (test assertions only)

## Setup

```sh
scripts/sync-sdk.sh      # vendor cpp/dslvm.hpp per sdk.lock
scripts/sync-lexbor.sh   # clone lexbor into third_party/ per lexbor.lock
```

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2

scripts/select-compiler.sh  # prints the compatible compiler path
lmscript examples/check-links.lms build/check-links.dslb
build/lmhtml --dsl build/check-links.dslb
```

CLI contract:

- `lmhtml --dsl-schema` — compact human/LLM summary of contexts, functions, types
- `lmhtml --dsl-schema --json` — the embedded schema bytes (compiler input)
- `lmhtml --dsl-signatures` — canonical registry signatures (parity checks)
- `lmhtml [--dry-run] --dsl A.dslb [B.dslb ...]` — run bytecode files in order
  against the one embedded schema; all files are loaded and verified before
  anything runs; shared state across files; stops at the first failure.
  With `--dry-run`, `Html.save` reports a `would-save` finding
  (`N bytes, new|changed|unchanged`) and writes nothing.

Exit codes: 0 ok, 1 lint violations or normalize --check differences,
2 usage/compile, 3 host load/run error, 4 application failure. On Linux
and Windows the default build links fully static (`-static`); disable with
`-DLMHTML_FULL_STATIC=OFF`. macOS links its platform system libraries.

## Usage guide (the five use cases)

All work is an lmscript `_dsl_` chunk compiled against `schema/lmhtml.dsl.json`
(see `examples/` for complete runnable scripts):

1. **Broken links** — `examples/check-links.lms`: `html.load_dir(dir)` →
   `Doc.links()` (a/link/img/script/srcset) → policy via
   `html.link_kind(page, url)` (`internal-page`, `internal-asset`, `anchor`,
   `external`, `special`, `outside-root`) plus `str_match(url, glob)` for
   hosting-noise patterns → `html.link_exists` → `report.add` +
   `report.write_text/json`. `load_dir(dir, include, exclude)` filters pages
   by glob on the dir-relative path (`blog/2024/a.html`): `""` or `"*"`
   include everything, exclude wins over include, `*` crosses `/`, `**/`
   also matches zero directories, a leading `./` is ignored; absolute
   patterns must lie below `dir`, and patterns leaving it are refused.
   Hidden files and dirs are included; directory symlinks are not followed;
   results are sorted by path.
2. **CSS class/id typos** — `examples/class-typos.lms`: site-wide sets in one
   call (`DocList.classes_used()` / `css_classes()` / `ids()` / `css_ids()`,
   covering inline `<style>`, linked local stylesheets and `@media` blocks);
   `str_difference` + `str_union` + `str_list` for custom set flow;
   `suggest(name, candidates)` gives edit-distance near misses; ignore
   JS-hook patterns with `str_match(name, "language-*")`. Glob syntax:
   `*` any run (including `/`), `?` one byte, `[...]` class with ranges,
   `[!...]` negated class, `\x` literal x (an unterminated `[` is literal);
   bytes, not code points; worst case is refused beyond 2^27 steps.
3. **Structural diff** — `examples/diff-pages.lms`: `html.diff(a, b)` ignores
   whitespace/comment/attribute-order/`data-*` noise and emits
   `diff-added/removed/changed` findings with CSS-ish paths; `Node.hash()` is
   the normalized subtree fingerprint.
4. **Common parts → template** — `examples/make-template.lms`:
   `html.write_template(pages, k, out)` finds subtrees shared by ≥ k pages
   and emits a sample-based template with `<!-- slot:TAG-N -->` markers;
   `common-part` findings report page coverage.
5. **Shared-part rewrite** — `examples/rewrite-nav.lms`: `select` +
   `replace_with`/`set_attr`/`insert_before`/`insert_after`/`remove`, then
   `html.save(page, path)`. Saves are atomic (temp + rename), confined to the
   working directory (the output root), and never touch inputs unless the
   script explicitly saves to the same path. Re-diff with `html.diff` to
   verify convergence; re-runs are idempotent. Use `--dry-run` first.

Serialization is mostly faithful but normalizes (verified): doctype becomes
`<!DOCTYPE html>` and whitespace before `<html>` is dropped; missing
`<html>`/`<head>`/`<body>` wrappers and optional tags are added (`<tbody>`,
`</td>`, `</p>`, ...); tag names are lowercased; attribute values are always
double-quoted with `&`, `"`, `<`, `>` and U+00A0 escaped, and boolean
attributes gain `=""` (`hidden` → `hidden=""`); `<br/>` becomes `<br>`;
character references are decoded (`&copy;` is written as UTF-8 `©`).
Indentation between elements is kept as text. lexbor does not decode legacy
encodings, so `Html.save` refuses a page whose source is not valid UTF-8
rather than writing mixed Latin-1/UTF-8 bytes.

## Test

```sh
./test.sh                            # release build + full pipeline
BUILD_DIR=build-sanitize ./test.sh   # ASan/UBSan variant
```

## License

lmhtml's own code (including the vendored `cpp/dslvm.hpp` runtime) is MIT —
see `LICENSE`. Third-party components and their licenses are listed in
`THIRD_PARTY_NOTICES` (lexbor is Apache-2.0; runtime-library notes for the
static binaries are there too).

### Validation scripts

`Doc.byte_size()` returns the original UTF-8 source size in bytes.
`Doc.doctype()` returns the leading doctype name (empty when missing or preceded
by content; comments and whitespace are ignored). `Doc.markup_errors()` returns
an explicit-tag nesting audit from lexbor tokens **before** tree repair: unlike
HTML itself, this audit requires closing tags even where HTML allows omission.
These three values describe the loaded source, not subsequent DOM mutations.
`Node.attributes()` lists attribute names in document order; `Node.path()` gives
a CSS path using element sibling positions. Paths reflect the current tree.
After writing findings, call `Report.fail_if_any()` to return a nonzero host
exit with `APP_FAIL`; an empty report succeeds. Reports otherwise remain
informational, preserving existing analysis and rewrite scripts.

### Preserving author source while replacing document shells

`Doc.source()` returns the original bytes. `Doc.source_outer(tag)` and
`Doc.source_inner(tag)` return the source of one explicit element or its content,
using lexbor token ranges (not DOM serialization). `Doc.source_elements(tag)`
returns all matching outer source slices in source order. Names are lowercase. Missing
or implicit elements return an empty string; duplicate matches and unclosed
non-void elements are errors. These are source snapshots: DOM edits do not change
them. They preserve quotes, entities, comments, UTF-8 and preformatted whitespace.
The overload `Doc.source_inner(tag, omit_tags)` removes explicit **direct child**
elements named in a space-separated list. An empty tag selects the whole source
fragment; `!doctype` names an explicit doctype declaration. Filtering uses token offsets and lexical parentage: identical HTML in
comments, raw text or nested content is preserved.

`Html.save(source_string, path)` writes those UTF-8 bytes verbatim, atomically,
within the working directory, with the same path policy and dry-run reporting
as `Html.save(doc, path)`. Equal files are not rewritten in apply mode. This lets
scripts replace a generated shell while retaining authored HTML exactly.

`str_find`, `str_replace` and `str_trim` come from the pinned cpp-dsl-sdk string
catalog (`string-catalog.lock`, `cpp/stdlib/string.hpp`); they operate on bytes.
CMake selects those three implementations verbatim so unused catalog float
parsers do not require floating-point `from_chars` on Apple libc++.
`str_text_prefix(string, count)` returns at most count UTF-8 codepoints without
splitting a character (negative counts yield empty; invalid UTF-8 is an error).
The schema and `--dsl-schema` summary include these overloads and functions.

`html.png_size(path)` reads only the 33-byte PNG signature/IHDR header from a
regular local file and returns an immutable `ImageSize` with `width()` and
`height()` integer methods. Paths are filesystem paths relative to the working
directory (or absolute), not site URLs. Missing files, non-PNG/truncated headers,
zero dimensions and dimensions above 2^31-1 raise a located DSL error. This is
metadata inspection, not full PNG/CRC or pixel-data validation. The returned
object remains valid across chunks for the host lifetime. `--dsl-schema` lists the
new methods automatically from the registry.

### Markdown export

This generic exporter is not used by lmctl skills, which are HTML only.

`Doc.markdown(selector) -> string` deterministically exports exactly one selected
content element (for example `doc.markdown("main")`). It supports h1–h4, paragraphs,
nested lists (including ordered starts), pre/code/kbd/samp, rectangular tables,
links/images, strong/em, blockquotes, hr and definition lists (terms become bold
paragraphs), plus main/body/section/article/figure containers. Code text is preserved;
fences grow to accommodate embedded backticks. Links retain their targets and titles;
call `Node.set_attr` before export if a publishing pipeline needs link mapping.
Document chrome is excluded by selecting only content. Output ends in a newline.
Unsupported tags, table spans/block cells, ambiguous selectors and non-UTF-8 input
fail clearly. This is a controlled-subset exporter, not arbitrary HTML round-tripping.

## Markdown notes, indexes and web views

Markdown input uses the vendored md4c 0.5.2 CommonMark parser with GFM tables,
task lists, strikethrough and autolinks. `md4c.lock` pins the upstream commit and
SHA256 of every source/license file; `python3 scripts/verify-md4c.py` verifies them.
There is no runtime dependency. The MIT license is included in release notices.

```sh
lmhtml md2html notes/chapter.md build/chapter.html
lmhtml md2html notes/chapter.md                    # HTML on stdout
lmhtml index notes                               # generate notes/index.md
lmhtml index notes --check                       # exit 3 on drift, no writes
lmhtml index notes --order chapter-order.txt      # one relative .md path per line
lmhtml mdcheck notes                             # existing links + anchors
lmhtml site notes build/notes --template shell.html
```

Write notes as UTF-8 Markdown: one `# Title`, a short opening paragraph, and
`##`/`###` sections. Use relative links to other notes and assets. Raw HTML is
**disabled by default** (`MD_FLAG_NOHTML`); `md2html --allow-html` or
`html.from_markdown(text, true)` enables it explicitly for trusted input. This
is a parser option, not a sanitizer for untrusted URL schemes or templates.
Fenced code retains its info language in `data-language`; generated output has
no CSS or class names. GFM tasks retain disabled checkbox inputs.

An optional initial `---` block is stripped. Only single-line `title: value` and
`summary: value` metadata are recognized; matching outer quotes are removed.
This is deliberately not a YAML parser. Missing closing `---` is an error.
Metadata overrides the first h1/paragraph for `Doc.title()` and `Doc.summary()`;
newlines and whitespace collapse to one line. Headings receive lowercase ASCII
letter/digit IDs with punctuation/whitespace replaced by hyphens, `section` as
the empty fallback, and `-2`, `-3` collision suffixes. This includes h2/h3.

`index` recursively lists .md chapters (excluding its own root index.md), with
relative percent-encoded links and escaped one-line descriptions. Files sort by
relative path; the optional order file places named chapters first and rejects
unknown/duplicate names. The result is normal Markdown starting with the single
comment `<!-- generated-by: lmhtml index -->`, followed by an index heading and
chapter links. It is suitable for `Read durable-memory/index.md first`. Repeated
runs are byte-identical and do not rewrite unchanged files.

### Templates and DSL post-processing

A template is an ordinary trusted UTF-8 HTML file with these exact placeholders:

| Placeholder | Meaning |
| --- | --- |
| `{{title}}` | Escaped title (text or quoted HTML attribute context) |
| `{{summary}}` | Escaped one-line summary (text or quoted attribute context) |
| `{{path}}` | Escaped input path, or relative output path in `site` |
| `{{content}}` | Converted body HTML, required in a custom template |
| `{{toc}}` | Generated list of links to h2/h3 headings |

Substitution happens once, so metadata containing `{{content}}` stays literal.
Do not put metadata slots in script, style, or unquoted attribute contexts.
Without a template the host emits an HTML5 document with lang, charset, viewport,
title, description and main. An example shell:

```html
<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>{{title}}</title><meta name="description" content="{{summary}}">
</head><body><nav>{{toc}}</nav><main>{{content}}</main></body></html>
```

`html.load("note.md")` and `html.load_dir("notes", "*.md", "")` return the same
Doc/DocList objects as HTML. `html.from_markdown(text)` and
`html.render_markdown(text, template_text, path)` return mutable Docs: existing
selectors, node edits, link checks and `html.save(doc, path)` are the DSL hook
for decorating head/nav/footer. `html.index_markdown(dir, order_text)` returns
the index string for `html.save`; `html.check_markdown(dir)` checks links;
`html.site(dir, out, template_text)` builds the web view. All are listed in
`--dsl-schema`. See `tests/markdown-input.py` for an executable DSL example.

`site` preserves subdirectories, copies non-Markdown assets, rewrites local
Markdown links to .html with query/anchor suffixes preserved, and creates an
index page even when index.md is absent. It checks source links before writing.
References outside the note directory remain links to the original repository
files, relative to the output page; serve the repository root when previewing.
External links are not fetched. Inputs and outputs must not overlap; symlink
entries inside the input directory are refused. An asset whose output path
collides with a converted Markdown page is an error before any files are written. All output uses the existing atomic, idempotent save path
inside the current working directory. Site builds do not delete unrelated or
previous output files: use an empty output directory when removing chapters.

## Plain structure and agent extraction

The plain profile uses headings, paragraphs, lists, definition lists, tables,
links, images and aligned code blocks. Prose has no inline code or decorative
strong/em/b/i/u/mark/small/span/font. Use diagrams and video for visual content.
The normal serializer escapes only ampersand and less-than in text (including
pre/code), and ampersand and double quote in attributes. Other punctuation is
literal UTF-8. Raw-text script/style serialization keeps their syntax intact.

```
lmhtml lint --stats notes pages
lmhtml lint --stats --json pages
lmhtml lint --stats --compare before/pages pages
lmhtml lint --config plain.conf pages
lmhtml md-lint durable-memory
lmhtml normalize pages durable-memory
lmhtml normalize --check pages
lmhtml clean saved.html --base https://example.org/docs/page.html
lmhtml clean saved.html --markdown
lmhtml clean saved.html --check
lmhtml clean https://example.org/docs/page.html
```

lint recursively reads .html/.htm/.md files, reports counts and densities per
100 whitespace-delimited words, and sorts files worst-first. JSON includes a
TOTAL record and optional before/after count and byte deltas. --compare takes a
snapshot directory or file; it does not invoke Git. Tokens are explicitly a
UTF-8 bytes/4 estimate, not a model tokenizer. Directory rules in --config are
lines of DIRECTORY METRIC MAX_COUNT; comments begin with #. Counts above the
threshold fail (exit 1). Defaults are zero. All ids are counted, but h2/h3 ids
are permitted as stable chapter anchors; other ids fail. A team may explicitly
allow one strong critical warning using a strong threshold of 1; normalize
still removes emphasis by default. This profile intentionally does not guess
whether a term deserves emphasis.

Markdown lint uses md4c's parsed emphasis and code spans, not marker searches:
underscores in identifiers, list markers and fenced code are not violations.
It rejects emphasis, inline backticks, raw HTML, emoji, trailing whitespace,
soft-wrapped paragraphs and notes without exactly one h1. The generated index
comment and front matter are metadata exceptions. Index drift and link checks
remain separate index --check and mdcheck operations.

normalize applies source-range edits to HTML, leaving unrelated source intact.
It removes decoration and inline code wrappers, class/style/data attributes,
non-heading ids, duplicate attributes and empty prose elements; canonicalizes
entities and non-breaking spaces; and unfolds prose. Code-block alignment stays
unchanged. It checks decoded text equality with insignificant whitespace folded
before each atomic save, and repeated runs are byte-idempotent. Markdown
normalization reparses and emits the plain subset, retains front matter, and
checks rendered text equality; unsupported structures fail rather than silently
losing text. The DSL exposes Doc.normalize() returning source text; pass that
text to Html.save. Doc.clean(base_url) returns extracted semantic HTML.

clean chooses the first main, then role=main, then article, then body. It removes navigation,
headers, footers, asides, forms, scripts/styles and common cookie/advertising
containers by tag, role, class/id tokens and hidden state. It strips presentation
and inline decoration, retains semantic structure and image alt text, and
resolves references with --base. It never executes JavaScript. Local file
extraction is offline and needs no runtime dependencies; URL input invokes curl
with an argument vector, follows HTTP(S) redirects and times out after 60 seconds.
This adds no runtime library dependency to the parser. Network redirects/content may change between calls;
save a mirror for reproducible results. Heuristics are not a guarantee of article
relevance and clean is not a sanitizer for arbitrary untrusted interactive HTML.

clean --check requires exactly one main and compares its serialized semantic
content with extraction, printing both on mismatch. Attribute stripping and URL
resolution are allowed; dropping content or inline decoration fails. Stats go
to stderr so stdout remains usable HTML or plain Markdown. Offline robustness
checks cover saved Jekyll, Sphinx and Rust-book mirrors; fixtures in tests/plain.py
cover removal, escaping, links, text preservation, lint counts and idempotence.
