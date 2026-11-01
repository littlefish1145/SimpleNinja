#include "lexer.h"

#include <stdio.h>
#include <string.h>

#include "eval_env.h"
#include "scan.h"
#include "util.h"

using namespace std;

// $^ supported starting this version
const int kMinNewlineEscapeVersionMajor = 1;
const int kMinNewlineEscapeVersionMinor = 14;

namespace {

// Byte at |p|, treating anything past the end of the input as NUL.  The
// reference lexer relies on the input being NUL terminated; doing it
// explicitly is equivalent and also safe for non-terminated buffers.
inline unsigned char At(const char* p, const char* end) {
  return p < end ? static_cast<unsigned char>(*p) : 0;
}

// simple_varname = [a-zA-Z0-9_-]+
// (varname additionally allows '.', which is why this is separate.)
const char* FindSimpleVarNameEnd(const char* p, const char* end) {
  while (p < end) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok)
      break;
    ++p;
  }
  return p;
}

const char kBadEscape[] =
    "bad $-escape (literal $ must be written as $$)";

}  // namespace

bool Lexer::Error(const string& message, string* err) {
  // Compute line/column.
  int line = 1;
  const char* line_start = input_.data();
  for (const char* p = input_.data(); p < last_token_; ++p) {
    if (*p == '\n') {
      ++line;
      line_start = p + 1;
    }
  }
  int col = last_token_ ? (int)(last_token_ - line_start) : 0;

  char buf[1024];
  snprintf(buf, sizeof(buf), "%s:%d: ", std::string(filename_).c_str(), line);
  *err = buf;
  *err += message + "\n";

  // Add some context to the message.
  const int kTruncateColumn = 72;
  if (col > 0 && col < kTruncateColumn) {
    int len;
    bool truncated = true;
    for (len = 0; len < kTruncateColumn; ++len) {
      if (line_start[len] == 0 || line_start[len] == '\n') {
        truncated = false;
        break;
      }
    }
    *err += string(line_start, len);
    if (truncated)
      *err += "...";
    *err += "\n";
    *err += string(col, ' ');
    *err += "^ near here";
  }

  return false;
}

Lexer::Lexer(const char* input) {
  Start("input", input);
}

void Lexer::Start(string_view filename, string_view input) {
  filename_ = filename;
  input_ = input;
  end_ = input_.data() + input_.size();
  ofs_ = input_.data();
  last_token_ = NULL;
}

const char* Lexer::TokenName(Token t) {
  switch (t) {
    case ERROR:    return "lexing error";
    case BUILD:    return "'build'";
    case COLON:    return "':'";
    case DEFAULT:  return "'default'";
    case EQUALS:   return "'='";
    case IDENT:    return "identifier";
    case INCLUDE:  return "'include'";
    case INDENT:   return "indent";
    case NEWLINE:  return "newline";
    case PIPE2:    return "'||'";
    case PIPE:     return "'|'";
    case PIPEAT:   return "'|@'";
    case POOL:     return "'pool'";
    case RULE:     return "'rule'";
    case SUBNINJA: return "'subninja'";
    case TEOF:     return "eof";
  }
  return NULL;  // not reached
}

const char* Lexer::TokenErrorHint(Token expected) {
  switch (expected) {
    case COLON:
      return " ($ also escapes ':')";
    default:
      return "";
  }
}

string Lexer::DescribeLastError() {
  if (last_token_) {
    switch (last_token_[0]) {
      case '\t':
        return "tabs are not allowed, use spaces";
    }
  }
  return "lexing error";
}

void Lexer::UnreadToken() {
  ofs_ = last_token_;
}

Lexer::Token Lexer::ReadToken() {
  const char* p = ofs_;
  const char* start;
  Lexer::Token token;
  for (;;) {
    start = p;

    // Every rule below may be prefixed by a run of spaces; look past them
    // first, but keep |p| untouched so that shorter rules still see them.
    const char* q = p;
    while (At(q, end_) == ' ')
      ++q;

    // [ ]*"#"[^\000\n]*"\n" -> a comment line: the trailing newline is
    // swallowed and no NEWLINE token is emitted.
    if (At(q, end_) == '#') {
      const char* end = q + 1;
      while (At(end, end_) != 0 && At(end, end_) != '\n')
        ++end;
      if (At(end, end_) == '\n') {
        p = end + 1;
        continue;
      }
      // Unterminated (EOF): the shorter rules apply instead, i.e. the
      // leading spaces become INDENT, or the '#' itself becomes ERROR.
    }

    if (At(q, end_) == '\r' && At(q + 1, end_) == '\n') {
      p = q + 2;
      token = NEWLINE;
      break;
    }
    if (At(q, end_) == '\n') {
      p = q + 1;
      token = NEWLINE;
      break;
    }
    if (q != p) {
      p = q;
      token = INDENT;
      break;
    }

    const unsigned char c = At(p, end_);
    if (c == 0) {
      ++p;
      token = TEOF;
      break;
    }

    // varname = [a-zA-Z0-9_.-]+, with keywords winning on an exact match.
    const char* id_end = sninja::scan::find_ident_end(p, end_);
    if (id_end != p) {
      const size_t len = (size_t)(id_end - p);
      p = id_end;
      token = IDENT;
      if (len == 5 && memcmp(start, "build", 5) == 0)
        token = BUILD;
      else if (len == 4 && memcmp(start, "pool", 4) == 0)
        token = POOL;
      else if (len == 4 && memcmp(start, "rule", 4) == 0)
        token = RULE;
      else if (len == 7 && memcmp(start, "default", 7) == 0)
        token = DEFAULT;
      else if (len == 7 && memcmp(start, "include", 7) == 0)
        token = INCLUDE;
      else if (len == 8 && memcmp(start, "subninja", 8) == 0)
        token = SUBNINJA;
      break;
    }

    ++p;
    if (c == '=')
      token = EQUALS;
    else if (c == ':')
      token = COLON;
    else if (c == '|') {
      if (At(p, end_) == '@') {
        ++p;
        token = PIPEAT;
      } else if (At(p, end_) == '|') {
        ++p;
        token = PIPE2;
      } else {
        token = PIPE;
      }
    } else
      token = ERROR;
    break;
  }

  last_token_ = start;
  ofs_ = p;
  if (token != NEWLINE && token != TEOF)
    EatWhitespace();
  return token;
}

bool Lexer::PeekToken(Token token) {
  Token t = ReadToken();
  if (t == token)
    return true;
  UnreadToken();
  return false;
}

void Lexer::EatWhitespace() {
  const char* p = ofs_;
  for (;;) {
    // [ ]+ | "$\r\n" | "$\n" -> continue; nul | [^] -> break.
    if (At(p, end_) == ' ') {
      p = sninja::scan::find_space_end(p, end_);
      continue;
    }
    if (At(p, end_) == '$' && At(p + 1, end_) == '\r' &&
        At(p + 2, end_) == '\n') {
      p += 3;
      continue;
    }
    if (At(p, end_) == '$' && At(p + 1, end_) == '\n') {
      p += 2;
      continue;
    }
    break;
  }
  ofs_ = p;
}

bool Lexer::ReadIdent(string* out) {
  const char* p = ofs_;
  const char* start = p;
  p = sninja::scan::find_ident_end(p, end_);
  if (p == start) {
    last_token_ = start;
    return false;
  }
  out->assign(start, p - start);
  last_token_ = start;
  ofs_ = p;
  EatWhitespace();
  return true;
}

bool Lexer::ReadEvalString(EvalString* eval, bool path, string* err) {
  const char* p = ofs_;
  const char* start;
  for (;;) {
    start = p;

    // [^$ :\r\n|\000]+
    const char* text_end = sninja::scan::find_eval_stop(p, end_);
    if (text_end != p) {
      eval->AddText(string_view(p, text_end - p));
      p = text_end;
      continue;
    }

    const unsigned char c = At(p, end_);

    if (c == 0) {
      last_token_ = start;
      return Error("unexpected EOF", err);
    }

    if (c == '\r') {
      if (At(p + 1, end_) == '\n') {
        p += 2;
        if (path)
          p = start;
        break;
      }
      last_token_ = start;
      return Error(DescribeLastError(), err);
    }

    if (c == '\n' || c == ' ' || c == ':' || c == '|') {
      ++p;
      if (path) {
        p = start;
        break;
      }
      if (c == '\n')
        break;
      eval->AddText(string_view(p - 1, 1));
      continue;
    }

    if (c == '$') {
      ++p;
      const unsigned char e = At(p, end_);

      if (e == '$') {
        ++p;
        eval->AddText("$");
        continue;
      }
      if (e == ' ') {
        ++p;
        eval->AddText(" ");
        continue;
      }
      if (e == ':') {
        ++p;
        eval->AddText(":");
        continue;
      }
      if (e == '^') {
        if (!newline_version_checked_) {
          if ((manifest_version_major < kMinNewlineEscapeVersionMajor) ||
              (manifest_version_major == kMinNewlineEscapeVersionMajor &&
               manifest_version_minor < kMinNewlineEscapeVersionMinor)) {
            return Error("using $^ escape requires specifying "
                         "'ninja_required_version' with version greater or "
                         "equal 1.14",
                         err);
          }
          newline_version_checked_ = true;
        }
        ++p;
        eval->AddText("\n");
        continue;
      }
      if (e == '\r' && At(p + 1, end_) == '\n') {
        p += 2;
        p = sninja::scan::find_space_end(p, end_);
        continue;
      }
      if (e == '\n') {
        ++p;
        p = sninja::scan::find_space_end(p, end_);
        continue;
      }
      if (e == '{') {
        ++p;
        const char* name = p;
        const char* name_end = sninja::scan::find_ident_end(p, end_);
        if (name_end != name && At(name_end, end_) == '}') {
          eval->AddSpecial(string_view(name, name_end - name));
          p = name_end + 1;
          continue;
        }
        last_token_ = start;
        return Error(kBadEscape, err);
      }
      const char* name_end = FindSimpleVarNameEnd(p, end_);
      if (name_end != p) {
        eval->AddSpecial(string_view(p, name_end - p));
        p = name_end;
        continue;
      }
      last_token_ = start;
      return Error(kBadEscape, err);
    }

    // [^]
    last_token_ = start;
    return Error(DescribeLastError(), err);
  }

  last_token_ = start;
  ofs_ = p;
  if (path)
    EatWhitespace();
  // Non-path strings end in newlines, so there's no whitespace to eat.
  return true;
}
