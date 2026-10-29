#pragma once
// A stall report in dlssnr-amd.log: when the game's frames stop for a while (or none came at all a minute after the
// module loaded), what every thread of the process is doing - each thread's stack as return addresses into the
// modules (module+offset, with the nearest export when the module has one) and, for the threads in NR's own code, the
// step they are in (Phase). A game that freezes leaves the place it froze in the log instead of nothing.
//
// The report suspends the threads one at a time only long enough to copy their registers and stack (nothing is
// allocated or locked meanwhile) and writes after they run again. At most four reports a process; NR_WATCHDOG=0: none.
// Implemented in nr_pe_log.cpp (linked into every module of ours).
namespace nr::pe::watchdog {

// The module's start (the .asi's InitializeASI, ReShade's add-on attach): the one-minute check for no frame at all.
void start();

// A frame went by (a Present). Starts the watchdog if start() was not called.
void beat();

// The watching thread ends and this waits until it has (it holds the module while it runs, and lets go as it ends):
// before a module of ours is unloaded - ReShade calls the add-on's AddonUninit, outside the loader lock, before its
// FreeLibrary when the game's last device goes. Never from DllMain. Nothing starts it again in this module's life.
void stop_and_wait();

// The step this thread is in, while the object lives (nested steps restore the outer one). `what` must be a literal.
struct Phase {
    explicit Phase(const char* what);
    ~Phase();
    Phase(const Phase&) = delete;
    Phase& operator=(const Phase&) = delete;
    const char* previous;
};

}  // namespace nr::pe::watchdog
