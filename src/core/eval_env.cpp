#include "eval_env.h"

#include <assert.h>

std::string BindingEnv::LookupVariable(std::string_view var) {
  const auto i = bindings_.find(var);
  if (i != bindings_.end())
    return i->second;
  if (parent_)
    return parent_->LookupVariable(var);
  return "";
}

void BindingEnv::AddBinding(const std::string& key, std::string_view val) {
  bindings_[key].assign(val);
}

void BindingEnv::AddRule(std::unique_ptr<const Rule> rule) {
  assert(LookupRuleCurrentScope(rule->name()) == nullptr);
  rules_[rule->name()] = std::move(rule);
}

const Rule* BindingEnv::LookupRuleCurrentScope(std::string_view rule_name) {
  auto i = rules_.find(rule_name);
  if (i == rules_.end())
    return nullptr;
  return i->second.get();
}

const Rule* BindingEnv::LookupRule(std::string_view rule_name) {
  auto i = rules_.find(rule_name);
  if (i != rules_.end())
    return i->second.get();
  if (parent_)
    return parent_->LookupRule(rule_name);
  return nullptr;
}

void Rule::AddBinding(const std::string& key, const EvalString& val) {
  bindings_[key] = val;
}

const EvalString* Rule::GetBinding(std::string_view key) const {
  Bindings::const_iterator i = bindings_.find(key);
  if (i == bindings_.end())
    return nullptr;
  return &i->second;
}

std::unique_ptr<Rule> Rule::Phony() {
  auto rule = std::unique_ptr<Rule>(new Rule("phony"));
  rule->phony_ = true;
  return rule;
}

bool Rule::IsPhony() const {
  return phony_;
}

// static
bool Rule::IsReservedBinding(std::string_view var) {
  return var == "command" ||
      var == "depfile" ||
      var == "dyndep" ||
      var == "description" ||
      var == "deps" ||
      var == "generator" ||
      var == "pool" ||
      var == "restat" ||
      var == "rspfile" ||
      var == "rspfile_content" ||
      var == "msvc_deps_prefix";
}

const std::map<std::string, std::unique_ptr<const Rule>, std::less<>>&
BindingEnv::GetRules() const {
  return rules_;
}

std::string BindingEnv::LookupWithFallback(std::string_view var,
                                           const EvalString* eval,
                                           Env* env) {
  const auto i = bindings_.find(var);
  if (i != bindings_.end())
    return i->second;

  if (eval)
    return eval->Evaluate(env);

  if (parent_)
    return parent_->LookupVariable(var);

  return "";
}

std::string EvalString::Evaluate(Env* env) const {
  if (parsed_.empty()) {
    return single_token_;
  }

  std::string result;
  for (TokenList::const_iterator i = parsed_.begin(); i != parsed_.end(); ++i) {
    if (i->second == RAW)
      result.append(i->first);
    else
      result.append(env->LookupVariable(i->first));
  }
  return result;
}

void EvalString::AddText(std::string_view text) {
  if (parsed_.empty()) {
    single_token_.append(text);
  } else if (!parsed_.empty() && parsed_.back().second == RAW) {
    parsed_.back().first.append(text);
  } else {
    parsed_.push_back(std::make_pair(std::string(text), RAW));
  }
}

void EvalString::AddSpecial(std::string_view text) {
  if (parsed_.empty() && !single_token_.empty()) {
    // Going from one to two tokens, so we can no longer apply
    // our single_token_ optimization and need to push everything
    // onto the vector.
    parsed_.push_back(std::make_pair(std::move(single_token_), RAW));
  }
  parsed_.push_back(std::make_pair(std::string(text), SPECIAL));
}

std::string EvalString::Serialize() const {
  std::string result;
  if (parsed_.empty() && !single_token_.empty()) {
    result.append("[");
    result.append(single_token_);
    result.append("]");
  } else {
    for (const auto& pair : parsed_) {
      result.append("[");
      if (pair.second == SPECIAL)
        result.append("$");
      result.append(pair.first);
      result.append("]");
    }
  }
  return result;
}

std::string EvalString::Unparse() const {
  std::string result;
  if (parsed_.empty() && !single_token_.empty()) {
    result.append(single_token_);
  } else {
    for (TokenList::const_iterator i = parsed_.begin();
         i != parsed_.end(); ++i) {
      bool special = (i->second == SPECIAL);
      if (special)
        result.append("${");
      result.append(i->first);
      if (special)
        result.append("}");
    }
  }
  return result;
}

