#include "job.h"

#include <windows.h>

namespace {

std::string LastErrorString() {
  char buf[64];
  snprintf(buf, sizeof(buf), "%lu", GetLastError());
  return std::string(buf);
}

}  // namespace

KillOnCloseJob::~KillOnCloseJob() {
  // Closing the last handle to a job with KILL_ON_JOB_CLOSE terminates
  // everything still in it, so this is what stops a `-t` tool that was
  // interrupted from leaving children behind.
  if (job_)
    CloseHandle(static_cast<HANDLE>(job_));
}

bool KillOnCloseJob::Create(std::string* err) {
  job_ = CreateJobObjectA(nullptr, nullptr);
  if (!job_) {
    *err = "CreateJobObject failed: " + LastErrorString();
    return false;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
  memset(&limits, 0, sizeof(limits));
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(static_cast<HANDLE>(job_),
                               JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits))) {
    *err = "SetInformationJobObject failed: " + LastErrorString();
    CloseHandle(static_cast<HANDLE>(job_));
    job_ = nullptr;
    return false;
  }
  usable_ = true;
  return true;
}

bool KillOnCloseJob::Assign(void* process) {
  if (!usable_ || !process)
    return false;
  if (AssignProcessToJobObject(static_cast<HANDLE>(job_),
                               static_cast<HANDLE>(process)))
    return true;

  // ERROR_ACCESS_DENIED here means sninja is already running inside a job that
  // does not allow nesting.  That is a supported configuration, not a failure:
  // fall back to the caller terminating children directly.
  if (GetLastError() == ERROR_ACCESS_DENIED)
    usable_ = false;
  return false;
}

void KillOnCloseJob::KillAll() {
  if (usable_)
    TerminateJobObject(static_cast<HANDLE>(job_), 1);
}

KillOnCloseJob* KillOnCloseJob::Global() {
  static KillOnCloseJob* job = [] {
    KillOnCloseJob* j = new KillOnCloseJob();
    std::string ignored;
    j->Create(&ignored);
    return j;
  }();
  return job;
}
