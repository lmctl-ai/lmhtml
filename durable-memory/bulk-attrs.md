# Bulk attributes

Node.set_attrs(StrMap) -> Node validates the node and names before applying the batch. A validation error makes no DOM changes; this is not a transaction guarantee for unrelated native allocation failure. Values are strings, keys retain normal HTML attribute-name semantics, and set_attr uses the same validator. There is no batch or value-size quota.

Node.get_attrs(StrList) -> StrMap copies present keys in request order, coalesces duplicates and preserves empty strings. The registry standard collection arena owns the result; it cannot alias DOM storage. The getter has a passive dry callback, while the setter dry preview returns its receiver without writing. Both handle a skipped/null acquisition.

The authoritative schema is schema/lmhtml.host.json; CMake regenerates lmhtml.dsl.json. Native test tests/bulk_attrs.cpp checks state after a bad second key and getter ownership/Unicode/empty/large values; tests/bulk-attrs.lms exercises real bytecode. Both run in test.sh and CI.
