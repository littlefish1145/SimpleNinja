// sninja - a simplified, SIMD-accelerated build.ninja implementation.
//
// Milestone 1 (current): manifest parsing, graph construction and the query
// tools that only need the parsed graph.  Tool output is byte-compatible with
// upstream ninja's `targets`, `rules`, `commands` and `inputs` tools.
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include <direct.h>
#include <windows.h>

#include "build.h"
#include "build_log.h"
#include "clean.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "missing_deps.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "status_printer.h"
#include "graph.h"
#include "manifest_parser.h"
#include "scan.h"
#include "state.h"
#include "util.h"
#include "version.h"

using namespace std;

namespace {

/// Answers BuildLogUser's question: "is this output no longer in the graph?".
struct ManifestBuildLogUser : public BuildLogUser {
  explicit ManifestBuildLogUser(const State& state) : state(state) {}
  bool IsPathDead(std::string_view path) const override {
    return state.LookupNode(path) == nullptr;
  }

  const State& state;
};

void Usage(const char* exe) {
  printf(
      "usage: %s [options] [targets]\n"
      "\n"
      "Options:\n"
      "  -f FILE     read FILE as the build manifest\n"
      "  -t TOOL     run a tool: clean, missing-deps, targets, rules,\n"      "                commands, inputs, dump, edges\n"
"  -j N        build with N parallel jobs (default: number of P cores)\n"
"  -k          keep going after a failure\n"
"  -n          dry run\n"
"  -v          show commands\n"
"  --build-logfile=FILE  write the build log to FILE\n"
      "  --version   print the ninja compatibility version\n"
      "  -h          print this message\n",
      exe);
}

/// -t targets [depth N|all]
void ToolTargetsList(const vector<Node*>& nodes, int depth, int indent) {
  for (vector<Node*>::const_iterator n = nodes.begin(); n != nodes.end(); ++n) {
    for (int i = 0; i < indent; ++i)
      printf("  ");
    const char* target = (*n)->path().c_str();
    if ((*n)->in_edge()) {
      printf("%s: %s\n", target, (*n)->in_edge()->rule_->name().c_str());
      if (depth > 1 || depth <= 0)
        ToolTargetsList((*n)->in_edge()->inputs_, depth - 1, indent + 1);
    } else {
      printf("%s\n", target);
    }
  }
}

/// -t targets all -- every output of every edge, in manifest order.
int ToolTargetsListAll(State* state) {
  for (vector<Edge*>::iterator e = state->edges_.begin();
       e != state->edges_.end(); ++e) {
    for (vector<Node*>::iterator out_node = (*e)->outputs_.begin();
         out_node != (*e)->outputs_.end(); ++out_node) {
      printf("%s: %s\n", (*out_node)->path().c_str(),
             (*e)->rule_->name().c_str());
    }
  }
  return 0;
}

/// -t targets rule [NAME] -- the (sorted, deduplicated) outputs of one rule.
int ToolTargetsListByRule(State* state, const string& rule_name) {
  std::set<std::string> rules;
  for (vector<Edge*>::iterator e = state->edges_.begin();
       e != state->edges_.end(); ++e) {
    if ((*e)->rule_->name() == rule_name) {
      for (vector<Node*>::iterator out_node = (*e)->outputs_.begin();
           out_node != (*e)->outputs_.end(); ++out_node)
        rules.insert((*out_node)->path());
    }
  }
  for (std::set<std::string>::const_iterator i = rules.begin();
       i != rules.end(); ++i)
    printf("%s\n", i->c_str());
  return 0;
}

int ToolTargets(State* state, const vector<string>& args, string* err) {
  int depth = 1;
  if (!args.empty()) {
    const string& mode = args[0];
    if (mode == "all") {
      return ToolTargetsListAll(state);
    } else if (mode == "rule") {
      return ToolTargetsListByRule(state, args.size() > 1 ? args[1] : "");
    } else if (mode == "depth") {
      if (args.size() > 1)
        depth = atoi(args[1].c_str());
    } else {
      Error("unknown target tool mode '%s'", mode.c_str());
      return 1;
    }
  }

  vector<Node*> root_nodes = state->RootNodes(err);
  if (!err->empty()) {
    Error("%s", err->c_str());
    return 1;
  }
  ToolTargetsList(root_nodes, depth, 0);
  return 0;
}

int ToolRules(State* state, const vector<string>& args) {
  bool print_description = false;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "-d")
      print_description = true;
  }
  for (const auto& r : state->bindings_.GetRules()) {
    printf("%s", r.first.c_str());
    if (print_description) {
      const EvalString* description = r.second->GetBinding("description");
      if (description != NULL)
        printf(": %s", description->Unparse().c_str());
    }
    printf("\n");
  }
  return 0;
}

void PrintCommands(Edge* edge, EdgeSet* seen, bool single) {
  if (!edge)
    return;
  if (!seen->insert(edge).second)
    return;

  if (!single) {
    for (vector<Node*>::iterator in = edge->inputs_.begin();
         in != edge->inputs_.end(); ++in)
      PrintCommands((*in)->in_edge(), seen, single);
  }

  if (!edge->is_phony())
    puts(edge->EvaluateCommand().c_str());
}

/// The nodes a tool should operate on: the targets named on the command line,
/// or the default targets when none were given.
bool CollectTargetNodes(State* state, const vector<string>& targets,
                        vector<Node*>* nodes, string* err) {
  if (targets.empty()) {
    *nodes = state->DefaultNodes(err);
    return err->empty();
  }
  for (size_t i = 0; i < targets.size(); ++i) {
    string path = targets[i];
    uint64_t slash_bits;
    CanonicalizePath(&path, &slash_bits);
    Node* node = state->LookupNode(path);
    if (!node) {
      *err = "unknown target '" + path + "'";
      return false;
    }
    nodes->push_back(node);
  }
  return true;
}

int ToolCommands(State* state, const vector<string>& targets, bool single) {
  vector<Node*> nodes;
  string err;
  if (!CollectTargetNodes(state, targets, &nodes, &err)) {
    Error("%s", err.c_str());
    return 1;
  }
  EdgeSet seen;
  for (vector<Node*>::iterator in = nodes.begin(); in != nodes.end(); ++in)
    PrintCommands((*in)->in_edge(), &seen, single);
  return 0;
}

int ToolInputs(State* state, const vector<string>& targets) {
  vector<Node*> nodes;
  string err;
  if (!CollectTargetNodes(state, targets, &nodes, &err)) {
    Error("%s", err.c_str());
    return 1;
  }
  InputsCollector collector;
  for (const Node* node : nodes)
    collector.VisitNode(node);

  std::vector<std::string> inputs = collector.GetInputsAsStrings(true);
  std::sort(inputs.begin(), inputs.end());
  for (const std::string& input : inputs)
    puts(input.c_str());
  return 0;
}

int ToolDump(State* state) {
  state->Dump();
  return 0;
}

/// sninja extension: a readable dump of every edge with its input/output kinds.
int ToolEdges(State* state) {
  for (vector<Edge*>::const_iterator e = state->edges_.begin();
       e != state->edges_.end(); ++e) {
    Edge* edge = *e;
    printf("edge #%zu: %s\n", edge->id_, edge->rule().name().c_str());
    for (size_t i = 0; i < edge->outputs_.size(); ++i) {
      printf("  out%s: %s\n", edge->is_implicit_out(i) ? " (implicit)" : "",
             edge->outputs_[i]->path().c_str());
    }
    for (size_t i = 0; i < edge->inputs_.size(); ++i) {
      const char* kind = edge->is_order_only(i)  ? " (order-only)"
                         : edge->is_implicit(i)  ? " (implicit)"
                                                 : "";
      printf("  in%s: %s\n", kind, edge->inputs_[i]->path().c_str());
    }
    for (vector<Node*>::const_iterator v = edge->validations_.begin();
         v != edge->validations_.end(); ++v)
      printf("  validation: %s\n", (*v)->path().c_str());
    printf("  dyndep: %s\n",
           edge->dyndep_ ? edge->dyndep_->path().c_str() : "(none)");
  }
  return 0;
}

int RunTool(const std::string& tool, State* state,
            const std::vector<std::string>& tool_args,
            const std::vector<std::string>& targets, std::string* err,
            const BuildConfig& config, DepsLog* deps_log,
            BuildLog* build_log, const std::string& build_log_path,
  DiskInterface* disk_interface) {
  if (tool == "targets")
    return ToolTargets(state, tool_args, err);
  if (tool == "rules")
    return ToolRules(state, tool_args);
  if (tool == "commands") {
    bool single = false;
    for (const std::string& a : tool_args) {
      if (a == "-s")
        single = true;
    }
    return ToolCommands(state, targets, single);
  }
  if (tool == "inputs")
    return ToolInputs(state, targets);
  if (tool == "dump")
    return ToolDump(state);
  if (tool == "edges")
    return ToolEdges(state);
  if (tool == "restat") {
    // Re-stat the outputs recorded in the build log and update their recorded
    // mtimes.  This is how a build recovers when a command wrote its outputs and
    // then failed or was interrupted: the log then claims an older mtime than
    // the file really has, and every later run would rebuild that edge forever.
    //
    // With no arguments every output is restatted; otherwise only the named ones.
    std::vector<char*> outputs;
    for (const std::string& o : tool_args)
      outputs.push_back(const_cast<char*>(o.c_str()));
    if (!build_log->Restat(build_log_path, *disk_interface,
                           static_cast<int>(outputs.size()),
                           outputs.empty() ? nullptr : outputs.data(), err)) {
      Error("restat: %s", err->c_str());
      return 1;
    }
    return 0;
  }
  if (tool == "clean") {
    Cleaner cleaner(state, config, disk_interface);
    // Everything after the tool name lands in |tool_args|, not |targets|.
    bool rules_mode = false;
    bool clean_generator = false;
    std::vector<std::string> clean_args;
    for (const std::string& a : tool_args) {
      if (a == "-r" || a == "rules") {
        rules_mode = true;
        continue;
      }
      if (a == "-g") {
        clean_generator = true;
        continue;
      }
      if (a == "-v") {
        continue;
      }
      clean_args.push_back(a);
    }
    if (!clean_args.empty() && clean_args[0] == "dead")
      return cleaner.CleanDead(&build_log->entries());
    if (rules_mode) {
      std::vector<char*> rules;
      for (const std::string& r : clean_args)
        rules.push_back(const_cast<char*>(r.c_str()));
      return cleaner.CleanRules(static_cast<int>(rules.size()), rules.data());
    }
    if (!clean_args.empty()) {
      std::vector<char*> args;
      for (const std::string& t : clean_args)
        args.push_back(const_cast<char*>(t.c_str()));
      return cleaner.CleanTargets(static_cast<int>(args.size()), args.data());
    }
    return cleaner.CleanAll(clean_generator);
  }
  if (tool == "missing-deps" || tool == "missingdeps") {
    MissingDependencyPrinter printer;
    MissingDependencyScanner scanner(&printer, deps_log, state, disk_interface);
    for (Node* node : state->DefaultNodes(err))
      scanner.ProcessNode(node);
    scanner.PrintStats();
    return scanner.HadMissingDeps() ? 1 : 0;
  }

  Error("unknown tool '%s'", tool.c_str());
  return 1;
}


}  // namespace

int main(int argc, char** argv) {
  sninja::scan::Init();

  std::string manifest_path = "build.ninja";
  std::string chdir;
  std::string build_log_path;
  std::string tool;
  std::vector<std::string> tool_args;
  std::vector<std::string> targets;
  BuildConfig build_config;
  build_config.verbosity = BuildConfig::STATUS;
  build_config.parallelism = 1;
  bool have_parallelism = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      Usage(argv[0]);
      return 0;
    }
    if (arg == "--version") {
      printf("%s\n", kNinjaVersion);
      return 0;
    }
    if (arg == "-C" && i + 1 < argc) {
      chdir = argv[++i];
      continue;
    }
    if (arg == "-f" && i + 1 < argc) {
      manifest_path = argv[++i];
      continue;
    }
    if (arg == "-j" && i + 1 < argc) {
      build_config.parallelism = atoi(argv[++i]);
      if (build_config.parallelism < 0)
        build_config.parallelism = GetDefaultParallelism();
      have_parallelism = true;
      continue;
    }
    if (arg.compare(0, 2, "-j") == 0 && arg.size() > 2) {
      build_config.parallelism = atoi(arg.c_str() + 2);
      have_parallelism = true;
      continue;
    }
    if (arg == "-k" || arg == "--keep-going") {
      build_config.keep_going = true;
      continue;
    }
    if (arg == "-n" || arg == "--dry-run") {
      build_config.dry_run = true;
      continue;
    }
    if (arg == "-v" || arg == "--verbose") {
      build_config.verbosity = BuildConfig::COMMAND;
      continue;
    }
    if (arg == "-d" || arg == "--debug") {
      build_config.verbosity = BuildConfig::COMMAND;
      continue;
    }
    if (arg.rfind("--build-logfile=", 0) == 0) {
      build_log_path = arg.substr(strlen("--build-logfile="));
      continue;
    }
    if (arg == "-t" && i + 1 < argc) {
      tool = argv[++i];
      // Everything up to the next option belongs to the tool.  Some tools take
      // their own single-letter options, so those are collected too and the
      // tool filters them.
      while (i + 1 < argc && argv[i + 1][0] != '-') {
        tool_args.push_back(argv[++i]);
      }
  if (tool == "clean") {
        while (i + 1 < argc &&
               (strcmp(argv[i + 1], "-r") == 0 || strcmp(argv[i + 1], "-g") == 0 ||
                strcmp(argv[i + 1], "-v") == 0)) {
          tool_args.push_back(argv[++i]);
          while (i + 1 < argc && argv[i + 1][0] != '-')
            tool_args.push_back(argv[++i]);
        }
      }
      continue;
    }


    if (!arg.empty() && arg[0] == '-') {
      Usage(argv[0]);
      return 1;
    }
    targets.push_back(arg);
  }
  if (!have_parallelism)
    build_config.parallelism = GetDefaultParallelism();

  if (!chdir.empty()) {
    if (_chdir(chdir.c_str()) != 0) {
      Error("chdir to '%s' failed", chdir.c_str());
      return 1;
    }
  }

  RealDiskInterface disk;
  State state;
  ManifestParser parser(&state, &disk);

  std::string err;
  if (!parser.Load(manifest_path, &err)) {
    Error("%s", err.c_str());
    return 1;
  }

  if (!targets.empty()) {
    for (size_t i = 0; i < targets.size(); ++i) {
      std::string path = targets[i];
      uint64_t slash_bits;
      CanonicalizePath(&path, &slash_bits);
      if (!state.LookupNode(path)) {
        Error("unknown target '%s'", path.c_str());
        return 1;
      }
      if (!state.AddDefault(path, &err)) {
        Error("%s", err.c_str());
        return 1;
      }
    }
  }

  if (!tool.empty()) {
    // The tools that inspect build state need the logs loaded first, so that
    // `clean dead` and `missing-deps` see what the previous build recorded.
    RealDiskInterface disk;
    string tool_deps_log_path = ".ninja_deps";
    string tool_build_log_path =
        build_log_path.empty() ? ".ninja_log" : build_log_path;
    DepsLog tool_deps_log;
    BuildLog tool_build_log;
    string load_err;
    tool_deps_log.Load(tool_deps_log_path, &state, &load_err);
    tool_build_log.Load(tool_build_log_path, &load_err);
    return RunTool(tool, &state, tool_args, targets, &err, build_config,
                   &tool_deps_log, &tool_build_log, tool_build_log_path,
                   &disk);
  }


  // No tool: run a build.
  if (build_log_path.empty())
    build_log_path = ".ninja_log";
  string deps_log_path = ".ninja_deps";

  ManifestBuildLogUser log_user{state};
  BuildLog build_log;
  DepsLog deps_log;
  StatusPrinterImpl status(build_config);
  status.SetBuildLog(build_log_path);

  Builder builder(&state, build_config, &build_log, &deps_log, &disk, &status,
                  static_cast<int64_t>(GetTickCount64()));
  builder.SetBuildLog(build_log_path);
  builder.SetDepsLogPath(deps_log_path);
  if (!build_log.OpenForWrite(build_log_path, log_user, &err)) {
    Error("opening build log: %s", err.c_str());
    return 1;
  }
  if (!deps_log.OpenForWrite(deps_log_path, &err)) {
    Error("opening deps log: %s", err.c_str());
    return 1;
  }
  {
    string deps_err;
    deps_log.Load(deps_log_path, &state, &deps_err);
  }

  vector<Node*> default_nodes = state.DefaultNodes(&err);
  if (!err.empty()) {
    Error("%s", err.c_str());
    return 1;
  }
  if (default_nodes.empty()) {
    Error("no targets to build");
    return 1;
  }
  for (Node* node : default_nodes) {
    if (!builder.AddTarget(node->path(), &err)) {
      Error("%s", err.c_str());
      return 1;
    }
  }

  bool success = builder.Build(&err);
  build_log.Close();
  deps_log.Close();
  if (!success) {
    Error("%s", err.c_str());
    return 1;
  }
  return 0;
}
