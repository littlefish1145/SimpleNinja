// A path borrows plain text only until its build statement has been parsed.
// Variable paths retain Ninja's deferred evaluation, including edge bindings.
#ifndef SNINJA_MANIFEST_PATH_H_
#define SNINJA_MANIFEST_PATH_H_
#include <string_view>
#include <variant>
#include "eval_env.h"

struct ManifestPath {
  static bool IsCanonical(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/')
      return false;
#ifdef _WIN32
    if (path.find('\\') != std::string_view::npos)
      return false;
#endif
    for (size_t begin = 0;;) {
      size_t end = path.find('/', begin);
      const auto component = path.substr(begin, end - begin);
      if (component.empty() || component == "." || component == "..")
        return false;
      if (end == std::string_view::npos)
        return true;
      begin = end + 1;
    }
  }
  void Clear() { value.emplace<std::string_view>(); }
  bool empty() const {
    if (auto literal = std::get_if<std::string_view>(&value))
      return literal->empty();
    return std::get<EvalString>(value).empty();
  }
  std::string Evaluate(Env* env) const {
    if (auto literal = std::get_if<std::string_view>(&value))
      return std::string(*literal);
    return std::get<EvalString>(value).Evaluate(env);
  }
  StringPiece EvaluateCanonical(Env* env, std::string* storage, uint64_t* slash_bits) const {
    *slash_bits = 0;
    if (auto literal = std::get_if<std::string_view>(&value); literal && IsCanonical(*literal))
      return StringPiece(literal->data(), literal->size());
    *storage = Evaluate(env);
    if (!storage->empty())
      CanonicalizePath(storage, slash_bits);
    return *storage;
  }
  std::variant<std::string_view, EvalString> value;
};
#endif
