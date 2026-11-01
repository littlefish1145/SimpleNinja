#include "status_printer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <mutex>

#include "build.h"
#include "graph.h"
#include "util.h"

using namespace std;

StatusPrinterImpl::StatusPrinterImpl(const BuildConfig& config)
    : config_(config), started_edges_(0), finished_edges_(0), total_edges_(0),
      running_edges_(0), time_millis_(0), out_(stdout),
      finish_command_length_(0) {}

StatusPrinterImpl::~StatusPrinterImpl() = default;

void StatusPrinterImpl::SetBuildLog(const string& log) {
  lock_guard<mutex> lock(mutex_);
  build_log_ = log;
  // The build log path is echoed at the end of the build, like ninja does.
  status_prefix_ = "";
}

int64_t StatusPrinterImpl::GetElapsedMillis() const {
  lock_guard<mutex> lock(mutex_);
  return time_millis_;
}

void StatusPrinterImpl::SetTotalEdges(int total) {
  lock_guard<mutex> lock(mutex_);
  total_edges_ = total;
}

void StatusPrinterImpl::BuildEdgeStarted(const Edge* /*edge*/,
                                         int64_t /*start_time_millis*/) {
  // Edges run on several threads, so only the counters are touched here; the
  // per-edge line is printed once it finishes.
  lock_guard<mutex> lock(mutex_);
  ++started_edges_;
  ++running_edges_;
}

void StatusPrinterImpl::BuildEdgeFinished(Edge* edge, int64_t start_time_millis,
                                          int64_t end_time_millis, bool success,
                                          const string& output) {
  lock_guard<mutex> lock(mutex_);
  ++finished_edges_;
  if (running_edges_ > 0)
    --running_edges_;

  if (config_.verbosity == BuildConfig::NORMAL_ERRORS ||
      config_.verbosity == BuildConfig::QUIET) {
    if (!success)
      Error("%s", output.c_str());
    return;
  }

  if (config_.verbosity == BuildConfig::STATUS && !edge->use_console()) {
    string description = edge->GetBinding("description");
    if (description.empty())
      description = edge->rule().name();
    const int64_t time_millis = end_time_millis - start_time_millis;
    fprintf(out_, "[%d/%d] %-24s %6lldms\n", finished_edges_,
            total_edges_ ? total_edges_ : finished_edges_,
            description.c_str(), static_cast<long long>(time_millis));
  } else if (edge->use_console() && success) {
    // Console-pool edges own the terminal while they run, so give them a line
    // of their own back.
    fprintf(out_, "\n");
  }

  if (!success)
    Error("%s", output.c_str());

  if (end_time_millis > time_millis_)
    time_millis_ = end_time_millis;
  fflush(out_);
}

void StatusPrinterImpl::BuildLoadDyndeps(const string& /*dyndep*/,
                                         int64_t /*start_time_millis*/) {}

void StatusPrinterImpl::BuildLoadDyndepsFinished(const string& /*dyndep*/,
                                                 int64_t /*start_time_millis*/,
                                                 int64_t /*end_time_millis*/,
                                                 bool /*success*/,
                                                 const string& /*output*/) {}

void StatusPrinterImpl::BuildStarted() {
  lock_guard<mutex> lock(mutex_);
  started_edges_ = 0;
  finished_edges_ = 0;
  running_edges_ = 0;
  start_millis_ = NowMillis();
  time_millis_ = 0;
}

void StatusPrinterImpl::BuildFinished() {
  lock_guard<mutex> lock(mutex_);
  if (!build_log_.empty())
    fprintf(out_, "ninja: build log: %s\n", build_log_.c_str());
  PrintBuildStat();
  fflush(out_);
}

void StatusPrinterImpl::PrintBuildStat() {
  if (config_.verbosity == BuildConfig::QUIET)
    return;
  time_millis_ = NowMillis() - start_millis_;
  fprintf(out_, "ninja: %.3fs elapsed, %d edge(s) run\n",
          time_millis_ / 1000.0, finished_edges_);
}

void StatusPrinterImpl::Info(const char* msg, ...) {
  lock_guard<mutex> lock(mutex_);
  va_list ap;
  va_start(ap, msg);
  fprintf(out_, "ninja: ");
  vfprintf(out_, msg, ap);
  fprintf(out_, "\n");
  va_end(ap);
  fflush(out_);
}

void StatusPrinterImpl::Warning(const char* msg, ...) {
  lock_guard<mutex> lock(mutex_);
  va_list ap;
  va_start(ap, msg);
  fprintf(out_, "ninja: warning: ");
  vfprintf(out_, msg, ap);
  fprintf(out_, "\n");
  va_end(ap);
  fflush(out_);
}

void StatusPrinterImpl::Error(const char* msg, ...) {
  lock_guard<mutex> lock(mutex_);
  va_list ap;
  va_start(ap, msg);
  fprintf(out_, "ninja: error: ");
  vfprintf(out_, msg, ap);
  fprintf(out_, "\n");
  va_end(ap);
  fflush(out_);
}
