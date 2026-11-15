/* Native replacements for hot translated PPU functions.
 *
 * Each is the lifted function written again with the guest registers in
 * locals instead of ctx, its condition-register updates folded, and the
 * guest stack slots it writes and reads back kept in locals and stored once
 * on the way out. On return every register, the condition register and those
 * stack slots hold exactly what the lifted function leaves; every other guest
 * load is the same vm_read32 in the same order. include/port_ppu_native.h
 * makes the lifted definitions weak so these take every call.
 *
 * PORT_PPU_NATIVE=0 runs the lifted functions; PORT_PPU_NATIVE=check runs
 * both on each call, keeps the lifted result and reports any difference. */
#include "port_ppu_memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

void port_lifted_func_004B5120(ppu_context* ctx);

namespace {

int native_mode()
{
    static int mode = -1;               /* benign race: same value either way */
    if (mode < 0) {
        const char* v = std::getenv("PORT_PPU_NATIVE");
        mode = !v ? 1 : !std::strcmp(v, "check") ? 2 : std::atoi(v) ? 1 : 0;
    }
    return mode;
}

/* Compare two sign-extended words into CR field `shift`, with XER[SO]. */
#define CMPW(shift, a, b) do { const int64_t _a = (a), _b = (b); \
    cr = (cr & ~(0xFu << (shift))) | \
         ((((_a < _b) ? 8u : (_a > _b) ? 4u : 2u) | so) << (shift)); } while (0)

/* Find, in the list headed at r3+0x104, the entry whose node (entry+0) has
 * key r4 at node+0xC: r3 = the node, or 0. Entries link through entry+8; a
 * null link reads as 1, the end. 22.5% of the runTaskMain thread in STG02
 * combat when lifted: every step stored the current entry to sp-4 and loaded
 * it back, and stored 1 to sp-0xC, through the guarded accessors. */
void native_004B5120(ppu_context* ctx)
{
    uint64_t r0 = ctx->gpr[0], r3 = ctx->gpr[3], r5 = ctx->gpr[5], r6 = ctx->gpr[6];
    uint64_t r7 = ctx->gpr[7], r8 = ctx->gpr[8], r9 = ctx->gpr[9], r10;
    uint64_t r11 = ctx->gpr[11], r12 = ctx->gpr[12];
    const uint64_t r4 = ctx->gpr[4], sp = ctx->gpr[1];
    const uint32_t so = (ctx->xer >> 31) & 1u;
    uint32_t cr = ctx->cr;
    uint32_t slot4;                     /* [sp-4]; [sp-0xC] is always 1 */

    r10 = r3;
    r0 = vm_read32(r10 + 0x104); r10 += 0x104;
    CMPW(0, (int32_t)r0, 0);
    if ((cr >> 1) & 1) goto L51D8;
L5130:
    slot4 = (uint32_t)r0;
    r9 = slot4;
    r0 = 1;
    CMPW(28, (int32_t)r0, (int32_t)r9);
    if ((cr >> 29) & 1) goto L51C4;
    r11 = r9 & 0xFFFFFFFFull;          /* rldicl r11,r9,0,32 */
    r3 = vm_read32(r11 + 0x0);
    r5 = vm_read32(r3 + 0xC);
    CMPW(24, (int32_t)r5, (int32_t)r4);
    if (!((cr >> 25) & 1)) goto L5198;
    goto out;
L5160:
    r9 = vm_read32(r11 + 0x8);
    CMPW(0, (int32_t)r9, 0);
    if ((cr >> 1) & 1) goto L51CC;
    slot4 = (uint32_t)r9;
L5170:
    r9 = slot4;
    r8 = 1;
    CMPW(28, (int32_t)r8, (int32_t)r9);
    r11 = r9 & 0xFFFFFFFFull;          /* rldicl r11,r9,0,32 */
    if ((cr >> 29) & 1) goto L51C4;
L5188:
    r3 = vm_read32(r11 + 0x0);
    r12 = vm_read32(r3 + 0xC);
    CMPW(24, (int32_t)r12, (int32_t)r4);
    if ((cr >> 25) & 1) goto out;
L5198:
    r3 = slot4;
    CMPW(4, (int32_t)r3, (int64_t)-1);
    if (!((cr >> 5) & 1)) goto L5160;
    r6 = vm_read32(r10 + 0x0);
    r8 = 1;
    slot4 = (uint32_t)r6;
    r9 = slot4;
    CMPW(28, (int32_t)r8, (int32_t)r9);
    r11 = r9 & 0xFFFFFFFFull;          /* rldicl r11,r9,0,32 */
    if (!((cr >> 29) & 1)) goto L5188;
L51C4:
    r3 = 0;
    goto out;
L51CC:
    r7 = 1;
    slot4 = (uint32_t)r7;
    goto L5170;
L51D8:
    r0 = 1;
    goto L5130;
out:
    ctx->gpr[0] = r0; ctx->gpr[3] = r3; ctx->gpr[5] = r5; ctx->gpr[6] = r6;
    ctx->gpr[7] = r7; ctx->gpr[8] = r8; ctx->gpr[9] = r9; ctx->gpr[10] = r10;
    ctx->gpr[11] = r11; ctx->gpr[12] = r12;
    ctx->cr = cr;
    vm_write32(sp + -0x4, slot4);
    vm_write32(sp + -0xC, 1u);
}

#undef CMPW

/* PORT_PPU_NATIVE=check: the lifted function and then the native one from the
 * same registers; the lifted result is kept. */
void check(const char* name, void (*lifted)(ppu_context*), void (*native)(ppu_context*),
           ppu_context* ctx)
{
    static unsigned long calls, bad;
    const uint64_t sp = ctx->gpr[1];
    ppu_context want = *ctx, got = *ctx;
    lifted(&want);
    const uint32_t want4 = vm_read32(sp - 0x4), wantC = vm_read32(sp - 0xC);
    native(&got);
    const uint32_t got4 = vm_read32(sp - 0x4), gotC = vm_read32(sp - 0xC);
    if ((std::memcmp(&want, &got, sizeof want) || want4 != got4 || wantC != gotC) && bad++ < 16)
        std::fprintf(stderr, "[ppu-native] MISMATCH %s: r3 %llx/%llx cr %08x/%08x sp-4 %08x/%08x\n", name,
                     (unsigned long long)want.gpr[3], (unsigned long long)got.gpr[3],
                     want.cr, got.cr, want4, got4);
    if (++calls % 1000000ul == 0)
        std::fprintf(stderr, "[ppu-native] check %s: %lu calls, %lu mismatches\n", name, calls, bad);
    vm_write32(sp - 0x4, want4);
    vm_write32(sp - 0xC, wantC);
    *ctx = want;
}

}  // namespace

void func_004B5120(ppu_context* ctx)
{
    switch (native_mode()) {
    case 1:  native_004B5120(ctx); break;
    case 0:  port_lifted_func_004B5120(ctx); break;
    default: check("func_004B5120", port_lifted_func_004B5120, native_004B5120, ctx); break;
    }
}
