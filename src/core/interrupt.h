#ifndef SNINJA_INTERRUPT_H_
#define SNINJA_INTERRUPT_H_

// Console interrupt handling.
//
// Background commands are created with CREATE_NEW_PROCESS_GROUP, which means a
// Ctrl-C in the console no longer reaches them: only sninja is told about it.
// That is deliberate -- sninja decides what to stop -- but it makes sninja
// responsible for stopping the children itself, and until it does, a Ctrl-C
// would leave compilers running with no build supervising them.
//
// The handler does the minimum that is safe inside a console handler (set an
// event) and lets the workers notice at their next wait, where stopping the
// children and reporting the interrupted edge are ordinary code paths.

// Install the handler.  Safe to call more than once.  Does nothing when there is
// no console, which is the normal case under a GUI parent or a test runner.
void InstallConsoleInterruptHandler();

/// Escape hatch for the interrupt tests: with this set, children are not put in
/// the kill-on-close job, so the tests can observe what the console handler alone
/// does and prove the job object is load-bearing.  Not a supported configuration.
bool JobObjectEnabled();


// True once Ctrl-C, Ctrl-Break, or console close has been seen.
bool InterruptRequested();

// The event signalled by the handler, for code that has to wait on it alongside
// other handles.
void* ConsoleInterruptEvent();

// Ask the handler to stop the build.  Used by tests, which cannot deliver a real
// console Ctrl-C, and by any path that needs the same behaviour.
void RequestInterrupt();

#endif  // SNINJA_INTERRUPT_H_
