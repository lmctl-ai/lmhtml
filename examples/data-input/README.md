# Validating Data through an existing host dry callback

This standalone example imports an attribute object from two ordinary string
parameters: format and canonical text. It does not extend lmhtml's product
schema, flags, SDK pin or the lmcode standard. Its tiny fixture schema declares
only a normal string/string -> bool function using the existing schema format.

Build target lmhtml-data-input; its command is TARGET --lmcode FILE. The existing
invocation manifest chooses dry mode before VM execution. Only the real callback
opens TARGET; the optional dry callback parses JSON and checks attribute names
and string values, then returns its preview. Registry setup opens no resources.
No callback timer, output cap, hidden argument or inferred permission is added.

example.lms uses Data.toString() as an ordinary value argument and Data.toLmcode()
in with=[...] to return the same data through the existing Json standard type.
Checking never starts the host. Dry reaches validation once with no target file;
real validates then writes once. The marker on the existing dry envelope labels
this preview; it does not promise real persistence will succeed.

Native CTest exercises valid/invalid format and domain cases and counts callbacks.
Run the actual new-header integration with an explicit fresh compiler:

    python3 tests/data-input.py build/lmhtml-data-input /path/to/mono/bin/lmscript
    python3 tests/mutate-data-input.py

The compiler must include the with header feature; old published compilers cannot
parse this new example. Set LMSCRIPT_WITH_INPUT to that compiler path during
./test.sh for the same integration, while native CTest always runs. Mutation
copies are isolated and must fail a semantic assertion after a passing baseline.
