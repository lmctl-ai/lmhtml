# Examples

- `check-links.lms` Classify links and report broken local destinations in a three-page synthetic site.
- `class-typos.lms` Compare classes and IDs used by pages with the names defined in their styles.
- `diff-pages.lms` Report structural changes between two HTML pages while ignoring formatting noise.
- `make-template.lms` Find markup shared by five pages and write a reusable template with slots.
- `rewrite-nav.lms` Replace shared navigation and footer markup, save copies, and verify that the pages converge.

Run `./test.sh` to check each report or generated file against its expected output. The checked-in preview cards in `examples/renders/` show the corresponding verified output.

`extract-table.lms` reads saved HTML with a CSS row selector and emits JSON with
the caller's source URL and arrays of cell values.

```sh
lmscript examples/extract-table.lms saved.html '#results > tbody > tr' 'https://example.invalid/search?page=2'
```

Put the built `lmhtml` on PATH. Use a selector that matches only the desired
body rows. `Node.text()` decodes entities and includes descendant text. The
example collapses whitespace, keeps empty cells and preserves row order. It
expects ordinary rows of `td` cells and leaves header mapping and `rowspan`
or `colspan` interpretation to the caller.
