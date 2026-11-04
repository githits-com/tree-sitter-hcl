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
}
