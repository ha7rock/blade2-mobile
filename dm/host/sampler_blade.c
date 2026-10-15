// 采样器（通用宿主版）：DM_SAMPLE=毫秒[:*]——定时用 SIGUSR2 打断客体主线程（或 * = 全部客体线程），记 pc / lr / 帧指针链，按 模块+偏移 打印
// 信号处理里只拷寄存器进无锁环（不打印、不加锁）；打印在采样线程做。偏移相对映像首址（= 模块 base）。
// Sampler (generic-host version): DM_SAMPLE=ms[:*] — periodically interrupt the guest main thread (or * = all guest threads) with SIGUSR2, record pc / lr / frame-pointer chain, print as module+offset
// The signal handler only copies registers into a lock-free ring (no printing, no locks); printing happens on the sampler thread. Offsets are relative to the image start (= module base).
#include "dm.h"
#include <stdatomic.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach.h>
#define SRING 1024
static struct { uint64_t t, pc, lr, bt[16]; int n; uint32_t h; } g_ring[SRING];
static _Atomic unsigned g_head;
static pthread_t g_self; static int g_all;
uint32_t thr_cur_handle(void); int thr_list(pthread_t *out, int max); uint64_t now_ns(void); extern uint64_t g_t0_ns;
static void on_sample(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si; ucontext_t *uc = uc_; unsigned i = atomic_load(&g_head) % SRING;
    g_ring[i].h = thr_cur_handle(); g_ring[i].t = now_ns(); g_ring[i].pc = uc->uc_mcontext->__ss.__pc; g_ring[i].lr = uc->uc_mcontext->__ss.__lr;
    uint64_t fp = uc->uc_mcontext->__ss.__fp; int n = 0;
    for (; n < 16 && fp && !(fp & 7); n++) {
        uint64_t fr[2]; vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), fp, sizeof fr, (vm_address_t)fr, &got) || got != sizeof fr) break;
        g_ring[i].bt[n] = fr[1]; if (fr[0] <= fp) { n++; break; } fp = fr[0];
    }
    g_ring[i].n = n; atomic_fetch_add(&g_head, 1);
}
static void off(const char *tag, uint64_t a) {
    for (int i = 0; i < g_nmod; i++) {
        uint64_t b = (uint64_t)(uintptr_t)g_mod[i].base, img = b, lo = g_mod[i].patch_off < 0 ? b - (uint64_t)(-g_mod[i].patch_off) : b;   // base 即映像首址；补丁区在映像前面（loader.c map_image） | base is the image start; the patch area lies below it (loader.c map_image)
        if (a >= lo && a < b + g_mod[i].size) { fprintf(stderr, " %s%s%s+%#llx", tag, *tag ? "=" : "", g_mod[i].name, (unsigned long long)(a - img)); return; }
    }
    fprintf(stderr, " %s%s%#llx", tag, *tag ? "=" : "", (unsigned long long)a);
}
static void *sampler(void *p) {
    useconds_t us = (useconds_t)(uintptr_t)p; unsigned printed = 0;
    for (;;) {
        usleep(us);
        if (g_all) { pthread_t all[64]; int n = thr_list(all, 64); for (int k = 0; k < n; k++) { pthread_kill(all[k], SIGUSR2); usleep(200); } }
        else pthread_kill(g_self, SIGUSR2);
        for (unsigned h = atomic_load(&g_head); printed < h; printed++) {
            unsigned i = printed % SRING;
            fprintf(stderr, "[dm] SAMPLE #%u [%.3fs] thr=%#x", printed + 1, (g_ring[i].t - g_t0_ns) / 1e9, g_ring[i].h); off("pc", g_ring[i].pc); off("lr", g_ring[i].lr);
            fprintf(stderr, " bt:"); for (int k = 0; k < g_ring[i].n; k++) off("", g_ring[i].bt[k]); fputc('\n', stderr);
        }
    }
}
void crash_sample_self(void) {
    const char *e = getenv("DM_SAMPLE"); if (!e) return;
    g_self = pthread_self(); const char *c = strchr(e, ':'); g_all = c && c[1] == '*';
    struct sigaction sa = {.sa_sigaction = on_sample, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigaction(SIGUSR2, &sa, 0);
    pthread_t t; pthread_create(&t, NULL, sampler, (void *)(uintptr_t)(atoi(e) * 1000)); pthread_detach(t);
}

// ---------- 探针 DM_PROBE=模块:映像内偏移[,…]（通用宿主版，移植自 diag.c）：客体地址上放 brk，命中时打 x0..x3 / x19..x23 / x8 与回溯，再从跳板执行原指令 ----------
// 偏移相对映像首址（= 模块 base；与 sdis.py / 采样器一致）。跳板借用 x16：只放在函数入口或 x16 死的位置；原指令不能是 PC 相对指令。
// ---------- Probes DM_PROBE=module:image-offset[,...] (generic-host version, ported from diag.c): brk at the guest address, on hit print x0..x3 / x19..x23 / x8 and a backtrace, then run the original instruction from a trampoline ----------
// Offsets are relative to the image start (= module base; same as sdis.py / the sampler). The trampoline borrows x16: only at function entries or where x16 is dead; the original instruction must not be PC-relative.
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#define MAX_PROBE 16
static struct { uint64_t at; uint32_t *slot; unsigned hits; char name[40]; } g_probe[MAX_PROBE];
static int g_nprobe; static uint32_t *g_slots;
void crash_set_trap_hook(int (*fn)(ucontext_t *uc)); void dm_backtrace(uint64_t fp);
static int probe_hit(ucontext_t *uc) {
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    for (int i = 0; i < g_nprobe; i++) if (g_probe[i].at == pc) {
        fprintf(stderr, "[dm] PROBE %s #%u [%.4fs] thr=%#x", g_probe[i].name, ++g_probe[i].hits, (now_ns() - g_t0_ns) / 1e9, thr_cur_handle());
        for (int r = 0; r < 4; r++) fprintf(stderr, " x%d=%#llx", r, uc->uc_mcontext->__ss.__x[r]);
        for (int r = 19; r < 24; r++) fprintf(stderr, " x%d=%#llx", r, uc->uc_mcontext->__ss.__x[r]);
        fprintf(stderr, " x8=%#llx lr=", uc->uc_mcontext->__ss.__x[8]); off("", uc->uc_mcontext->__ss.__lr); fputc('\n', stderr);
        dm_backtrace(uc->uc_mcontext->__ss.__fp);
        uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot; return 1;
    }
    return 0;
}
void probe_install(void) {
    const char *e = getenv("DM_PROBE"); if (!e) return;
    crash_set_trap_hook(probe_hit);
    g_slots = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    char spec[512]; strlcpy(spec, e, sizeof spec);
    for (char *t = strtok(spec, ","); t; t = strtok(NULL, ",")) {
        char *c = strchr(t, ':'); if (!c) die("DM_PROBE 格式：模块:偏移"); *c = 0; module_t *m = NULL;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, t)) m = &g_mod[i];
        if (!m || g_nprobe >= MAX_PROBE) die("DM_PROBE 无此模块或探针过多 %s", t);
        uint64_t off_ = strtoull(c + 1, 0, 16), at = (uint64_t)(uintptr_t)m->base + off_;   // base 即映像首址 | base is the image start
        uint32_t *w = (uint32_t *)at, *sl = g_slots + g_nprobe * 8;
        sl[0] = *w; sl[1] = 0x58000050; sl[2] = 0xD61F0200; *(uint64_t *)(sl + 3) = at + 4;   // 原指令；ldr x16,#8；br x16；回跳地址 | original; ldr x16,#8; br x16; return address
        uint64_t pg = at & ~0x3FFFull;
        if (mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE)) die("probe mprotect");
        *w = 0xD43E0000; mprotect((void *)pg, 0x4000, PROT_READ | PROT_EXEC); sys_icache_invalidate(w, 4);
        g_probe[g_nprobe].at = at; g_probe[g_nprobe].slot = sl; snprintf(g_probe[g_nprobe].name, sizeof g_probe[0].name, "%s+%#llx", m->name, (unsigned long long)off_);
        LOG("探针 %s（原指令 %08x）", g_probe[g_nprobe].name, sl[0]); g_nprobe++;
    }
    mprotect(g_slots, 0x4000, PROT_READ | PROT_EXEC); sys_icache_invalidate(g_slots, 0x4000);
}
