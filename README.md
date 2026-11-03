# tree-sitter-hcl

tree-sitter grammar for the [HCL](https://github.com/hashicorp/hcl/blob/main/hclsyntax/spec.md) language

## Maintained fork

This [fork](https://github.com/githits-com/tree-sitter-hcl) retains upstream's
bindings and grammar structure, with narrow fixes for Unicode and long heredoc
delimiters and integer-mantissa exponents such as `1e10`. It is consumed by a
full Git revision; no separate registry release is required.

The Rust `tree_sitter_hcl::LANGUAGE` binding parses HCL syntax, including native
Terraform configuration. The `terraform` grammar entry point uses the same
grammar under another exported name. Both scanners receive the same fixes and
every corpus case selects both languages. Parsing does not validate Terraform
provider schemas or evaluate configuration.

Heredoc identifiers use the generated grammar's Unicode identifier ranges and
UTF-8 bytes rather than locale-dependent character classification. Digit-first
and other non-identifier starts are rejected. The scanner still skips whitespace
before an opener, including newlines; it does not validate opener line shape.

The old `CHAR_MAX` bound was unrelated to the C scanner's `uint32_t` serialized
lengths; removing it makes supported long delimiters independent of the platform's
plain-char signedness. See upstream
[issue #78](https://github.com/tree-sitter-grammars/tree-sitter-hcl/issues/78).

External scanner state must fit in Tree-sitter's 1,024-byte serialization buffer,
including delimiter bytes and context metadata. An opener whose delimiter alone
cannot fit is refused, yielding a parse error rather than a phantom declaration.
Deeply nested templates can still exceed the combined state limit and produce
error or mis-bounded trees. This fork does not claim arbitrary delimiter lengths
or nesting depths. It preserves the state layout rather than adopting upstream
[PR #79](https://github.com/tree-sitter-grammars/tree-sitter-hcl/pull/79), which also
changes serialization density. The separate retained-memory report in
[issue #77](https://github.com/tree-sitter-grammars/tree-sitter-hcl/issues/77) remains
relevant for large or deeply nested templates; no memory improvement is claimed.

## Regenerating the fork

Use Tree-sitter CLI **0.25.6**, matching CI, and regenerate both entry points:

```bash
tree-sitter generate
(cd dialects/terraform && tree-sitter generate)
```

The two `identifier_ranges.h` files mirror the generated
`sym_identifier_character_set_1` and `_2` tables as `hcl_identifier_start` and
`hcl_identifier_continue`. After regeneration, refresh the copied ranges if
needed and run this Bash sync check; no diff is expected:

```bash
for grammar in . dialects/terraform; do
  diff -u \
    <(sed -n '/^static const TSCharacterRange sym_identifier_character_set_[12]\[\] = {/,/^};/p' "$grammar/src/parser.c" | sed -e 's/sym_identifier_character_set_1/hcl_identifier_start/' -e 's/sym_identifier_character_set_2/hcl_identifier_continue/') \
    <(sed -n '/^static const TSCharacterRange hcl_identifier_.*\[\] = {/,/^};/p' "$grammar/src/identifier_ranges.h")
done
cmp src/identifier_ranges.h dialects/terraform/src/identifier_ranges.h
```

Run `tree-sitter test --debug-build` and `cargo test` for local correctness.
The existing CI matrix verifies generated output, both grammar entry points,
bindings and sample files; scanner changes also run the existing fuzz workflow.
Node binding tests use the `tree-sitter` 0.25.1 dev dependency, whose C++20 build
selection supports current Node headers. The consumer's optional peer range
stays unchanged. The upstream playground below does not include these fork fixes.

## Try It Out

Try the parser in the [playground](https://tree-sitter-grammars.github.io/tree-sitter-hcl/)

## Example

Highlighting `example/example.hcl`:

![Highlighting Example](https://i.imgur.com/yUJ0ybK.png)

## Developing

It is recommended to use `nix` to fulfill all development dependencies. To activate the development environment simply run `nix-shell` in the project root.

## Running Tests

To run tests simply run `nix-shell --run 'tree-sitter test'`.

## Compliance

The directory `example/real_world_stuff` contains a corpus of hcl files that I found with the github query `language:HCL` for users `coreos`, `hashicorp`, `oracle` and `terraform-community-modules`.

```bash
tree-sitter parse --quiet --stat example/real_world_stuff/*/*

Total parses: 1892; successful parses: 1892; failed parses: 0; success percentage: 100.00%
```

## Fuzzing

See the [fuzzing repo for this parser](https://github.com/MichaHoffmann/tree-sitter-hcl-fuzz)

## Attributions

Pages were copied from https://github.com/m-novikov/tree-sitter-sql
