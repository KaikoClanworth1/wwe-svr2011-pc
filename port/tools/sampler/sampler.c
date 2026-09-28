/*
 * Out-of-process sampling profiler for the SvR 2011 port.
 *
 *   sampler.exe <pid> <seconds> <report.txt> [max-depth]
 *
 * Every ~1 ms each thread of <pid> is suspended, its stack walked with
 * StackWalk64 (reading the target's memory, so the game can never deadlock
 * on us), and resumed. Stacks are aggregated per thread and symbolized with
 * DbgHelp (PDBs beside the modules, else export names). The report lists,
 * per thread, the share of samples and the hottest stacks - a thread that
 * spends its time in NtWaitForSingleObject under some caller is waiting on
 * that caller.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <timeapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

#define MAX_THREADS 256
#define MAX_DEPTH 48
#define MAX_STACKS 65536

typedef struct {
    DWORD tid;
    uint64_t hash;
    int depth;
    uint64_t frames[MAX_DEPTH];
    unsigned count;
} Stack;

typedef struct {
    DWORD tid;
    unsigned samples;
    WCHAR name[64];
} ThreadInfo;

static Stack s_stacks[MAX_STACKS];
static int s_nstacks;
static ThreadInfo s_threads[MAX_THREADS];
static int s_nthreads;

static ThreadInfo *thread_info(DWORD tid)
{
    for (int i = 0; i < s_nthreads; i++)
        if (s_threads[i].tid == tid)
            return &s_threads[i];
    if (s_nthreads == MAX_THREADS)
        return NULL;
    ThreadInfo *t = &s_threads[s_nthreads++];
    memset(t, 0, sizeof *t);
    t->tid = tid;
    return t;
}

static void record(DWORD tid, const uint64_t *frames, int depth)
{
    uint64_t h = 1469598103934665603ull ^ tid;
    for (int i = 0; i < depth; i++)
        h = (h ^ frames[i]) * 1099511628211ull;
    for (int i = 0; i < s_nstacks; i++) {
        if (s_stacks[i].hash == h && s_stacks[i].tid == tid) {
            s_stacks[i].count++;
            return;
        }
    }
    if (s_nstacks == MAX_STACKS)
        return;
    Stack *s = &s_stacks[s_nstacks++];
    s->tid = tid;
    s->hash = h;
    s->depth = depth;
    memcpy(s->frames, frames, depth * sizeof frames[0]);
    s->count = 1;
}

static int sample_thread(HANDLE proc, HANDLE th, uint64_t *frames, int max_depth)
{
    CONTEXT ctx;
    STACKFRAME64 sf;
    int depth = 0;
    if (SuspendThread(th) == (DWORD)-1)
        return 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &ctx)) {
        memset(&sf, 0, sizeof sf);
        sf.AddrPC.Offset = ctx.Rip;
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp;
        sf.AddrStack.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrFrame.Mode = AddrModeFlat;
        frames[depth++] = ctx.Rip;  /* the leaf, even if the walk fails */
        sf.AddrPC.Offset = ctx.Rip;
        int first = 1;
        while (depth < max_depth &&
               StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &ctx, NULL,
                           SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            if (!sf.AddrPC.Offset)
                break;
            if (first) {  /* StackWalk64 reports the leaf again first */
                first = 0;
                if (sf.AddrPC.Offset == ctx.Rip)
                    continue;
            }
            frames[depth++] = sf.AddrPC.Offset;
        }
    }
    ResumeThread(th);
    return depth;
}

static void symbolize(HANDLE proc, uint64_t addr, char *out, size_t n)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    IMAGEHLP_MODULE64 mod;
    DWORD64 disp = 0;
    const char *mname = "?";
    memset(&mod, 0, sizeof mod);
    mod.SizeOfStruct = sizeof mod;
    if (SymGetModuleInfo64(proc, addr, &mod))
        mname = mod.ModuleName;
    memset(buf, 0, sizeof buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(proc, addr, &disp, sym))
        snprintf(out, n, "%s!%s+0x%llx", mname, sym->Name, (unsigned long long)disp);
    else
        snprintf(out, n, "%s!0x%llx", mname, (unsigned long long)(addr - mod.BaseOfImage));
}

static int cmp_stack(const void *a, const void *b)
{
    const Stack *x = a, *y = b;
    if (x->tid != y->tid)
        return x->tid < y->tid ? -1 : 1;
    return (int)y->count - (int)x->count;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: sampler <pid> <seconds> <report.txt> [max-depth] [tid,tid,...]\n");
        return 2;
    }
    DWORD pid = (DWORD)atoi(argv[1]);
    double seconds = atof(argv[2]);
    int max_depth = argc > 4 ? atoi(argv[4]) : 24;
    DWORD only[64];
    int nonly = 0;
    if (argc > 5) {
        char *tok = strtok(argv[5], ",");
        while (tok && nonly < 64) {
            only[nonly++] = (DWORD)strtoul(tok, NULL, 10);
            tok = strtok(NULL, ",");
        }
    }
    if (max_depth > MAX_DEPTH)
        max_depth = MAX_DEPTH;

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        fprintf(stderr, "OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    if (!SymInitialize(proc, NULL, TRUE)) {
        fprintf(stderr, "SymInitialize failed: %lu\n", GetLastError());
        return 1;
    }

    HANDLE handles[MAX_THREADS];
    DWORD tids[MAX_THREADS];
    int nh = 0;
    unsigned rounds = 0;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    timeBeginPeriod(1);
    for (;;) {
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
        if (t >= seconds)
            break;
        if (rounds % 200 == 0) {  /* refresh the thread list */
            for (int i = 0; i < nh; i++)
                CloseHandle(handles[i]);
            nh = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = { sizeof te };
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid || nh == MAX_THREADS)
                        continue;
                    if (nonly) {
                        int keep = 0;
                        for (int k = 0; k < nonly; k++)
                            keep |= only[k] == te.th32ThreadID;
                        if (!keep)
                            continue;
                    }
                    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                           THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                    if (!th)
                        continue;
                    handles[nh] = th;
                    tids[nh++] = te.th32ThreadID;
                    ThreadInfo *ti = thread_info(te.th32ThreadID);
                    PWSTR desc = NULL;
                    if (ti && !ti->name[0] && SUCCEEDED(GetThreadDescription(th, &desc)) && desc) {
                        wcsncpy_s(ti->name, 64, desc, _TRUNCATE);
                        LocalFree(desc);
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        for (int i = 0; i < nh; i++) {
            uint64_t frames[MAX_DEPTH];
            int d = sample_thread(proc, handles[i], frames, max_depth);
            if (d > 0) {
                ThreadInfo *ti = thread_info(tids[i]);
                if (ti)
                    ti->samples++;
                record(tids[i], frames, d);
            }
        }
        rounds++;
        Sleep(1);
    }
    timeEndPeriod(1);

    FILE *f = fopen(argv[3], "w");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
    }
    fprintf(f, "%u sampling rounds over %.1f s\n\n", rounds, seconds);
    qsort(s_stacks, s_nstacks, sizeof s_stacks[0], cmp_stack);
    for (int ti = 0; ti < s_nthreads; ti++) {
        ThreadInfo *t = &s_threads[ti];
        if (!t->samples)
            continue;
        fprintf(f, "==== thread %lu \"%ls\" (%u samples)\n", t->tid, t->name, t->samples);
        int shown = 0;
        for (int i = 0; i < s_nstacks && shown < 6; i++) {
            Stack *s = &s_stacks[i];
            if (s->tid != t->tid)
                continue;
            fprintf(f, "  -- %5.1f%%  (%u)\n", 100.0 * s->count / t->samples, s->count);
            for (int k = 0; k < s->depth; k++) {
                char name[400];
                symbolize(proc, s->frames[k], name, sizeof name);
                fprintf(f, "       %s\n", name);
            }
            shown++;
        }
        fprintf(f, "\n");
    }
    fclose(f);
    SymCleanup(proc);
    printf("%u rounds, %d threads, %d distinct stacks -> %s\n", rounds, s_nthreads, s_nstacks, argv[3]);
    return 0;
}
