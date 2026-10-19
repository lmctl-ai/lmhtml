#pragma once
// Normalized subtree hashing and structural tree diff over lexbor DOM trees.
// Normalization (shared by both): text trimmed and whitespace runs collapsed,
// comments/doctype dropped, attributes sorted by name, class tokens sorted,
// data-* attributes dropped, child order kept. See durable-memory/04-algorithms.md.
#include "html.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace lmhtml {

// Merkle hash (FNV-1a 64) of the normalized subtree.
uint64_t subtree_hash(lxb_dom_node_t *node);

struct DiffFinding {
  std::string kind, file, message;
  int64_t line = 0;
};

// Structural diff of two document trees; appends diff-added / diff-removed /
// diff-changed findings with CSS-ish paths like
// `html > body:nth-child(2) > p.intro:nth-child(3)`. Returns the count.
// Algorithm: lockstep compare of tag/attributes/text, children matched by LCS
// over subtree hashes; unmatched pairs with equal tags recurse (changed),
// the rest are added/removed.
size_t diff_documents(Document &a, Document &b, std::vector<DiffFinding> &out);

}
