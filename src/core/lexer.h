#ifndef SNINJA_LEXER_H_
#define SNINJA_LEXER_H_

#include <string>
#include <string_view>

// Windows may #define ERROR.
#ifdef ERROR
#undef ERROR
#endif

struct EvalString;

/// A lexer for build.ninja files.
///
/// The token-level DFA of the original re2c-generated lexer is
/// hand-transcribed here; the bulk character scanning (identifier runs,
/// text runs, whitespace runs) is delegated to the SIMD scanners in
/// scan.h.  Semantics, error strings and error positions are kept
/// byte-identical to the reference implementation.
struct Lexer {
  Lexer() {}
  /// Helper ctor useful for tests.
  explicit Lexer(const char* input);

  enum Token {
    ERROR,
    BUILD,
    COLON,
    DEFAULT,
    EQUALS,
    IDENT,
    INCLUDE,
    INDENT,
    NEWLINE,
    PIPE,
    PIPE2,
    PIPEAT,
    POOL,
    RULE,
    SUBNINJA,
    TEOF,
  };

  /// Return a human-readable form of a token, used in error messages.
  static const char* TokenName(Token t);

  /// Return a human-readable token hint, used in error messages.
  static const char* TokenErrorHint(Token expected);

  /// If the last token read was an ERROR token, provide more info
  /// or the empty string.
  std::string DescribeLastError();

  /// Start parsing some input.
  void Start(std::string_view filename, std::string_view input);

  /// Read a Token from the Token enum.
  Token ReadToken();

  /// Rewind to the last read Token.
  void UnreadToken();

  /// If the next token is \a token, read it and return true.
  bool PeekToken(Token token);

  /// Read a simple identifier (a rule or variable name).
  /// Returns false if a name can't be read.
  bool ReadIdent(std::string* out);

  /// Read a path (complete with $escapes).
  /// Returns false only on error, returned path may be empty if a delimiter
  /// (space, newline) is hit.
  bool ReadPath(EvalString* path, std::string* err) {
    return ReadEvalString(path, true, err);
  }

  /// Read the value side of a var = value line (complete with $escapes).
  /// Returns false only on error.
  bool ReadVarValue(EvalString* value, std::string* err) {
    return ReadEvalString(value, false, err);
  }

  /// Construct an error message with context.
  bool Error(const std::string& message, std::string* err);

  /// Parsed 'ninja_required_version' from the manifest.
  int manifest_version_major = 0;
  int manifest_version_minor = 0;

 private:
  /// Skip past whitespace (called after each read token/ident/etc.).
  void EatWhitespace();

  /// Read a $-escaped string.
  bool ReadEvalString(EvalString* eval, bool path, std::string* err);

  std::string_view filename_;
  std::string_view input_;
  const char* end_;
  const char* ofs_;
  const char* last_token_;

  /// Holds true if ninja_required_version checked for $^ (newline) escape.
  bool newline_version_checked_ = false;
};

#endif  // SNINJA_LEXER_H_
