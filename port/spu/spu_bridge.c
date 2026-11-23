/* BLJS10220 exact-image registrations. Static rejection is in spu/inventory.json. */
#include "spu_workload.h"
#include "spu_interp.h"
#include <stdio.h>
#include <stdlib.h>
#if __has_include("battride_audio/audio_task.h")
#include "battride_audio/audio_task.h"   /* scripts/build/lift_audio_spu.py */
#endif
/* The stripped executable segment includes words the static translator
 * rejects. Interpret live instructions rather than silently skipping them. */
extern void spu_spurs_taskset_syscall(spu_context*);
extern uint32_t port_spu_interp_resume(spu_context*, uint32_t, uint32_t);
extern void spu_begin_image(int image_id);
#define AUDIO_IMAGE_ID 506692475

#ifdef PORT_AUDIO_LIFTED
/* The task's code below its data tail and the overlays it loads (the MP3
 * codec, the module embedded in the executable), translated by
 * scripts/build/lift_audio_spu.py: interpreted, the task filled about a third of
 * the cellAudio blocks in time. A translated function is run when the
 * interpreter reaches it, unless the script left it interpreted, and for an
 * overlay only while local store holds that overlay's instructions there
 * (another one may be loaded at the same address). Its transfers are followed here;
 * anything else goes back to the interpreter. PORT_AUDIO_INTERPRET=1
 * interprets everything. */
#define AUDIO_COUNT(a) (sizeof (a) / sizeof (a)[0])
static int audio_compiled_ok(const spu_context* ctx, uint32_t pc)
{
    if (pc < PORT_AUDIO_TASK_CODE_END) {
        size_t lo = 0, hi = AUDIO_COUNT(port_audio_interpret_only);
        while (lo < hi) { size_t m = (lo + hi) / 2;
            if (port_audio_interpret_only[m] < pc) lo = m + 1; else hi = m; }
        return lo == AUDIO_COUNT(port_audio_interpret_only) || port_audio_interpret_only[lo] != pc;
    }
#ifdef PORT_AUDIO_CODEC
    for (size_t r = 0; r < AUDIO_COUNT(port_audio_codec_ranges); r++) {
        if (pc < port_audio_codec_ranges[r].lo || pc >= port_audio_codec_ranges[r].hi) continue;
        size_t lo = 0, hi = AUDIO_COUNT(port_audio_codec_entries);
        while (lo < hi) { size_t m = (lo + hi) / 2;
            if (port_audio_codec_entries[m].pc < pc) lo = m + 1; else hi = m; }
        if (lo == AUDIO_COUNT(port_audio_codec_entries) || port_audio_codec_entries[lo].pc != pc) return 0;
        for (int i = 0; i < 4; i++) {
            const uint8_t* p = ctx->ls + pc + 4u * (uint32_t)i;
            uint32_t w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
            if (w != port_audio_codec_entries[lo].w[i]) return 0;
        }
        return 1;
    }
#endif
    return 0;
}

/* The call drain of the translated audio code (lift_audio_spu.py puts it in
 * place of the runtime's spu_drain_call): run the callee, and whatever it
 * transfers to, until the pc comes back to return_pc. A transfer to code with
 * no translation -- or one that must stay interpreted -- is interpreted until
 * it returns, reaches translated code again or calls the task syscall,
 * instead of unwinding the whole task (spu_restart_dispatch), so a function
 * with an indirect call can run translated (docs/PLANO0.6.1.md, N1). */
extern SPU_THREAD_LOCAL uint32_t port_spu_interp_stop2;
extern void spu_halt(spu_context*);
void port_audio_drain(spu_context* ctx, uint32_t return_pc)
{
    const uint32_t ret = return_pc & SPU_LS_MASK;
    for (;;) {
        void (*tf)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        ctx->pc &= SPU_LS_MASK;
        const uint32_t pc = ctx->pc;
        if (!tf || tf == spu_indirect_branch) {
            if (pc == ret) return;
            if (pc == SPURS_TASKSET_PM_SYSCALL_LS) {        /* as battride_audio_task */
                spu_spurs_taskset_syscall(ctx);
                ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
                continue;
            }
            tf = audio_compiled_ok(ctx, pc) ? spu_lifted_lookup(ctx, pc) : 0;
        } else if (!audio_compiled_ok(ctx, pc)) {
            tf = 0;
        }
        if (!tf) {
            const uint32_t outer = port_spu_interp_stop2;
            port_spu_interp_stop2 = SPURS_TASKSET_PM_SYSCALL_LS;
            port_spu_interp_resume(ctx, pc, ret);
            port_spu_interp_stop2 = outer;
            if (ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT)) {
                spu_halt(ctx);
                return;
            }
            continue;
        }
        if (ctx->int_enable && (ctx->event_status & ctx->event_mask))
            tf = spu_take_interrupt(ctx, tf);
        tf(ctx);
    }
}

/* Run translated code from ctx->pc until it leaves translated code.
 * Returns with ctx->pc where interpretation continues. */
static void audio_run_compiled(spu_context* ctx, spu_lifted_fn fn)
{
    void (*tf)(spu_context*) = fn;
    while (tf) {
        if (ctx->int_enable && (ctx->event_status & ctx->event_mask))
            tf = spu_take_interrupt(ctx, tf);
        tf(ctx);
        tf = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        if (!tf) break;
        ctx->pc &= SPU_LS_MASK;
        if (ctx->pc == SPURS_TASKSET_PM_SYSCALL_LS || !audio_compiled_ok(ctx, ctx->pc)) break;
        if (tf == spu_indirect_branch && !(tf = spu_lifted_lookup(ctx, ctx->pc))) break;
    }
}
#endif

static void battride_audio_task(spu_context* ctx)
{
    uint32_t pc=0x3050u;
    int resume=0;
#ifdef PORT_AUDIO_LIFTED
    static int interpret_all=-1;
    if (interpret_all<0) { const char* e=getenv("PORT_AUDIO_INTERPRET"); interpret_all=e && *e && *e!='0'; }
#endif
    for (;;) {
        if (resume) port_spu_interp_resume(ctx, pc, SPURS_TASKSET_PM_SYSCALL_LS);
        else spu_interp_run_until(ctx, pc, SPURS_TASKSET_PM_SYSCALL_LS);
        resume=0;
        if (ctx->pc == SPURS_TASKSET_PM_SYSCALL_LS) {
            /* The async task adapter planted this synthetic task-API sentinel.
             * Service it using the same HLE as lifted SPURS tasks. */
            spu_spurs_taskset_syscall(ctx);
            pc=ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            continue;
        }
        if (ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT)) return;
        /* The interpreter stopped at a lifted entry. */
        pc=ctx->pc & SPU_LS_MASK;
#ifdef PORT_AUDIO_LIFTED
        spu_lifted_fn fn;
        if (!interpret_all && audio_compiled_ok(ctx, pc) && (fn=spu_lifted_lookup(ctx, pc))) {
            audio_run_compiled(ctx, fn);
            pc=ctx->pc & SPU_LS_MASK;
            continue;
        }
#endif
        resume=1;
    }
}
void port_register_spu(void)
{
#ifdef PORT_AUDIO_LIFTED
    spu_begin_image(AUDIO_IMAGE_ID);
    spu_audio_spu_recomp_register();
#ifdef PORT_AUDIO_CODEC
    port_audio_codec_register();
#endif
    spu_begin_image(0);
#endif
    spu_workload_register_img(0xC1759183E2FF7448ULL, battride_audio_task,
                             AUDIO_IMAGE_ID, "battride_audio_task.interpreted");
    spu_workload_register_img(0xDA70F59822356AD0ULL, battride_audio_task,
                             AUDIO_IMAGE_ID, "battride_audio_task.text.interpreted");
#ifdef PORT_AUDIO_LIFTED
    fprintf(stderr, "[port] SPU audio image registered: translated task%s, interpreter for the rest\n",
#ifdef PORT_AUDIO_CODEC
            " and overlays"
#else
            ""
#endif
            );
#else
    fprintf(stderr, "[port] SPU audio image registered with interpreter fallback\n");
#endif
}
