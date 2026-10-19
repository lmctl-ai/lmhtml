# Schema v0

Form: `"dsl.schema": 2` (array-of-overloads signatures), bytecode v5 — the form
the **current** compiler at `~/lmctl-mono/lmscript/dsl.ts` accepts
(`DSL_VERSION = 5`, schema-1 singletons refused). Validated for real, not
guessed: a scratch copy of lmcad's adapter approach (Node
`--experimental-strip-types`, importing `dsl.ts` directly) ran
`parseDslSchema` + `compileDsl` on the exact JSON below plus two chunks
(explicit loops and `foreach` over a `NodeList`): **schema OK, 37 canonical
signatures, chunks compile to DSLB v5 bytecode** (1193 and 1024 bytes).

## Host/script split

- **Script expresses policy**: which directory, which selectors, thresholds
  `k`, what replacement HTML, which findings to emit, output paths.
- **Host owns lifecycle and heavy algorithms**: parsing, serialization, URL
  resolution, file existence, edit distance, subtree hashing, tree diff,
  common-subtree detection, template emission, report formatting. Each is one
  schema call; the DSL never loops over characters or bytes.
- Context objects: `html: Html` (facade for loading/algorithms) and
  `report: Report` (findings sink). Two contexts, like lmor-dsl's `cp`/`out`.

## Types and collections

`NodeList`, `StrList`, `DocList` are plain host object types with
`len() -> int` + `at(int) -> T` — the compiler lowers `foreach` onto that
protocol (verified above). No bytecode collections exist (R31).

## Use-case coverage map

1. **Broken links**: `html.load_dir` → `Doc.links()` → `html.link_exists` /
   `html.resolve` → `report.add` / `report.write_*`.
2. **Class/id typos**: `Doc.classes_used()` vs `Doc.css_classes()` (from
   inline `<style>` via lexbor's CSS parser; external CSS deferred),
   `Doc.ids()`, `suggest(name, candidates)` for edit-distance near misses.
3. **Structural diff**: `html.diff(a, b) -> int` (findings count; details go
   to `report`), `Node.hash()` for script-side subtree fingerprints.
4. **Common parts + template**: `html.write_template(docs, k, path) -> int`
   (host-side normalized-subtree hashing across pages; slots for the rest).
5. **Shared-part rewrite**: `Node.select` / `Node.replace_with(string)` /
   `Node.set_attr` / `Node.remove` + `html.save`.

## The schema (schema/lmhtml.dsl.json, 2759 bytes, as validated)

```json
{
  "dsl.schema": 2,
  "name": "lmhtml",
  "types": {
    "Doc": {
      "methods": {
        "select": [{ "params": ["string"], "returns": "NodeList" }],
        "path": [{ "params": [], "returns": "string" }],
        "links": [{ "params": [], "returns": "StrList" }],
        "classes_used": [{ "params": [], "returns": "StrList" }],
        "css_classes": [{ "params": [], "returns": "StrList" }],
        "ids": [{ "params": [], "returns": "StrList" }]
      }
    },
    "Node": {
      "methods": {
        "tag": [{ "params": [], "returns": "string" }],
        "attr": [{ "params": ["string"], "returns": "string" }],
        "has_attr": [{ "params": ["string"], "returns": "bool" }],
        "set_attr": [{ "params": ["string", "string"], "returns": "self" }],
        "text": [{ "params": [], "returns": "string" }],
        "outer_html": [{ "params": [], "returns": "string" }],
        "parent": [{ "params": [], "returns": "Node" }],
        "select": [{ "params": ["string"], "returns": "NodeList" }],
        "remove": [{ "params": [], "returns": "void" }],
        "replace_with": [{ "params": ["string"], "returns": "Node" }],
        "hash": [{ "params": [], "returns": "int" }]
      }
    },
    "NodeList": {
      "methods": {
        "len": [{ "params": [], "returns": "int" }],
        "at": [{ "params": ["int"], "returns": "Node" }]
      }
    },
    "StrList": {
      "methods": {
        "len": [{ "params": [], "returns": "int" }],
        "at": [{ "params": ["int"], "returns": "string" }]
      }
    },
    "DocList": {
      "methods": {
        "len": [{ "params": [], "returns": "int" }],
        "at": [{ "params": ["int"], "returns": "Doc" }]
      }
    },
    "Html": {
      "methods": {
        "load": [{ "params": ["string"], "returns": "Doc" }],
        "load_dir": [{ "params": ["string"], "returns": "DocList" }],
        "parse": [{ "params": ["string"], "returns": "Doc" }],
        "save": [{ "params": ["Doc", "string"], "returns": "void" }],
        "resolve": [{ "params": ["Doc", "string"], "returns": "string" }],
        "link_exists": [{ "params": ["Doc", "string"], "returns": "bool" }],
        "diff": [{ "params": ["Doc", "Doc"], "returns": "int" }],
        "write_template": [{ "params": ["DocList", "int", "string"], "returns": "int" }]
      }
    },
    "Report": {
      "methods": {
        "add": [{ "params": ["string", "string", "int", "string"], "returns": "void" }],
        "write_text": [{ "params": ["string"], "returns": "void" }],
        "write_json": [{ "params": ["string"], "returns": "void" }]
      }
    }
  },
  "functions": {
    "suggest": [{ "params": ["string", "StrList"], "returns": "StrList" }]
  },
  "context": {
    "html": "Html",
    "report": "Report"
  }
}
```

## Member count and deliberate omissions

35 host members (9 types/functions entries, 37 signature lines incl. 2
context declarations). Deferred to v1+, consciously:

- `insert_before` / `insert_after` (v0: `replace_with` + `set_attr` cover the
  header/footer rewrite case).
- `Node.children` (scoped `select("*")` works), `Node.inner_html`,
  `replace_with(Node)` (string form suffices; the script builds HTML).
- Iterating findings (`Report.at`): scripts produce findings, they don't
  consume them.
- http(s) fetching, external CSS files, `<img src>` in `links()` — see
  05-plan.md for what each milestone adds.

## Quirk: line numbers

lexbor's DOM does **not** retain source line/column. `Report.add`'s `line`
parameter exists for the JSON shape; the host passes `0` (unknown) in v0.
Byte-offset tracking is possible later via a custom parse callback — open
item, not promised.
