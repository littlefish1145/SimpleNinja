#include "parser.h"

#include "disk_interface.h"
#include "util.h"

using namespace std;

bool Parser::Load(const string& filename, string* err, Lexer* parent) {
  string contents;
  string read_err;
  if (file_reader_->ReadFile(filename, &contents, &read_err) !=
      FileReader::Okay) {
    *err = "loading '" + filename + "': " + read_err;
    if (parent)
      parent->Error(string(*err), err);
    return false;
  }

  return Parse(filename, contents, err);
}

bool Parser::ExpectToken(Lexer::Token expected, string* err) {
  Lexer::Token token = lexer_.ReadToken();
  if (token != expected) {
    string message = string("expected ") + Lexer::TokenName(expected);
    message += string(", got ") + Lexer::TokenName(token);
    message += Lexer::TokenErrorHint(expected);
    return lexer_.Error(message, err);
  }
  return true;
}
