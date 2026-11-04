//! This crate provides HCL language support for the [tree-sitter] parsing library.
//!
//! Typically, you will use the [`LANGUAGE`] constant to add this language to a
//! tree-sitter [`Parser`], and then use the parser to parse some code:
//!
//! ```
//! let code = r#"
//! example "test" {
//!   foo = "bar"
//! }
//! "#;
//! let mut parser = tree_sitter::Parser::new();
//! let language = tree_sitter_hcl::LANGUAGE;
//! parser
//!     .set_language(&language.into())
//!     .expect("Error loading HCL parser");
//! let tree = parser.parse(code, None).unwrap();
//! assert!(!tree.root_node().has_error());
//! ```
//!
//! [`Parser`]: https://docs.rs/tree-sitter/0.25.3/tree_sitter/struct.Parser.html
//! [tree-sitter]: https://tree-sitter.github.io/

use tree_sitter_language::LanguageFn;

extern "C" {
    fn tree_sitter_hcl() -> *const ();
}

/// The tree-sitter [`LanguageFn`] for this grammar.
pub const LANGUAGE: LanguageFn = unsafe { LanguageFn::from_raw(tree_sitter_hcl) };

/// The content of the [`node-types.json`] file for this grammar.
///
/// [`node-types.json`]: https://tree-sitter.github.io/tree-sitter/using-parsers/6-static-node-types
pub const NODE_TYPES: &str = include_str!("../../src/node-types.json");

#[cfg(test)]
mod tests {
    #[test]
    fn test_can_load_grammar() {
        let mut parser = tree_sitter::Parser::new();
        parser
            .set_language(&super::LANGUAGE.into())
            .expect("Error loading HCL parser");
    }
    fn parse(source: &str) -> tree_sitter::Tree {
        let mut parser = tree_sitter::Parser::new();
        parser.set_language(&super::LANGUAGE.into()).unwrap();
        parser.parse(source, None).unwrap()
    }

    fn boundary_ranges(node: tree_sitter::Node<'_>, ranges: &mut Vec<(String, usize, usize)>) {
        if matches!(
            node.kind(),
            "template_literal"
                | "template_interpolation"
                | "template_directive"
                | "heredoc_identifier"
                | "block"
        ) {
            ranges.push((node.kind().to_owned(), node.start_byte(), node.end_byte()));
        }
        let mut cursor = node.walk();
        for child in node.named_children(&mut cursor) {
            boundary_ranges(child, ranges);
        }
    }

    #[test]
    fn quoted_literal_boundaries_keep_named_byte_ranges() {
        let source = r#"plain = "alpha  beta  "
value = "left \\ \" \n \r \t \u0041 \U0001F600 café $%$% $${escaped} %%{escaped} ${var.name} %{ if true }yes%{ else }no%{ endif } right  "
resource "test" "quoted_after" {}
"#;
        let tree = parse(source);
        assert!(!tree.root_node().has_error());
        let mut ranges = Vec::new();
        boundary_ranges(tree.root_node(), &mut ranges);
        ranges.sort();
        let expected = vec![
            ("block".to_owned(), 164, 197),
            ("template_directive".to_owned(), 118, 154),
            ("template_interpolation".to_owned(), 106, 117),
            ("template_literal".to_owned(), 9, 20),
            ("template_literal".to_owned(), 33, 105),
            ("template_literal".to_owned(), 130, 133),
            ("template_literal".to_owned(), 142, 144),
            ("template_literal".to_owned(), 155, 160),
            ("template_literal".to_owned(), 174, 178),
            ("template_literal".to_owned(), 181, 193),
        ];
        assert_eq!(ranges, expected);
    }

    #[test]
    fn heredoc_literal_boundaries_keep_named_byte_ranges() {
        let source = r#"value = <<-END
  literal 本文 $%$% and $$$ %% $${escaped} %%{escaped}
  ${var.name} then %{ if true }body%{ else }other%{ endif }
  END-suffix
  END
resource "test" "heredoc_after" {}
"#;
        let tree = parse(source);
        assert!(!tree.root_node().has_error());
        let mut ranges = Vec::new();
        boundary_ranges(tree.root_node(), &mut ranges);
        ranges.sort();
        let expected = vec![
            ("block".to_owned(), 151, 185),
            ("heredoc_identifier".to_owned(), 11, 14),
            ("heredoc_identifier".to_owned(), 147, 150),
            ("template_directive".to_owned(), 91, 131),
            ("template_interpolation".to_owned(), 74, 85),
            ("template_literal".to_owned(), 17, 71),
            ("template_literal".to_owned(), 86, 90),
            ("template_literal".to_owned(), 103, 107),
            ("template_literal".to_owned(), 116, 121),
            ("template_literal".to_owned(), 134, 144),
            ("template_literal".to_owned(), 161, 165),
            ("template_literal".to_owned(), 168, 181),
        ];
        assert_eq!(ranges, expected);
    }

    #[test]
    fn embedded_nul_is_not_absorbed_into_a_literal_run() {
        let tree = parse("value = \"before\0after\"\nresource \"test\" \"after\" {}\n");
        assert!(tree.root_node().has_error());
    }
    // This prefix matches the bundled parser.h TSLexer, including unused callbacks.
    #[repr(C)]
    struct RawLexer {
        lookahead: i32,
        result_symbol: u16,
        advance: unsafe extern "C" fn(*mut RawLexer, bool),
        mark_end: unsafe extern "C" fn(*mut RawLexer),
        get_column: unsafe extern "C" fn(*mut RawLexer) -> u32,
        is_at_included_range_start: unsafe extern "C" fn(*const RawLexer) -> bool,
        eof: unsafe extern "C" fn(*const RawLexer) -> bool,
        log: Option<unsafe extern "C" fn(*const RawLexer, *const std::ffi::c_char, ...)>,
    }

    #[repr(C)]
    struct IdentifierLexer {
        raw: RawLexer,
        characters: Vec<char>,
        position: usize,
    }

    unsafe extern "C" fn advance_identifier(raw: *mut RawLexer, _skip: bool) {
        // SAFETY: scan receives a whole IdentifierLexer pointer cast to its
        // repr(C) first field, and calls synchronously without retaining it.
        let lexer = unsafe { &mut *raw.cast::<IdentifierLexer>() };
        if lexer.position < lexer.characters.len() {
            lexer.position += 1;
        }
        lexer.raw.lookahead = lexer
            .characters
            .get(lexer.position)
            .copied()
            .unwrap_or('\0') as i32;
    }

    unsafe extern "C" fn mark_identifier_end(_raw: *mut RawLexer) {}
    unsafe extern "C" fn identifier_column(_raw: *mut RawLexer) -> u32 {
        0
    }
    unsafe extern "C" fn identifier_range_start(_raw: *const RawLexer) -> bool {
        false
    }
    unsafe extern "C" fn identifier_eof(raw: *const RawLexer) -> bool {
        // SAFETY: same whole-object pointer and synchronous lifetime as advance.
        let lexer = unsafe { &*raw.cast::<IdentifierLexer>() };
        lexer.position == lexer.characters.len()
    }

    extern "C" {
        fn tree_sitter_hcl_external_scanner_create() -> *mut std::ffi::c_void;
        fn tree_sitter_hcl_external_scanner_scan(
            scanner: *mut std::ffi::c_void,
            lexer: *mut RawLexer,
            valid_symbols: *const bool,
        ) -> bool;
        fn tree_sitter_hcl_external_scanner_serialize(
            scanner: *mut std::ffi::c_void,
            buffer: *mut std::ffi::c_char,
        ) -> u32;
        fn tree_sitter_hcl_external_scanner_destroy(scanner: *mut std::ffi::c_void);
    }

    fn serialized_identifier(identifier: &str) -> Vec<u8> {
        let characters: Vec<_> = identifier.chars().collect();
        let mut lexer = IdentifierLexer {
            raw: RawLexer {
                lookahead: characters.first().copied().unwrap_or('\0') as i32,
                result_symbol: u16::MAX,
                advance: advance_identifier,
                mark_end: mark_identifier_end,
                get_column: identifier_column,
                is_at_included_range_start: identifier_range_start,
                eof: identifier_eof,
                log: None,
            },
            characters,
            position: 0,
        };
        let mut symbols = [false; 8];
        symbols[7] = true; // HEREDOC_IDENTIFIER in both scanner token enums.
        let mut buffer = [0u8; 1024]; // TREE_SITTER_SERIALIZATION_BUFFER_SIZE.
                                      // SAFETY: the existing scanner callbacks own this opaque allocation.
        let scanner = unsafe { tree_sitter_hcl_external_scanner_create() };
        assert!(!scanner.is_null());
        // SAFETY: all callback pointers match parser.h; the whole lexer and
        // symbol/buffer arrays live through these synchronous calls. The
        // scanner is destroyed before checking results, including failure.
        let (accepted, length) = unsafe {
            let accepted = tree_sitter_hcl_external_scanner_scan(
                scanner,
                (&mut lexer as *mut IdentifierLexer).cast(),
                symbols.as_ptr(),
            );
            let length =
                tree_sitter_hcl_external_scanner_serialize(scanner, buffer.as_mut_ptr().cast());
            tree_sitter_hcl_external_scanner_destroy(scanner);
            (accepted, length as usize)
        };
        assert!(accepted);
        assert_eq!(lexer.raw.result_symbol, 7);
        assert!(length <= buffer.len());
        buffer[..length].to_vec()
    }

    #[test]
    fn long_heredoc_state_contains_known_sha256_vectors() {
        // 1,000 A bytes is an upstream known vector. The other lengths cover
        // both padding branches and a complete 64-byte input chunk.
        let cases = [
            (
                33,
                "5d873590851b7b00b60490c8e6966b3409c385adcc9590d801f0e03e268b5ba5",
            ),
            (
                63,
                "1b58d00f5b1fbd2a1884d666a2be33c2fa7463dff32cd60ef200c0f750a6b70f",
            ),
            (
                64,
                "d53eda7a637c99cc7fb566d96e9fa109bf15c478410a3f5eb4d4c4e26cd081f6",
            ),
            (
                1000,
                "c2e686823489ced2017f6059b8b239318b6364f6dcd835d0a519105a1eadd6e4",
            ),
        ];
        for (length, digest) in cases {
            let expected: Vec<_> = digest
                .as_bytes()
                .chunks_exact(2)
                .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
                .collect();
            let state = serialized_identifier(&"A".repeat(length));
            assert!(
                state.len() <= 64,
                "length {length}: {} state bytes",
                state.len()
            );
            assert!(
                state.ends_with(&expected),
                "length {length}: digest mismatch"
            );
        }
        let exact = serialized_identifier(&"A".repeat(32));
        assert!(exact.ends_with(&[b'A'; 32]));
    }

    #[test]
    fn long_heredoc_matching_preserves_templates_and_following_declarations() {
        for delimiter in [
            "A".repeat(32),
            "A".repeat(33),
            "A".repeat(1011),
            "界".repeat(20),
        ] {
            let mut other = delimiter.clone();
            let _ = other.pop();
            other.push(if delimiter.is_ascii() { 'B' } else { '海' });
            let source = format!(
                "value = <<{delimiter}\n{other}\n{delimiter}${{x}}\n${{\"\"}}\n{delimiter}\nresource \"test\" \"after\" {{}}\n"
            );
            let tree = parse(&source);
            assert!(
                !tree.root_node().has_error(),
                "{delimiter}: {}",
                tree.root_node().to_sexp()
            );
            let mut ranges = Vec::new();
            boundary_ranges(tree.root_node(), &mut ranges);
            assert_eq!(
                ranges
                    .iter()
                    .filter(|(kind, _, _)| kind == "template_interpolation")
                    .count(),
                2
            );
            let blocks: Vec<_> = ranges
                .iter()
                .filter(|(kind, _, _)| kind == "block")
                .collect();
            assert_eq!(blocks.len(), 1);
            assert_eq!(
                &source[blocks[0].1..blocks[0].2],
                "resource \"test\" \"after\" {}"
            );
        }
    }
}
