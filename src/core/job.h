#ifndef SNINJA_JOB_H_
#define SNINJA_JOB_H_

#include <string>

// A Windows job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.
//
// Every child sninja starts is assigned to this job, so that when sninja exits
// -- normally, on Ctrl-C, or by being killed -- the operating system kills
// whatever is left in the job.  This matters because children are created with
// CREATE_NEW_PROCESS_GROUP: a console Ctrl-C no longer reaches them, so without
// this they would be orphaned and keep running after sninja is gone.  It also
// covers grandchildren (cl.exe spawns its own helpers), which enumerating and
// terminating our direct children would miss.
//
// A process can only be assigned to one job per nesting level, and assigning can
// fail if sninja itself is already inside a job that forbids nesting.  When that
// happens Assign() reports it and callers fall back to terminating the direct
// children themselves; correctness does not depend on the job existing.
class KillOnCloseJob {
 public:
  ~KillOnCloseJob();

  /// Create the job and arm the kill-on-close limit.
  bool Create(std::string* err);

  /// Put a freshly created process into the job.  Returns false if this build
  /// cannot use a job (see AssignToJobObject returning access-denied), which is
  /// not an error: the caller just has to do its own cleanup.
  bool Assign(void* process);

  /// Kill everything currently in the job, without closing the job.
  void KillAll();

  void* handle() const { return job_; }

  /// The one job shared by every child of this process.
  static KillOnCloseJob* Global();

 private:
  void* job_ = nullptr;
  bool usable_ = false;
};

#endif  // SNINJA_JOB_H_
