/**
 * @file
 * @brief UNIX-style crash handling functions (also used on Windows).
**/

#include "AppHdr.h"

#include "crash.h"

#if defined(UNIX)
#include <unistd.h>
#include <sys/param.h>
#ifndef __HAIKU__
        #define BACKTRACE_SUPPORTED
#endif
#endif

#ifdef USE_UNIX_SIGNALS
#include <sys/time.h>
#endif
#include <csignal>


#ifndef TARGET_OS_WINDOWS
# include <cerrno>
# include <sys/wait.h>
#endif

#ifdef BACKTRACE_SUPPORTED
#if defined(TARGET_CPU_MIPS) || \
    defined(TARGET_OS_FREEBSD) || \
    defined(TARGET_OS_NETBSD) || \
    defined(TARGET_OS_OPENBSD) || \
    defined(TARGET_COMPILER_CYGWIN) || \
    defined(__ANDROID__)
        #undef BACKTRACE_SUPPORTED
#endif
#endif

#ifdef BACKTRACE_SUPPORTED

#include <cxxabi.h>

#if !defined(TARGET_OS_MACOSX) && \
    !defined(TARGET_OS_WINDOWS) && \
    !defined(TARGET_COMPILER_CYGWIN)
#include <execinfo.h>
#endif

#ifdef TARGET_OS_MACOSX
#include <dlfcn.h>

typedef int (*backtrace_t)(void * *, int);
typedef char **(*backtrace_symbols_t)(void * const *, int);

// Used to convert from void* to function pointer (without a
// compiler warning).
template <typename TO, typename FROM> TO nasty_cast(FROM f)
{
    union
    {
        FROM f;
        TO t;
    } u;

    u.f = f;

    return u.t;
}
#endif // TARGET_OS_MACOSX

#endif // BACKTRACE_SUPPORTED

// Support Yama LSM ptrace restrictions
#ifdef TARGET_OS_LINUX
#   include <sys/prctl.h>
#   ifndef PR_SET_PTRACER
#       define PR_SET_PTRACER 0x59616d61
#   endif
#   ifndef PR_SET_PTRACER_ANY
#       define PR_SET_PTRACER_ANY ((unsigned long)-1)
#   endif
#endif

#include "files.h"
#include "initfile.h"
#include "options.h"
#include "state.h"
#include "stringutil.h"
#include "syscalls.h"
#include "threads.h"
#include "tiles-build-specific.h"

/////////////////////////////////////////////////////////////////////////////
// Code for printing out debugging info on a crash.
////////////////////////////////////////////////////////////////////////////
static int _crash_signal    = 0;
static int _recursion_depth = 0;

#ifdef TARGET_OS_WINDOWS
// Windows: hard crashes (access violations etc.) are structured exceptions,
// not signals, so the signal handlers below never see them. An unhandled
// exception filter routes them into crash_signal_handler(). Defined at the
// end of this file, away from <windows.h>'s macros.
static void _install_windows_crash_filter();
// "Access violation at foo() (file.cc:123)", filled in by the filter.
static string _windows_crash_where;
#endif
static mutex_t crash_mutex;

// Make this non-static so stack traces are easier to follow
void crash_signal_handler(int sig_num)
{
    // We rely on mutexes ignoring locks held by the same thread.
    // On some platforms, this must be explicitly enabled (which we do).

    // This mutex is never unlocked again -- the first thread to crash will
    // do a dump then terminate the process while everyone else waits here
    // forever.

    // XXX: This is a bit dangerous: if we catch a signal while any
    // non-asynch-signal-safe function is executing, and then call
    // pthread_mutex_lock() (which is also not asynch-signal-safe),
    // the behaviour is undefined.
    mutex_lock(crash_mutex);

    if (crawl_state.game_crashed)
    {
        if (_recursion_depth > 0)
            return;
        _recursion_depth++;

        fprintf(stderr, "Recursive crash.\n");

        string dir = (!Options.morgue_dir.empty() ? Options.morgue_dir :
                      !SysEnv.crawl_dir.empty()   ? SysEnv.crawl_dir
                                                  : "");

        if (!dir.empty() && dir[dir.length() - 1] != FILE_SEPARATOR)
            dir += FILE_SEPARATOR;

        char name[180];

        snprintf(name, sizeof(name), "%scrash-recursive-%s-%s.txt", dir.c_str(),
                you.your_name.c_str(), make_file_time(time(nullptr)).c_str());

        FILE* file = fopen_replace(name);

        if (file == nullptr)
            file = stderr;

        write_stack_trace(file);

        if (file != stderr)
            fclose(file);
        return;
    }

    _crash_signal            = sig_num;
    crawl_state.game_crashed = true;

    // During a crash, we may be in an inconsistent state (duh). Doing a number
    // of things can cause a lock up, especially calling non-reentrant functions
    // like malloc() and friends, used by C++ basics like std::string
    // internally.
    // There's no reliable way to ensure such things won't happen. A pragmatic
    // solution is to abort the crash dump.
    alarm(120);

    // In case the crash dumper is unable to open a file and has to dump
    // to stderr.
#ifndef USE_TILE_LOCAL
    if (crawl_state.io_inited)
        console_shutdown();
#endif

#ifdef USE_TILE_WEB
    tiles.shutdown();
#endif

#ifdef WATCHDOG
    /* Infinite loop protection.

       Not tickling the watchdog for 60 seconds of user CPU time (not wall
       time!) means something is terribly wrong. Even worst hogs like
       pre-0.6 god renouncement or current Time Step in the Abyss don't take
       more than several seconds.

       DGL only -- local players will notice the game is stuck and be able
       to kill it.

       It's likely to die horribly -- it's one of signals that is often
       received while in non-signal safe functions, especially malloc()
       which _will_ fuck the process up (remember, C++ can't blink without
       malloc()ing something). In such cases, alarm() above will kill us.
       That's nasty and random, but at least should give us backtraces most
       of the time, and avoid dragging down the servers. And even if for
       some odd reason SIGALRM won't kill us, the worst that can happen is
       wasting 100% CPU which is precisely what happens right now.
    */
    if (sig_num == SIGVTALRM)
        die_noline("Stuck game with 100%% CPU use\n");
#endif

    do_crash_dump();

    // Now crash for real.
    signal(sig_num, SIG_DFL);
    raise(sig_num);
}

void init_crash_handler()
{
    mutex_init(crash_mutex);

#if defined(USE_UNIX_SIGNALS)
    for (int i = 1; i <= 64; i++)
    {
#ifdef SIGALRM
        if (i == SIGALRM)
            continue;
#endif
#ifdef SIGHUP
        if (i == SIGHUP)
            continue;
#endif
#ifdef SIGQUIT
        if (i == SIGQUIT)
            continue;
#endif
#ifdef SIGINT
        if (i == SIGINT)
            continue;
#endif
#ifdef SIGCHLD
        if (i == SIGCHLD)
            continue;
#endif
#ifdef SIGTSTP
        if (i == SIGTSTP)
            continue;
#endif
#ifdef SIGCONT
        if (i == SIGCONT)
            continue;
#endif
#ifdef SIGIO
        if (i == SIGIO)
            continue;
#endif
#ifdef SIGPROF
        if (i == SIGPROF)
            continue;
#endif
#ifdef SIGTTOU
        if (i == SIGTTOU)
            continue;
#endif
#ifdef SIGTTIN
        if (i == SIGTTIN)
            continue;
#endif
#ifdef SIGKILL
        if (i == SIGKILL)
            continue;
#endif
#ifdef SIGSTOP
        if (i == SIGSTOP)
            continue;
#endif
#ifdef SIGWINCH
        if (i == SIGWINCH)
            continue;
#endif

        signal(i, crash_signal_handler);
    }

#endif // if defined(USE_UNIX_SIGNALS)

#ifdef TARGET_OS_WINDOWS
    _install_windows_crash_filter();
#endif
}

string crash_signal_info()
{
#if defined(UNIX)
    #ifdef TARGET_OS_FREEBSD
        // FreeBSD's strsignal was not working properly so we just check
        // to make sure that the signal is in the available list of signals,
        // and then look it up in the table of signal handler names that
        // FreeBSD exposes.
        const char *name = nullptr;
        if (_crash_signal >= SIGHUP && _crash_signal <= SIGLIBRT)
            name = sys_signame[_crash_signal];
    #else
        const char *name = strsignal(_crash_signal);
    #endif

    if (name == nullptr)
        name = "INVALID";
    return make_stringf("Crash caused by signal #%d: %s", _crash_signal, name);
#elif defined(TARGET_OS_WINDOWS)
    return _windows_crash_where;
#else
    return "";
#endif

}

#if defined(BACKTRACE_SUPPORTED)
void write_stack_trace(FILE* file)
{
    void* frames[50];

#if defined(TARGET_OS_MACOSX)
    backtrace_t backtrace;
    backtrace_symbols_t backtrace_symbols;
    backtrace = nasty_cast<backtrace_t, void*>(dlsym(RTLD_DEFAULT, "backtrace"));
    backtrace_symbols = nasty_cast<backtrace_symbols_t, void*>(dlsym(RTLD_DEFAULT, "backtrace_symbols"));
    if (!backtrace || !backtrace_symbols)
    {
        fprintf(stderr, "Couldn't get a stack trace.\n");
        fprintf(file, "Couldn't get a stack trace.\n");
        return;
    }
#endif

    int num_frames = backtrace(frames, ARRAYSZ(frames));
    char **symbols = backtrace_symbols(frames, num_frames);

#if !defined(TARGET_OS_MACOSX)
    if (symbols == nullptr)
    {
        fprintf(stderr, "Out of memory.\n");
        fprintf(file,   "Out of memory.\n");

        // backtrace_symbols_fd() can print out the stack trace even if
        // malloc() can't find any free memory.
        backtrace_symbols_fd(frames, num_frames, fileno(file));
        return;
    }
#endif

    fprintf(file, "Obtained %d stack frames.\n", num_frames);

    // Now we prettify the printout to even show demangled C++ function names.
    string bt = "";
    for (int i = 0; i < num_frames; i++)
    {
#if defined(TARGET_OS_MACOSX)
        char *addr = ::strstr(symbols[i], "0x");
        char *mangled = ::strchr(addr, ' ') + 1;
        char *offset = ::strchr(addr, '+');
        char *postmangle = ::strchr(mangled, ' ');
        if (mangled)
            *(mangled - 1) = 0;
        bt += addr;
        int status;
        bt += ": ";
        if (addr && mangled)
        {
            if (postmangle)
                *postmangle = '\0';
            char *realname = abi::__cxa_demangle(mangled, 0, 0, &status);
            if (realname)
                bt += realname;
            else
                bt += mangled;
            bt += " ";
            bt += offset;
            free(realname);
        }
#else // TARGET_OS_MACOSX
        bt += symbols[i];
        int status;
        // Extract the identifier from symbols[i]. It's inside of parens.
        char *firstparen = ::strchr(symbols[i], '(');
        char *lastparen = ::strchr(symbols[i], '+');
        if (firstparen != 0 && lastparen != 0 && firstparen < lastparen)
        {
            bt += ": ";
            *lastparen = '\0';
            char *realname = abi::__cxa_demangle(firstparen + 1, 0, 0, &status);
            if (realname != nullptr)
                bt += realname;
            free(realname);
        }
#endif
        bt += "\n";
    }

    fprintf(file, "%s", bt.c_str());

    free(symbols);
}
#elif !defined(TARGET_OS_WINDOWS) // BACKTRACE_SUPPORTED
void write_stack_trace(FILE* file)
{
    const char* msg = "Unable to get stack trace on this platform.\n";
    fprintf(stderr, "%s", msg);
    fprintf(file, "%s", msg);
}
#endif // (Windows: write_stack_trace() is at the end of this file.)

void call_gdb(FILE *file)
{
#ifndef TARGET_OS_WINDOWS
    if (crawl_state.no_gdb)
        return (void)fprintf(file, "%s\n", crawl_state.no_gdb);

    fprintf(file, "Trying to run gdb.\n");
    fflush(file); // so we can use fileno()

    char attach_cmd[20] = {};
    snprintf(attach_cmd, sizeof(attach_cmd), "attach %d", getpid());

#ifdef TARGET_OS_LINUX
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
    switch (int gdb = fork())
    {
    case -1:
        return (void)fprintf(file, "Couldn't fork: %s\n", strerror(errno));
    case 0:
        {
            int fd = fileno(file);
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);

            const char* argv[] =
            {
                "nice",
                GDB_PATH,
                "-batch",
                "-ex", "show version", // Too bad -iex needs gdb >=7.5 (jessie)
                "-ex", attach_cmd,
                "-ex", "bt full",
                0
            };
            execv("/usr/bin/nice", (char* const*)argv);
            printf("Failed to start gdb: %s\n", strerror(errno));
            fflush(stdout);
            _exit(0);
        }
        return;
    default:
        waitpid(gdb, 0, 0);
    }
#else
    UNUSED(file);
#endif
}

void disable_other_crashes()
{
    // If one thread calls end() without going through a crash (a handled
    // fatal error), no one else should be allowed to crash. We're already
    // going down so blocking the other thread is ok.
    mutex_lock(crash_mutex);
}

void watchdog()
{
#ifdef UNIX
    struct itimerval t;
    t.it_interval.tv_sec = 0;
    t.it_interval.tv_usec = 0;
    t.it_value.tv_sec = 60;
    t.it_value.tv_usec = 0;
    setitimer(ITIMER_VIRTUAL, &t, 0);
#else
    // Real time rather than CPU time.
    // This will break DGL, but it makes no sense on Windows anyway.
    // Mapstat is cool with this.
    alarm(60);
#endif
}

#ifdef TARGET_OS_WINDOWS
/////////////////////////////////////////////////////////////////////////////
// Windows crash handling: symbolised stack traces (dbghelp + the build's PDB)
// and an unhandled exception filter for hard crashes. Kept at the end of the
// file so <windows.h>'s macros can't interfere with the code above.
/////////////////////////////////////////////////////////////////////////////
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// windows.h may already have been included (via other headers) with this
// excluded, so declare it directly. It's exported by kernel32/ntdll.
extern "C" USHORT WINAPI RtlCaptureStackBackTrace(ULONG skip, ULONG count,
                                                 PVOID *frames, PULONG hash);

// "function (file.cc:123)" for a code address, as far as symbols allow.
static string _windows_symbolise(DWORD64 addr)
{
    HANDLE proc = GetCurrentProcess();
    char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    string out;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, addr, &disp, sym))
        out = sym->Name;
    else
        out = make_stringf("0x%llx", (unsigned long long)addr);

    IMAGEHLP_LINE64 line;
    line.SizeOfStruct = sizeof(line);
    DWORD ldisp = 0;
    if (SymGetLineFromAddr64(proc, addr, &ldisp, &line))
    {
        const char *file = strrchr(line.FileName, 0x5C);   // backslash
        out += make_stringf(" (%s:%lu)", file ? file + 1 : line.FileName,
                            (unsigned long)line.LineNumber);
    }
    return out;
}

static bool _windows_syms_ready()
{
    static bool tried = false, ok = false;
    if (!tried)
    {
        tried = true;
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        ok = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    }
    return ok;
}

void write_stack_trace(FILE* file)
{
    void *frames[62];
    const USHORT n = RtlCaptureStackBackTrace(0, ARRAYSZ(frames), frames, nullptr);
    if (!_windows_syms_ready())
    {
        fprintf(file, "Obtained %u stack frames (no symbols).\n", n);
        for (USHORT i = 0; i < n; i++)
            fprintf(file, "  %p\n", frames[i]);
        return;
    }
    fprintf(file, "Obtained %u stack frames.\n", n);
    for (USHORT i = 0; i < n; i++)
    {
        fprintf(file, "  %s\n",
                _windows_symbolise(reinterpret_cast<DWORD64>(frames[i])).c_str());
    }
}

static LONG WINAPI _windows_crash_filter(EXCEPTION_POINTERS *info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const DWORD64 where =
        reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress);
    const char *what = code == EXCEPTION_ACCESS_VIOLATION   ? "Access violation"
                     : code == EXCEPTION_STACK_OVERFLOW     ? "Stack overflow"
                     : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "Division by zero"
                                                            : "Exception";
    _windows_crash_where = make_stringf("%s (0x%08lX)", what, (unsigned long)code);
    if (_windows_syms_ready())
        _windows_crash_where += " at " + _windows_symbolise(where);
    if (code == EXCEPTION_ACCESS_VIOLATION
        && info->ExceptionRecord->NumberParameters >= 2)
    {
        _windows_crash_where += make_stringf(" (%s address 0x%llx)",
            info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
    }
    fprintf(stderr, "%s\n", _windows_crash_where.c_str());

    // The usual crash report (crash-*.txt with stack trace, messages...).
    // It raises SIGSEGV at the end, which terminates the process.
    crash_signal_handler(SIGSEGV);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void _install_windows_crash_filter()
{
    SetUnhandledExceptionFilter(_windows_crash_filter);
}
#endif // TARGET_OS_WINDOWS
