# Examples

- `check-links.lms` Classify links and report broken local destinations in a three-page synthetic site.
- `class-typos.lms` Compare classes and IDs used by pages with the names defined in their styles.
- `diff-pages.lms` Report structural changes between two HTML pages while ignoring formatting noise.
- `make-template.lms` Find markup shared by five pages and write a reusable template with slots.
- `rewrite-nav.lms` Replace shared navigation and footer markup, save copies, and verify that the pages converge.

Run `./test.sh` to check each report or generated file against its expected output. The checked-in preview cards in `examples/renders/` show the corresponding verified output.
