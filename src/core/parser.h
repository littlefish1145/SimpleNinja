#ifndef SNINJA_PARSER_H_
#define SNINJA_PARSER_H_

#include <string>

#include "lexer.h"

struct FileReader;
struct State;

/// Base class for parsers.
struct Parser {
  Parser(State* state, FileReader* file_reader)
      : state_(state), file_reader_(file_reader) {}
  virtual ~Parser() {}

  /// Load and parse a file.
  bool Load(const std::string& filename, std::string* err, Lexer* parent = NULL);

 protected:
  /// If the next token is not \a expected, produce an error string
  /// saying "expected foo, got bar".
  bool ExpectToken(Lexer::Token expected, std::string* err);

  State* state_;
  FileReader* file_reader_;
  Lexer lexer_;

 private:
  /// Parse a file, given its contents as a string.
  virtual bool Parse(const std::string& filename, const std::string& input,
                     std::string* err) = 0;
};

#endif  // SNINJA_PARSER_H_
