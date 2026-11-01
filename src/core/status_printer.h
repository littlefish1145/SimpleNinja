#ifndef SNINJA_STATUS_PRINTER_H_
#define SNINJA_STATUS_PRINTER_H_

#include <stdio.h>

#include <mutex>
#include <string>

#include "timestamp.h"

struct BuildConfig;
struct Edge;
struct Status;

/// Interface for printing the status of a build: which edges are running,
/// how many are left, and (with -v) the command lines.
///
/// Edges run on several threads, so implementations must be thread safe.
struct StatusPrinter {
  virtual ~StatusPrinter() {}

  virtual void BuildEdgeStarted(const Edge* edge, int64_t start_time_millis) = 0;
  virtual void BuildEdgeFinished(Edge* edge, int64_t start_time_millis,
                                 int64_t end_time_millis, bool success,
                                 const std::string& output) = 0;
  virtual void BuildLoadDyndeps(const std::string& dyndep,
                                int64_t start_time_millis) = 0;
  virtual void BuildLoadDyndepsFinished(const std::string& dyndep,
                                        int64_t start_time_millis,
                                        int64_t end_time_millis, bool success,
                                        const std::string& output) = 0;
  virtual void BuildStarted() = 0;
  virtual void BuildFinished() = 0;

  virtual void Info(const char* msg, ...) = 0;
  virtual void Warning(const char* msg, ...) = 0;
  virtual void Error(const char* msg, ...) = 0;

  virtual void SetBuildLog(const std::string& /*log*/) {}
  /// The number of edges the build intends to run, used for "[n/total]".
  virtual void SetTotalEdges(int /*total*/) {}
};

/// Prints one "[n/total] description time" line per finished edge, plus the
/// final summary.
struct StatusPrinterImpl : StatusPrinter {
  explicit StatusPrinterImpl(const BuildConfig& config);
  ~StatusPrinterImpl() override;

  void BuildEdgeStarted(const Edge* edge, int64_t start_time_millis) override;
  void BuildEdgeFinished(Edge* edge, int64_t start_time_millis,
                         int64_t end_time_millis, bool success,
                         const std::string& output) override;
  void BuildLoadDyndeps(const std::string& dyndep,
                        int64_t start_time_millis) override;
  void BuildLoadDyndepsFinished(const std::string& dyndep,
                                int64_t start_time_millis,
                                int64_t end_time_millis, bool success,
                                const std::string& output) override;
  void BuildStarted() override;
  void BuildFinished() override;

  void Info(const char* msg, ...) override;
  void Warning(const char* msg, ...) override;
  void Error(const char* msg, ...) override;

  void SetBuildLog(const std::string& log) override;
  void SetTotalEdges(int total) override;

  /// Print the elapsed time and CPU usage at the end of the build.
  void PrintBuildStat();

  int64_t GetElapsedMillis() const;

 private:
  const BuildConfig& config_;
  int started_edges_, finished_edges_, total_edges_, running_edges_;
  int64_t time_millis_;
  int64_t start_millis_;
  FILE* out_;
  std::string status_prefix_;
  std::string build_log_;
  int finish_command_length_;

  /// Serialises output; edges finish on several threads at once.
  mutable std::mutex mutex_;
};

#endif  // SNINJA_STATUS_PRINTER_H_

