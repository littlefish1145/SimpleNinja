#ifndef SNINJA_DEPS_LOADER_H_
#define SNINJA_DEPS_LOADER_H_

#include <optional>
#include <string>
#include <vector>

#include "graph.h"

struct DepsLog;
struct State;

/// Implements loading dependency info recorded in the deps log / depfiles.
/// Mirrors `ImplicitDepLoader` from the upstream `graph.cc` but returns the
/// trimmed `graph.h` types we use.
struct ImplicitDepLoader : DepsLoader {
  ImplicitDepLoader(State* state, DepsLog* deps_log,
                    DiskInterface* disk_interface)
      : state_(state), deps_log_(deps_log), disk_interface_(disk_interface) {}

  bool LoadDeps(Edge* edge, EdgeInputsRange* added,
                std::string* err) override;
  bool LoadDepsTry(const Edge* edge, std::string* err) const override;

 private:
  bool LoadDepsFromLog(Edge* edge, EdgeInputsRange* added, std::string* err);
  bool LoadDepsFromLogTry(const Edge* edge, std::string* err) const;
  bool LoadDepFile(Edge* edge, const std::string& path, EdgeInputsRange* added,
                   std::string* err);
  bool LoadDepFileTry(const Edge* edge, const std::string& path,
                      std::string* err) const;
  bool ProcessDepfileDeps(Edge* edge, EdgeInputsRange* added,
                          std::vector<std::string_view>* depfile_ins,
                          std::string* err);
  std::vector<Node*>::iterator PreallocateSpace(Edge* edge, int count);

  State* state_;
  DepsLog* deps_log_;
  DiskInterface* disk_interface_;
};

#endif  // SNINJA_DEPS_LOADER_H_
