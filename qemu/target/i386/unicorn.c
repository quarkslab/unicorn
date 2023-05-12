/* Unicorn Emulator Engine */
/* By Nguyen Anh Quynh <aquynh@gmail.com>, 2015 */
/* Modified for Unicorn Engine by Chen Huitao<chenhuitao@hfmrit.com>, 2020 */

#include "uc_priv.h"
#include "sysemu/cpus.h"
#include "cpu.h"
#include "unicorn_common.h"
#include <unicorn/x86.h> /* needed for uc_x86_mmr */
#include "unicorn.h"

#define FPST(n) (env->fpregs[(env->fpstt + (n)) & 7].d)

#define X86_NON_CS_FLAGS (DESC_P_MASK | DESC_S_MASK | DESC_W_MASK | DESC_A_MASK)
static void load_seg_16_helper(CPUX86State *env, int seg, uint32_t selector)
{
    cpu_x86_load_seg_cache(env, seg, selector, (selector << 4), 0xffff,
                           X86_NON_CS_FLAGS);
}

void cpu_get_fp80(uint64_t *pmant, uint16_t *pexp, floatx80 f);
floatx80 cpu_set_fp80(uint64_t mant, uint16_t upper);

extern void helper_wrmsr(CPUX86State *env);
extern void helper_rdmsr(CPUX86State *env);

static void x86_set_pc(struct uc_struct *uc, uint64_t address)
{
    if (uc->mode == UC_MODE_16) {
        int16_t cs = (uint16_t)X86_CPU(uc->cpu)->env.segs[R_CS].selector;
        ((CPUX86State *)uc->cpu->env_ptr)->eip = address - cs * 16;
    } else
        ((CPUX86State *)uc->cpu->env_ptr)->eip = address;
}

static uint64_t x86_get_pc(struct uc_struct *uc)
{
    if (uc->mode == UC_MODE_16) {
        return X86_CPU(uc->cpu)->env.segs[R_CS].selector * 16 +
               ((CPUX86State *)uc->cpu->env_ptr)->eip;
    } else {
        return ((CPUX86State *)uc->cpu->env_ptr)->eip;
    }
}

static void x86_release(void *ctx)
{
    int i;
    TCGContext *tcg_ctx = (TCGContext *)ctx;
    X86CPU *cpu = (X86CPU *)tcg_ctx->uc->cpu;
    CPUTLBDesc *d = cpu->neg.tlb.d;
    CPUTLBDescFast *f = cpu->neg.tlb.f;
    CPUTLBDesc *desc;
    CPUTLBDescFast *fast;
    X86CPUClass *xcc = X86_CPU_GET_CLASS(cpu);

    release_common(ctx);
    for (i = 0; i < NB_MMU_MODES; i++) {
        desc = &(d[i]);
        fast = &(f[i]);
        g_free(desc->iotlb);
        g_free(fast->table);
    }

    free(xcc->model);
}

void x86_reg_reset(struct uc_struct *uc)
{
    CPUArchState *env = uc->cpu->env_ptr;

    memset(env->regs, 0, sizeof(env->regs));
    memset(env->segs, 0, sizeof(env->segs));
    memset(env->cr, 0, sizeof(env->cr));

    memset(&env->ldt, 0, sizeof(env->ldt));
    memset(&env->gdt, 0, sizeof(env->gdt));
    memset(&env->tr, 0, sizeof(env->tr));
    memset(&env->idt, 0, sizeof(env->idt));

    env->eip = 0;
    cpu_load_eflags(env, 0, -1);
    env->cc_op = CC_OP_EFLAGS;

    env->fpstt = 0; /* top of stack index */
    env->fpus = 0;
    env->fpuc = 0;
    memset(env->fptags, 0, sizeof(env->fptags)); /* 0 = valid, 1 = empty */

    env->mxcsr = 0;
    memset(env->xmm_regs, 0, sizeof(env->xmm_regs));
    memset(&env->xmm_t0, 0, sizeof(env->xmm_t0));
    memset(&env->mmx_t0, 0, sizeof(env->mmx_t0));

    memset(env->ymmh_regs, 0, sizeof(env->ymmh_regs));

    memset(env->opmask_regs, 0, sizeof(env->opmask_regs));
    memset(env->zmmh_regs, 0, sizeof(env->zmmh_regs));

    /* sysenter registers */
    env->sysenter_cs = 0;
    env->sysenter_esp = 0;
    env->sysenter_eip = 0;
    env->efer = 0;
    env->star = 0;

    env->vm_hsave = 0;

    env->tsc = 0;
    env->tsc_adjust = 0;
    env->tsc_deadline = 0;

    env->mcg_status = 0;
    env->msr_ia32_misc_enable = 0;
    env->msr_ia32_feature_control = 0;

    env->msr_fixed_ctr_ctrl = 0;
    env->msr_global_ctrl = 0;
    env->msr_global_status = 0;
    env->msr_global_ovf_ctrl = 0;
    memset(env->msr_fixed_counters, 0, sizeof(env->msr_fixed_counters));
    memset(env->msr_gp_counters, 0, sizeof(env->msr_gp_counters));
    memset(env->msr_gp_evtsel, 0, sizeof(env->msr_gp_evtsel));

#ifdef TARGET_X86_64
    memset(env->hi16_zmm_regs, 0, sizeof(env->hi16_zmm_regs));
    env->lstar = 0;
    env->cstar = 0;
    env->fmask = 0;
    env->kernelgsbase = 0;
#endif

    // TODO: reset other registers in CPUX86State qemu/target-i386/cpu.h

    // properly initialize internal setup for each mode
    switch (uc->mode) {
    default:
        break;
    case UC_MODE_16:
        env->hflags = 0;
        env->cr[0] = 0;
        // undo the damage done by the memset of env->segs above
        // for R_CS, not quite the same as x86_cpu_reset
        cpu_x86_load_seg_cache(env, R_CS, 0, 0, 0xffff,
                               DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK |
                                   DESC_R_MASK | DESC_A_MASK);
        // remainder yields same state as x86_cpu_reset
        load_seg_16_helper(env, R_DS, 0);
        load_seg_16_helper(env, R_ES, 0);
        load_seg_16_helper(env, R_SS, 0);
        load_seg_16_helper(env, R_FS, 0);
        load_seg_16_helper(env, R_GS, 0);

        break;
    case UC_MODE_32:
        env->hflags |= HF_CS32_MASK | HF_SS32_MASK | HF_OSFXSR_MASK;
        cpu_x86_update_cr0(env, CR0_PE_MASK); // protected mode
        break;
    case UC_MODE_64:
        env->hflags |= HF_CS32_MASK | HF_SS32_MASK | HF_CS64_MASK |
                       HF_LMA_MASK | HF_OSFXSR_MASK;
        env->hflags &= ~(HF_ADDSEG_MASK);
        env->efer |= MSR_EFER_LMA | MSR_EFER_LME; // extended mode activated
        cpu_x86_update_cr0(env, CR0_PE_MASK);     // protected mode
        /* If we are operating in 64bit mode then add the Long Mode flag
         * to the CPUID feature flag
         */
        env->features[FEAT_8000_0001_EDX] |= CPUID_EXT2_LM;
        break;
    }
}

static int x86_msr_read(CPUX86State *env, uc_x86_msr *msr)
{
    uint64_t ecx = env->regs[R_ECX];
    uint64_t eax = env->regs[R_EAX];
    uint64_t edx = env->regs[R_EDX];

    env->regs[R_ECX] = msr->rid;
    helper_rdmsr(env);

    msr->value = ((uint32_t)env->regs[R_EAX]) |
                 ((uint64_t)((uint32_t)env->regs[R_EDX]) << 32);

    env->regs[R_EAX] = eax;
    env->regs[R_ECX] = ecx;
    env->regs[R_EDX] = edx;

    /* The implementation doesn't throw exception or return an error if there is
     * one, so we will return 0.  */
    return 0;
}

static int x86_msr_write(CPUX86State *env, uc_x86_msr *msr)
{
    uint64_t ecx = env->regs[R_ECX];
    uint64_t eax = env->regs[R_EAX];
    uint64_t edx = env->regs[R_EDX];

    env->regs[R_ECX] = msr->rid;
    env->regs[R_EAX] = (unsigned int)msr->value;
    env->regs[R_EDX] = (unsigned int)(msr->value >> 32);
    helper_wrmsr(env);

    env->regs[R_ECX] = ecx;
    env->regs[R_EAX] = eax;
    env->regs[R_EDX] = edx;

    /* The implementation doesn't throw exception or return an error if there is
     * one, so we will return 0.  */
    return 0;
}

static uc_err reg_read(CPUX86State *env, unsigned int regid, void *value,
                       size_t *size, uc_mode mode)
{
    uc_err ret = UC_ERR_ARG;

    switch (regid) {
    default:
        break;
    case UC_X86_REG_FP0:
    case UC_X86_REG_FP1:
    case UC_X86_REG_FP2:
    case UC_X86_REG_FP3:
    case UC_X86_REG_FP4:
    case UC_X86_REG_FP5:
    case UC_X86_REG_FP6:
    case UC_X86_REG_FP7: {
        CHECK_REG_TYPE(char[10]);
        floatx80 reg = env->fpregs[regid - UC_X86_REG_FP0].d;
        cpu_get_fp80(value, (uint16_t *)((char *)value + sizeof(uint64_t)),
                     reg);
        return ret;
    }
    case UC_X86_REG_FPSW: {
        CHECK_REG_TYPE(uint16_t);
        uint16_t fpus = env->fpus;
        fpus = fpus & ~0x3800;
        fpus |= (env->fpstt & 0x7) << 11;
        *(uint16_t *)value = fpus;
        return ret;
    }
    case UC_X86_REG_FPCW:
        CHECK_REG_TYPE(uint16_t);
        *(uint16_t *)value = env->fpuc;
        return ret;
    case UC_X86_REG_FPTAG: {
        CHECK_REG_TYPE(uint16_t);
#define EXPD(fp) (fp.l.upper & 0x7fff)
#define MANTD(fp) (fp.l.lower)
#define MAXEXPD 0x7fff
        int fptag, exp, i;
        uint64_t mant;
        CPU_LDoubleU tmp;
        fptag = 0;
        for (i = 7; i >= 0; i--) {
            fptag <<= 2;
            if (env->fptags[i]) {
                fptag |= 3;
            } else {
                tmp.d = env->fpregs[i].d;
                exp = EXPD(tmp);
                mant = MANTD(tmp);
                if (exp == 0 && mant == 0) {
                    /* zero */
                    fptag |= 1;
                } else if (exp == 0 || exp == MAXEXPD ||
                           (mant & (1LL << 63)) == 0) {
                    /* NaNs, infinity, denormal */
                    fptag |= 2;
                }
            }
        }
        *(uint16_t *)value = fptag;
        return ret;
    }
    case UC_X86_REG_XMM0:
    case UC_X86_REG_XMM1:
    case UC_X86_REG_XMM2:
    case UC_X86_REG_XMM3:
    case UC_X86_REG_XMM4:
    case UC_X86_REG_XMM5:
    case UC_X86_REG_XMM6:
    case UC_X86_REG_XMM7: {
        CHECK_REG_TYPE(float64[2]);
        float64 *dst = (float64 *)value;
        ZMMReg *reg = (ZMMReg *)&env->xmm_regs[regid - UC_X86_REG_XMM0];
        dst[0] = reg->ZMM_Q(0);
        dst[1] = reg->ZMM_Q(1);
        return ret;
    }
    case UC_X86_REG_ST0:
    case UC_X86_REG_ST1:
    case UC_X86_REG_ST2:
    case UC_X86_REG_ST3:
    case UC_X86_REG_ST4:
    case UC_X86_REG_ST5:
    case UC_X86_REG_ST6:
    case UC_X86_REG_ST7: {
        CHECK_REG_TYPE(char[10]);
        memcpy(value, &FPST(regid - UC_X86_REG_ST0), 10);
        return ret;
    }
    case UC_X86_REG_YMM0:
    case UC_X86_REG_YMM1:
    case UC_X86_REG_YMM2:
    case UC_X86_REG_YMM3:
    case UC_X86_REG_YMM4:
    case UC_X86_REG_YMM5:
    case UC_X86_REG_YMM6:
    case UC_X86_REG_YMM7:
    case UC_X86_REG_YMM8:
    case UC_X86_REG_YMM9:
    case UC_X86_REG_YMM10:
    case UC_X86_REG_YMM11:
    case UC_X86_REG_YMM12:
    case UC_X86_REG_YMM13:
    case UC_X86_REG_YMM14:
    case UC_X86_REG_YMM15: {
        CHECK_REG_TYPE(float64[4]);
        float64 *dst = (float64 *)value;
        ZMMReg *lo_reg = (ZMMReg *)&env->xmm_regs[regid - UC_X86_REG_YMM0];
        XMMReg *hi_reg = &env->ymmh_regs[regid - UC_X86_REG_YMM0];
        dst[0] = lo_reg->ZMM_Q(0);
        dst[1] = lo_reg->ZMM_Q(1);
        dst[2] = hi_reg->_d[0];
        dst[3] = hi_reg->_d[1];
        return ret;
    }

    case UC_X86_REG_FIP:
        CHECK_REG_TYPE(uint64_t);
        *(uint64_t *)value = env->fpip;
        return ret;
    case UC_X86_REG_FCS:
        CHECK_REG_TYPE(uint16_t);
        *(uint16_t *)value = env->fpcs;
        return ret;
    case UC_X86_REG_FDP:
        CHECK_REG_TYPE(uint64_t);
        *(uint64_t *)value = env->fpdp;
        return ret;
    case UC_X86_REG_FDS:
        CHECK_REG_TYPE(uint16_t);
        *(uint16_t *)value = env->fpds;
        return ret;
    case UC_X86_REG_FOP:
        CHECK_REG_TYPE(uint16_t);
        *(uint16_t *)value = env->fpop;
        return ret;
    }

    switch (mode) {
    default:
        break;
    case UC_MODE_16:
        switch (regid) {
        default:
            break;
        case UC_X86_REG_ES:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = env->segs[R_ES].selector;
            return ret;
        case UC_X86_REG_SS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = env->segs[R_SS].selector;
            return ret;
        case UC_X86_REG_DS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = env->segs[R_DS].selector;
            return ret;
        case UC_X86_REG_FS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = env->segs[R_FS].selector;
            return ret;
        case UC_X86_REG_GS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = env->segs[R_GS].selector;
            return ret;
        case UC_X86_REG_FS_BASE:
            CHECK_REG_TYPE(uint32_t);
            *(uint32_t *)value = (uint32_t)env->segs[R_FS].base;
            return ret;
        }
        // fall-thru
    case UC_MODE_32:
        switch (regid) {
        default:
            break;
        case UC_X86_REG_CR0:
        case UC_X86_REG_CR1:
        case UC_X86_REG_CR2:
        case UC_X86_REG_CR3:
        case UC_X86_REG_CR4:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->cr[regid - UC_X86_REG_CR0];
            break;
        case UC_X86_REG_DR0:
        case UC_X86_REG_DR1:
        case UC_X86_REG_DR2:
        case UC_X86_REG_DR3:
        case UC_X86_REG_DR4:
        case UC_X86_REG_DR5:
        case UC_X86_REG_DR6:
        case UC_X86_REG_DR7:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->dr[regid - UC_X86_REG_DR0];
            break;
        case UC_X86_REG_FLAGS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = cpu_compute_eflags(env);
            break;
        case UC_X86_REG_EFLAGS:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = cpu_compute_eflags(env);
            break;
        case UC_X86_REG_EAX:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->regs[R_EAX];
            break;
        case UC_X86_REG_AX:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_EAX]);
            break;
        case UC_X86_REG_AH:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_H(env->regs[R_EAX]);
            break;
        case UC_X86_REG_AL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_EAX]);
            break;
        case UC_X86_REG_EBX:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->regs[R_EBX];
            break;
        case UC_X86_REG_BX:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_EBX]);
            break;
        case UC_X86_REG_BH:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_H(env->regs[R_EBX]);
            break;
        case UC_X86_REG_BL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_EBX]);
            break;
        case UC_X86_REG_ECX:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->regs[R_ECX];
            break;
        case UC_X86_REG_CX:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_ECX]);
            break;
        case UC_X86_REG_CH:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_H(env->regs[R_ECX]);
            break;
        case UC_X86_REG_CL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_ECX]);
            break;
        case UC_X86_REG_EDX:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->regs[R_EDX];
            break;
        case UC_X86_REG_DX:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_EDX]);
            break;
        case UC_X86_REG_DH:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_H(env->regs[R_EDX]);
            break;
        case UC_X86_REG_DL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_EDX]);
            break;
        case UC_X86_REG_ESP:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->regs[R_ESP];
            break;
        case UC_X86_REG_SP:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_ESP]);
            break;
        case UC_X86_REG_EBP:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->regs[R_EBP];
            break;
        case UC_X86_REG_BP:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_EBP]);
            break;
        case UC_X86_REG_ESI:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->regs[R_ESI];
            break;
        case UC_X86_REG_SI:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_ESI]);
            break;
        case UC_X86_REG_EDI:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->regs[R_EDI];
            break;
        case UC_X86_REG_DI:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_EDI]);
            break;
        case UC_X86_REG_EIP:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = env->eip;
            break;
        case UC_X86_REG_IP:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->eip);
            break;
        case UC_X86_REG_CS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_CS].selector;
            break;
        case UC_X86_REG_DS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_DS].selector;
            break;
        case UC_X86_REG_SS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_SS].selector;
            break;
        case UC_X86_REG_ES:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_ES].selector;
            break;
        case UC_X86_REG_FS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_FS].selector;
            break;
        case UC_X86_REG_GS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_GS].selector;
            break;
        case UC_X86_REG_IDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            ((uc_x86_mmr *)value)->limit = (uint16_t)env->idt.limit;
            ((uc_x86_mmr *)value)->base = (uint32_t)env->idt.base;
            break;
        case UC_X86_REG_GDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            ((uc_x86_mmr *)value)->limit = (uint16_t)env->gdt.limit;
            ((uc_x86_mmr *)value)->base = (uint32_t)env->gdt.base;
            break;
        case UC_X86_REG_LDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            ((uc_x86_mmr *)value)->limit = env->ldt.limit;
            ((uc_x86_mmr *)value)->base = (uint32_t)env->ldt.base;
            ((uc_x86_mmr *)value)->selector = (uint16_t)env->ldt.selector;
            ((uc_x86_mmr *)value)->flags = env->ldt.flags;
            break;
        case UC_X86_REG_TR:
            CHECK_REG_TYPE(uc_x86_mmr);
            ((uc_x86_mmr *)value)->limit = env->tr.limit;
            ((uc_x86_mmr *)value)->base = (uint32_t)env->tr.base;
            ((uc_x86_mmr *)value)->selector = (uint16_t)env->tr.selector;
            ((uc_x86_mmr *)value)->flags = env->tr.flags;
            break;
        case UC_X86_REG_MSR:
            CHECK_REG_TYPE(uc_x86_msr);
            x86_msr_read(env, (uc_x86_msr *)value);
            break;
        case UC_X86_REG_MXCSR:
            CHECK_REG_TYPE(uint32_t);
            *(uint32_t *)value = env->mxcsr;
            break;
        case UC_X86_REG_FS_BASE:
            CHECK_REG_TYPE(uint32_t);
            *(uint32_t *)value = (uint32_t)env->segs[R_FS].base;
            break;
        }
        break;

#ifdef TARGET_X86_64
    case UC_MODE_64:
        switch (regid) {
        default:
            break;
        case UC_X86_REG_CR0:
        case UC_X86_REG_CR1:
        case UC_X86_REG_CR2:
        case UC_X86_REG_CR3:
        case UC_X86_REG_CR4:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = env->cr[regid - UC_X86_REG_CR0];
            break;
        case UC_X86_REG_DR0:
        case UC_X86_REG_DR1:
        case UC_X86_REG_DR2:
        case UC_X86_REG_DR3:
        case UC_X86_REG_DR4:
        case UC_X86_REG_DR5:
        case UC_X86_REG_DR6:
        case UC_X86_REG_DR7:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = env->dr[regid - UC_X86_REG_DR0];
            break;
        case UC_X86_REG_FLAGS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = cpu_compute_eflags(env);
            break;
        case UC_X86_REG_EFLAGS:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = cpu_compute_eflags(env);
            break;
        case UC_X86_REG_RFLAGS:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = cpu_compute_eflags(env);
            break;
        case UC_X86_REG_RAX:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = env->regs[R_EAX];
            break;
        case UC_X86_REG_EAX:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[R_EAX]);
            break;
        case UC_X86_REG_AX:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_EAX]);
            break;
        case UC_X86_REG_AH:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_H(env->regs[R_EAX]);
            break;
        case UC_X86_REG_AL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_EAX]);
            break;
        case UC_X86_REG_RBX:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = env->regs[R_EBX];
            break;
        case UC_X86_REG_EBX:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[R_EBX]);
            break;
        case UC_X86_REG_BX:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_EBX]);
            break;
        case UC_X86_REG_BH:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_H(env->regs[R_EBX]);
            break;
        case UC_X86_REG_BL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_EBX]);
            break;
        case UC_X86_REG_RCX:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = env->regs[R_ECX];
            break;
        case UC_X86_REG_ECX:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[R_ECX]);
            break;
        case UC_X86_REG_CX:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_ECX]);
            break;
        case UC_X86_REG_CH:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_H(env->regs[R_ECX]);
            break;
        case UC_X86_REG_CL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_ECX]);
            break;
        case UC_X86_REG_RDX:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = env->regs[R_EDX];
            break;
        case UC_X86_REG_EDX:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[R_EDX]);
            break;
        case UC_X86_REG_DX:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_EDX]);
            break;
        case UC_X86_REG_DH:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_H(env->regs[R_EDX]);
            break;
        case UC_X86_REG_DL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_EDX]);
            break;
        case UC_X86_REG_RSP:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = env->regs[R_ESP];
            break;
        case UC_X86_REG_ESP:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[R_ESP]);
            break;
        case UC_X86_REG_SP:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_ESP]);
            break;
        case UC_X86_REG_SPL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_ESP]);
            break;
        case UC_X86_REG_RBP:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = env->regs[R_EBP];
            break;
        case UC_X86_REG_EBP:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[R_EBP]);
            break;
        case UC_X86_REG_BP:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_EBP]);
            break;
        case UC_X86_REG_BPL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_EBP]);
            break;
        case UC_X86_REG_RSI:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = env->regs[R_ESI];
            break;
        case UC_X86_REG_ESI:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[R_ESI]);
            break;
        case UC_X86_REG_SI:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_ESI]);
            break;
        case UC_X86_REG_SIL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_ESI]);
            break;
        case UC_X86_REG_RDI:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = env->regs[R_EDI];
            break;
        case UC_X86_REG_EDI:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[R_EDI]);
            break;
        case UC_X86_REG_DI:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[R_EDI]);
            break;
        case UC_X86_REG_DIL:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[R_EDI]);
            break;
        case UC_X86_REG_RIP:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = env->eip;
            break;
        case UC_X86_REG_EIP:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->eip);
            break;
        case UC_X86_REG_IP:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->eip);
            break;
        case UC_X86_REG_CS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_CS].selector;
            break;
        case UC_X86_REG_DS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_DS].selector;
            break;
        case UC_X86_REG_SS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_SS].selector;
            break;
        case UC_X86_REG_ES:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_ES].selector;
            break;
        case UC_X86_REG_FS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_FS].selector;
            break;
        case UC_X86_REG_GS:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = (uint16_t)env->segs[R_GS].selector;
            break;
        case UC_X86_REG_R8:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = READ_QWORD(env->regs[8]);
            break;
        case UC_X86_REG_R8D:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[8]);
            break;
        case UC_X86_REG_R8W:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[8]);
            break;
        case UC_X86_REG_R8B:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[8]);
            break;
        case UC_X86_REG_R9:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = READ_QWORD(env->regs[9]);
            break;
        case UC_X86_REG_R9D:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[9]);
            break;
        case UC_X86_REG_R9W:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[9]);
            break;
        case UC_X86_REG_R9B:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[9]);
            break;
        case UC_X86_REG_R10:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = READ_QWORD(env->regs[10]);
            break;
        case UC_X86_REG_R10D:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[10]);
            break;
        case UC_X86_REG_R10W:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[10]);
            break;
        case UC_X86_REG_R10B:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[10]);
            break;
        case UC_X86_REG_R11:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = READ_QWORD(env->regs[11]);
            break;
        case UC_X86_REG_R11D:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[11]);
            break;
        case UC_X86_REG_R11W:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[11]);
            break;
        case UC_X86_REG_R11B:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[11]);
            break;
        case UC_X86_REG_R12:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = READ_QWORD(env->regs[12]);
            break;
        case UC_X86_REG_R12D:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[12]);
            break;
        case UC_X86_REG_R12W:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[12]);
            break;
        case UC_X86_REG_R12B:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[12]);
            break;
        case UC_X86_REG_R13:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = READ_QWORD(env->regs[13]);
            break;
        case UC_X86_REG_R13D:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[13]);
            break;
        case UC_X86_REG_R13W:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[13]);
            break;
        case UC_X86_REG_R13B:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[13]);
            break;
        case UC_X86_REG_R14:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = READ_QWORD(env->regs[14]);
            break;
        case UC_X86_REG_R14D:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[14]);
            break;
        case UC_X86_REG_R14W:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[14]);
            break;
        case UC_X86_REG_R14B:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[14]);
            break;
        case UC_X86_REG_R15:
            CHECK_REG_TYPE(int64_t);
            *(int64_t *)value = READ_QWORD(env->regs[15]);
            break;
        case UC_X86_REG_R15D:
            CHECK_REG_TYPE(int32_t);
            *(int32_t *)value = READ_DWORD(env->regs[15]);
            break;
        case UC_X86_REG_R15W:
            CHECK_REG_TYPE(int16_t);
            *(int16_t *)value = READ_WORD(env->regs[15]);
            break;
        case UC_X86_REG_R15B:
            CHECK_REG_TYPE(int8_t);
            *(int8_t *)value = READ_BYTE_L(env->regs[15]);
            break;
        case UC_X86_REG_IDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            ((uc_x86_mmr *)value)->limit = (uint16_t)env->idt.limit;
            ((uc_x86_mmr *)value)->base = env->idt.base;
            break;
        case UC_X86_REG_GDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            ((uc_x86_mmr *)value)->limit = (uint16_t)env->gdt.limit;
            ((uc_x86_mmr *)value)->base = env->gdt.base;
            break;
        case UC_X86_REG_LDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            ((uc_x86_mmr *)value)->limit = env->ldt.limit;
            ((uc_x86_mmr *)value)->base = env->ldt.base;
            ((uc_x86_mmr *)value)->selector = (uint16_t)env->ldt.selector;
            ((uc_x86_mmr *)value)->flags = env->ldt.flags;
            break;
        case UC_X86_REG_TR:
            CHECK_REG_TYPE(uc_x86_mmr);
            ((uc_x86_mmr *)value)->limit = env->tr.limit;
            ((uc_x86_mmr *)value)->base = env->tr.base;
            ((uc_x86_mmr *)value)->selector = (uint16_t)env->tr.selector;
            ((uc_x86_mmr *)value)->flags = env->tr.flags;
            break;
        case UC_X86_REG_MSR:
            CHECK_REG_TYPE(uc_x86_msr);
            x86_msr_read(env, (uc_x86_msr *)value);
            break;
        case UC_X86_REG_MXCSR:
            CHECK_REG_TYPE(uint32_t);
            *(uint32_t *)value = env->mxcsr;
            break;
        case UC_X86_REG_XMM8:
        case UC_X86_REG_XMM9:
        case UC_X86_REG_XMM10:
        case UC_X86_REG_XMM11:
        case UC_X86_REG_XMM12:
        case UC_X86_REG_XMM13:
        case UC_X86_REG_XMM14:
        case UC_X86_REG_XMM15: {
            CHECK_REG_TYPE(float64[2]);
            float64 *dst = (float64 *)value;
            ZMMReg *reg = (ZMMReg *)&env->xmm_regs[regid - UC_X86_REG_XMM0];
            dst[0] = reg->ZMM_Q(0);
            dst[1] = reg->ZMM_Q(1);
            break;
        }
        case UC_X86_REG_FS_BASE:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = (uint64_t)env->segs[R_FS].base;
            break;
        case UC_X86_REG_GS_BASE:
            CHECK_REG_TYPE(uint64_t);
            *(uint64_t *)value = (uint64_t)env->segs[R_GS].base;
            break;
        }
        break;
#endif
    }

    return ret;
}

static uc_err reg_write(CPUX86State *env, unsigned int regid, const void *value,
                        size_t *size, uc_mode mode, int *setpc)
{
    uc_err ret = UC_ERR_ARG;

    switch (regid) {
    default:
        break;
    case UC_X86_REG_FP0:
    case UC_X86_REG_FP1:
    case UC_X86_REG_FP2:
    case UC_X86_REG_FP3:
    case UC_X86_REG_FP4:
    case UC_X86_REG_FP5:
    case UC_X86_REG_FP6:
    case UC_X86_REG_FP7: {
        CHECK_REG_TYPE(char[10]);
        uint64_t mant = *(uint64_t *)value;
        uint16_t upper = *(uint16_t *)((char *)value + sizeof(uint64_t));
        env->fpregs[regid - UC_X86_REG_FP0].d = cpu_set_fp80(mant, upper);
        return ret;
    }
    case UC_X86_REG_FPSW: {
        CHECK_REG_TYPE(uint16_t);
        uint16_t fpus = *(uint16_t *)value;
        env->fpus = fpus & ~0x3800;
        env->fpstt = (fpus >> 11) & 0x7;
        return ret;
    }
    case UC_X86_REG_FPCW:
        CHECK_REG_TYPE(uint16_t);
        cpu_set_fpuc(env, *(uint16_t *)value);
        return ret;
    case UC_X86_REG_FPTAG: {
        CHECK_REG_TYPE(uint16_t);
        int i;
        uint16_t fptag = *(uint16_t *)value;
        for (i = 0; i < 8; i++) {
            env->fptags[i] = ((fptag & 3) == 3);
            fptag >>= 2;
        }

        return ret;
    }
    case UC_X86_REG_XMM0:
    case UC_X86_REG_XMM1:
    case UC_X86_REG_XMM2:
    case UC_X86_REG_XMM3:
    case UC_X86_REG_XMM4:
    case UC_X86_REG_XMM5:
    case UC_X86_REG_XMM6:
    case UC_X86_REG_XMM7: {
        CHECK_REG_TYPE(float64[2]);
        float64 *src = (float64 *)value;
        ZMMReg *reg = (ZMMReg *)&env->xmm_regs[regid - UC_X86_REG_XMM0];
        reg->ZMM_Q(0) = src[0];
        reg->ZMM_Q(1) = src[1];
        return ret;
    }
    case UC_X86_REG_ST0:
    case UC_X86_REG_ST1:
    case UC_X86_REG_ST2:
    case UC_X86_REG_ST3:
    case UC_X86_REG_ST4:
    case UC_X86_REG_ST5:
    case UC_X86_REG_ST6:
    case UC_X86_REG_ST7: {
        CHECK_REG_TYPE(char[10]);
        memcpy(&FPST(regid - UC_X86_REG_ST0), value, 10);
        return ret;
    }
    case UC_X86_REG_YMM0:
    case UC_X86_REG_YMM1:
    case UC_X86_REG_YMM2:
    case UC_X86_REG_YMM3:
    case UC_X86_REG_YMM4:
    case UC_X86_REG_YMM5:
    case UC_X86_REG_YMM6:
    case UC_X86_REG_YMM7:
    case UC_X86_REG_YMM8:
    case UC_X86_REG_YMM9:
    case UC_X86_REG_YMM10:
    case UC_X86_REG_YMM11:
    case UC_X86_REG_YMM12:
    case UC_X86_REG_YMM13:
    case UC_X86_REG_YMM14:
    case UC_X86_REG_YMM15: {
        CHECK_REG_TYPE(float64[4]);
        float64 *src = (float64 *)value;
        ZMMReg *lo_reg = (ZMMReg *)&env->xmm_regs[regid - UC_X86_REG_YMM0];
        XMMReg *hi_reg = &env->ymmh_regs[regid - UC_X86_REG_YMM0];
        lo_reg->ZMM_Q(0) = src[0];
        lo_reg->ZMM_Q(1) = src[1];
        // YMM is not supported by QEMU at all
        // As of qemu 5.0.1, ymmh_regs is nowhere used.
        hi_reg->_d[0] = src[2];
        hi_reg->_d[1] = src[3];
        return ret;
    }

    case UC_X86_REG_FIP:
        CHECK_REG_TYPE(uint64_t);
        env->fpip = *(uint64_t *)value;
        return ret;
    case UC_X86_REG_FCS:
        CHECK_REG_TYPE(uint16_t);
        env->fpcs = *(uint16_t *)value;
        return ret;
    case UC_X86_REG_FDP:
        CHECK_REG_TYPE(uint64_t);
        env->fpdp = *(uint64_t *)value;
        return ret;
    case UC_X86_REG_FDS:
        CHECK_REG_TYPE(uint16_t);
        env->fpds = *(uint16_t *)value;
        return ret;
    case UC_X86_REG_FOP:
        CHECK_REG_TYPE(uint16_t);
        env->fpop = *(uint16_t *)value;
        return ret;
    }

    switch (mode) {
    default:
        break;

    case UC_MODE_16:
        switch (regid) {
        default:
            break;
        case UC_X86_REG_ES:
            CHECK_REG_TYPE(uint16_t);
            load_seg_16_helper(env, R_ES, *(uint16_t *)value);
            return ret;
        case UC_X86_REG_SS:
            CHECK_REG_TYPE(uint16_t);
            load_seg_16_helper(env, R_SS, *(uint16_t *)value);
            return ret;
        case UC_X86_REG_DS:
            CHECK_REG_TYPE(uint16_t);
            load_seg_16_helper(env, R_DS, *(uint16_t *)value);
            return ret;
        case UC_X86_REG_FS:
            CHECK_REG_TYPE(uint16_t);
            load_seg_16_helper(env, R_FS, *(uint16_t *)value);
            return ret;
        case UC_X86_REG_GS:
            CHECK_REG_TYPE(uint16_t);
            load_seg_16_helper(env, R_GS, *(uint16_t *)value);
            return ret;
        }
        // fall-thru
    case UC_MODE_32:
        switch (regid) {
        default:
            break;
        case UC_X86_REG_CR0:
            CHECK_REG_TYPE(uint32_t);
            cpu_x86_update_cr0(env, *(uint32_t *)value);
            goto write_cr;
        case UC_X86_REG_CR1:
        case UC_X86_REG_CR2:
        case UC_X86_REG_CR3:
            CHECK_REG_TYPE(uint32_t);
            cpu_x86_update_cr3(env, *(uint32_t *)value);
            goto write_cr;
        case UC_X86_REG_CR4:
            CHECK_REG_TYPE(uint32_t);
            cpu_x86_update_cr4(env, *(uint32_t *)value);
        write_cr:
            env->cr[regid - UC_X86_REG_CR0] = *(uint32_t *)value;
            break;
        case UC_X86_REG_DR0:
        case UC_X86_REG_DR1:
        case UC_X86_REG_DR2:
        case UC_X86_REG_DR3:
        case UC_X86_REG_DR4:
        case UC_X86_REG_DR5:
        case UC_X86_REG_DR6:
        case UC_X86_REG_DR7:
            CHECK_REG_TYPE(uint32_t);
            env->dr[regid - UC_X86_REG_DR0] = *(uint32_t *)value;
            break;
        case UC_X86_REG_FLAGS:
            CHECK_REG_TYPE(uint16_t);
            cpu_load_eflags(env, *(uint16_t *)value, -1);
            break;
        case UC_X86_REG_EFLAGS:
            CHECK_REG_TYPE(uint32_t);
            cpu_load_eflags(env, *(uint32_t *)value, -1);
            break;
        case UC_X86_REG_EAX:
            CHECK_REG_TYPE(uint32_t);
            env->regs[R_EAX] = *(uint32_t *)value;
            break;
        case UC_X86_REG_AX:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_EAX], *(uint16_t *)value);
            break;
        case UC_X86_REG_AH:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_H(env->regs[R_EAX], *(uint8_t *)value);
            break;
        case UC_X86_REG_AL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_EAX], *(uint8_t *)value);
            break;
        case UC_X86_REG_EBX:
            CHECK_REG_TYPE(uint32_t);
            env->regs[R_EBX] = *(uint32_t *)value;
            break;
        case UC_X86_REG_BX:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_EBX], *(uint16_t *)value);
            break;
        case UC_X86_REG_BH:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_H(env->regs[R_EBX], *(uint8_t *)value);
            break;
        case UC_X86_REG_BL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_EBX], *(uint8_t *)value);
            break;
        case UC_X86_REG_ECX:
            CHECK_REG_TYPE(uint32_t);
            env->regs[R_ECX] = *(uint32_t *)value;
            break;
        case UC_X86_REG_CX:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_ECX], *(uint16_t *)value);
            break;
        case UC_X86_REG_CH:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_H(env->regs[R_ECX], *(uint8_t *)value);
            break;
        case UC_X86_REG_CL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_ECX], *(uint8_t *)value);
            break;
        case UC_X86_REG_EDX:
            CHECK_REG_TYPE(uint32_t);
            env->regs[R_EDX] = *(uint32_t *)value;
            break;
        case UC_X86_REG_DX:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_EDX], *(uint16_t *)value);
            break;
        case UC_X86_REG_DH:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_H(env->regs[R_EDX], *(uint8_t *)value);
            break;
        case UC_X86_REG_DL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_EDX], *(uint8_t *)value);
            break;
        case UC_X86_REG_ESP:
            CHECK_REG_TYPE(uint32_t);
            env->regs[R_ESP] = *(uint32_t *)value;
            break;
        case UC_X86_REG_SP:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_ESP], *(uint16_t *)value);
            break;
        case UC_X86_REG_EBP:
            CHECK_REG_TYPE(uint32_t);
            env->regs[R_EBP] = *(uint32_t *)value;
            break;
        case UC_X86_REG_BP:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_EBP], *(uint16_t *)value);
            break;
        case UC_X86_REG_ESI:
            CHECK_REG_TYPE(uint32_t);
            env->regs[R_ESI] = *(uint32_t *)value;
            break;
        case UC_X86_REG_SI:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_ESI], *(uint16_t *)value);
            break;
        case UC_X86_REG_EDI:
            CHECK_REG_TYPE(uint32_t);
            env->regs[R_EDI] = *(uint32_t *)value;
            break;
        case UC_X86_REG_DI:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_EDI], *(uint16_t *)value);
            break;
        case UC_X86_REG_EIP:
            CHECK_REG_TYPE(uint32_t);
            env->eip = *(uint32_t *)value;
            *setpc = 1;
            break;
        case UC_X86_REG_IP:
            CHECK_REG_TYPE(uint16_t);
            env->eip = *(uint16_t *)value;
            *setpc = 1;
            break;
        case UC_X86_REG_CS:
            CHECK_REG_TYPE(uint16_t);
            ret = uc_check_cpu_x86_load_seg(env, R_CS, *(uint16_t *)value);
            if (ret) {
                return ret;
            }
            cpu_x86_load_seg(env, R_CS, *(uint16_t *)value);
            break;
        case UC_X86_REG_DS:
            CHECK_REG_TYPE(uint16_t);
            ret = uc_check_cpu_x86_load_seg(env, R_DS, *(uint16_t *)value);
            if (ret) {
                return ret;
            }
            cpu_x86_load_seg(env, R_DS, *(uint16_t *)value);
            break;
        case UC_X86_REG_SS:
            CHECK_REG_TYPE(uint16_t);
            ret = uc_check_cpu_x86_load_seg(env, R_SS, *(uint16_t *)value);
            if (ret) {
                return ret;
            }
            cpu_x86_load_seg(env, R_SS, *(uint16_t *)value);
            break;
        case UC_X86_REG_ES:
            CHECK_REG_TYPE(uint16_t);
            ret = uc_check_cpu_x86_load_seg(env, R_ES, *(uint16_t *)value);
            if (ret) {
                return ret;
            }
            cpu_x86_load_seg(env, R_ES, *(uint16_t *)value);
            break;
        case UC_X86_REG_FS:
            CHECK_REG_TYPE(uint16_t);
            ret = uc_check_cpu_x86_load_seg(env, R_FS, *(uint16_t *)value);
            if (ret) {
                return ret;
            }
            cpu_x86_load_seg(env, R_FS, *(uint16_t *)value);
            break;
        case UC_X86_REG_GS:
            CHECK_REG_TYPE(uint16_t);
            ret = uc_check_cpu_x86_load_seg(env, R_GS, *(uint16_t *)value);
            if (ret) {
                return ret;
            }
            cpu_x86_load_seg(env, R_GS, *(uint16_t *)value);
            break;
        case UC_X86_REG_IDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            env->idt.limit = (uint16_t)((uc_x86_mmr *)value)->limit;
            env->idt.base = (uint32_t)((uc_x86_mmr *)value)->base;
            break;
        case UC_X86_REG_GDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            env->gdt.limit = (uint16_t)((uc_x86_mmr *)value)->limit;
            env->gdt.base = (uint32_t)((uc_x86_mmr *)value)->base;
            break;
        case UC_X86_REG_LDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            env->ldt.limit = ((uc_x86_mmr *)value)->limit;
            env->ldt.base = (uint32_t)((uc_x86_mmr *)value)->base;
            env->ldt.selector = (uint16_t)((uc_x86_mmr *)value)->selector;
            env->ldt.flags = ((uc_x86_mmr *)value)->flags;
            break;
        case UC_X86_REG_TR:
            CHECK_REG_TYPE(uc_x86_mmr);
            env->tr.limit = ((uc_x86_mmr *)value)->limit;
            env->tr.base = (uint32_t)((uc_x86_mmr *)value)->base;
            env->tr.selector = (uint16_t)((uc_x86_mmr *)value)->selector;
            env->tr.flags = ((uc_x86_mmr *)value)->flags;
            break;
        case UC_X86_REG_MSR:
            CHECK_REG_TYPE(uc_x86_msr);
            x86_msr_write(env, (uc_x86_msr *)value);
            break;
        case UC_X86_REG_MXCSR:
            CHECK_REG_TYPE(uint32_t);
            cpu_set_mxcsr(env, *(uint32_t *)value);
            break;
            /*
        // Don't think base registers are a "thing" on x86
        case UC_X86_REG_FS_BASE:
            CHECK_REG_TYPE(uint32_t);
            env->segs[R_FS].base = *(uint32_t *)value;
            continue;
        case UC_X86_REG_GS_BASE:
            CHECK_REG_TYPE(uint32_t);
            env->segs[R_GS].base = *(uint32_t *)value;
            continue;
            */
        }
        break;

#ifdef TARGET_X86_64
    case UC_MODE_64:
        switch (regid) {
        default:
            break;
        case UC_X86_REG_CR0:
            CHECK_REG_TYPE(uint64_t);
            cpu_x86_update_cr0(env, *(uint32_t *)value);
            goto write_cr64;
        case UC_X86_REG_CR1:
        case UC_X86_REG_CR2:
        case UC_X86_REG_CR3:
            CHECK_REG_TYPE(uint64_t);
            cpu_x86_update_cr3(env, *(uint32_t *)value);
            goto write_cr64;
        case UC_X86_REG_CR4:
            CHECK_REG_TYPE(uint64_t);
            cpu_x86_update_cr4(env, *(uint32_t *)value);
        write_cr64:
            env->cr[regid - UC_X86_REG_CR0] = *(uint64_t *)value;
            break;
        case UC_X86_REG_DR0:
        case UC_X86_REG_DR1:
        case UC_X86_REG_DR2:
        case UC_X86_REG_DR3:
        case UC_X86_REG_DR4:
        case UC_X86_REG_DR5:
        case UC_X86_REG_DR6:
        case UC_X86_REG_DR7:
            CHECK_REG_TYPE(uint64_t);
            env->dr[regid - UC_X86_REG_DR0] = *(uint64_t *)value;
            break;
        case UC_X86_REG_FLAGS:
            CHECK_REG_TYPE(uint16_t);
            cpu_load_eflags(env, *(uint16_t *)value, -1);
            break;
        case UC_X86_REG_EFLAGS:
            CHECK_REG_TYPE(uint32_t);
            cpu_load_eflags(env, *(uint32_t *)value, -1);
            break;
        case UC_X86_REG_RFLAGS:
            CHECK_REG_TYPE(uint64_t);
            cpu_load_eflags(env, *(uint64_t *)value, -1);
            break;
        case UC_X86_REG_RAX:
            CHECK_REG_TYPE(uint64_t);
            env->regs[R_EAX] = *(uint64_t *)value;
            break;
        case UC_X86_REG_EAX:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[R_EAX], *(uint32_t *)value);
            break;
        case UC_X86_REG_AX:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_EAX], *(uint16_t *)value);
            break;
        case UC_X86_REG_AH:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_H(env->regs[R_EAX], *(uint8_t *)value);
            break;
        case UC_X86_REG_AL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_EAX], *(uint8_t *)value);
            break;
        case UC_X86_REG_RBX:
            CHECK_REG_TYPE(uint64_t);
            env->regs[R_EBX] = *(uint64_t *)value;
            break;
        case UC_X86_REG_EBX:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[R_EBX], *(uint32_t *)value);
            break;
        case UC_X86_REG_BX:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_EBX], *(uint16_t *)value);
            break;
        case UC_X86_REG_BH:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_H(env->regs[R_EBX], *(uint8_t *)value);
            break;
        case UC_X86_REG_BL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_EBX], *(uint8_t *)value);
            break;
        case UC_X86_REG_RCX:
            CHECK_REG_TYPE(uint64_t);
            env->regs[R_ECX] = *(uint64_t *)value;
            break;
        case UC_X86_REG_ECX:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[R_ECX], *(uint32_t *)value);
            break;
        case UC_X86_REG_CX:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_ECX], *(uint16_t *)value);
            break;
        case UC_X86_REG_CH:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_H(env->regs[R_ECX], *(uint8_t *)value);
            break;
        case UC_X86_REG_CL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_ECX], *(uint8_t *)value);
            break;
        case UC_X86_REG_RDX:
            CHECK_REG_TYPE(uint64_t);
            env->regs[R_EDX] = *(uint64_t *)value;
            break;
        case UC_X86_REG_EDX:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[R_EDX], *(uint32_t *)value);
            break;
        case UC_X86_REG_DX:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_EDX], *(uint16_t *)value);
            break;
        case UC_X86_REG_DH:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_H(env->regs[R_EDX], *(uint8_t *)value);
            break;
        case UC_X86_REG_DL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_EDX], *(uint8_t *)value);
            break;
        case UC_X86_REG_RSP:
            CHECK_REG_TYPE(uint64_t);
            env->regs[R_ESP] = *(uint64_t *)value;
            break;
        case UC_X86_REG_ESP:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[R_ESP], *(uint32_t *)value);
            break;
        case UC_X86_REG_SP:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_ESP], *(uint16_t *)value);
            break;
        case UC_X86_REG_SPL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_ESP], *(uint8_t *)value);
            break;
        case UC_X86_REG_RBP:
            CHECK_REG_TYPE(uint64_t);
            env->regs[R_EBP] = *(uint64_t *)value;
            break;
        case UC_X86_REG_EBP:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[R_EBP], *(uint32_t *)value);
            break;
        case UC_X86_REG_BP:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_EBP], *(uint16_t *)value);
            break;
        case UC_X86_REG_BPL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_EBP], *(uint8_t *)value);
            break;
        case UC_X86_REG_RSI:
            CHECK_REG_TYPE(uint64_t);
            env->regs[R_ESI] = *(uint64_t *)value;
            break;
        case UC_X86_REG_ESI:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[R_ESI], *(uint32_t *)value);
            break;
        case UC_X86_REG_SI:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_ESI], *(uint16_t *)value);
            break;
        case UC_X86_REG_SIL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_ESI], *(uint8_t *)value);
            break;
        case UC_X86_REG_RDI:
            CHECK_REG_TYPE(uint64_t);
            env->regs[R_EDI] = *(uint64_t *)value;
            break;
        case UC_X86_REG_EDI:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[R_EDI], *(uint32_t *)value);
            break;
        case UC_X86_REG_DI:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[R_EDI], *(uint16_t *)value);
            break;
        case UC_X86_REG_DIL:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[R_EDI], *(uint8_t *)value);
            break;
        case UC_X86_REG_RIP:
            CHECK_REG_TYPE(uint64_t);
            env->eip = *(uint64_t *)value;
            *setpc = 1;
            break;
        case UC_X86_REG_EIP:
            CHECK_REG_TYPE(uint32_t);
            env->eip = *(uint32_t *)value;
            *setpc = 1;
            break;
        case UC_X86_REG_IP:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->eip, *(uint16_t *)value);
            *setpc = 1;
            break;
        case UC_X86_REG_CS:
            CHECK_REG_TYPE(uint16_t);
            env->segs[R_CS].selector = *(uint16_t *)value;
            break;
        case UC_X86_REG_DS:
            CHECK_REG_TYPE(uint16_t);
            env->segs[R_DS].selector = *(uint16_t *)value;
            break;
        case UC_X86_REG_SS:
            CHECK_REG_TYPE(uint16_t);
            env->segs[R_SS].selector = *(uint16_t *)value;
            break;
        case UC_X86_REG_ES:
            CHECK_REG_TYPE(uint16_t);
            env->segs[R_ES].selector = *(uint16_t *)value;
            break;
        case UC_X86_REG_FS:
            CHECK_REG_TYPE(uint16_t);
            ret = uc_check_cpu_x86_load_seg(env, R_FS, *(uint16_t *)value);
            if (ret) {
                return ret;
            }
            cpu_x86_load_seg(env, R_FS, *(uint16_t *)value);
            break;
        case UC_X86_REG_GS:
            CHECK_REG_TYPE(uint16_t);
            ret = uc_check_cpu_x86_load_seg(env, R_GS, *(uint16_t *)value);
            if (ret) {
                return ret;
            }
            cpu_x86_load_seg(env, R_GS, *(uint16_t *)value);
            break;
        case UC_X86_REG_R8:
            CHECK_REG_TYPE(uint64_t);
            env->regs[8] = *(uint64_t *)value;
            break;
        case UC_X86_REG_R8D:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[8], *(uint32_t *)value);
            break;
        case UC_X86_REG_R8W:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[8], *(uint16_t *)value);
            break;
        case UC_X86_REG_R8B:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[8], *(uint8_t *)value);
            break;
        case UC_X86_REG_R9:
            CHECK_REG_TYPE(uint64_t);
            env->regs[9] = *(uint64_t *)value;
            break;
        case UC_X86_REG_R9D:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[9], *(uint32_t *)value);
            break;
        case UC_X86_REG_R9W:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[9], *(uint16_t *)value);
            break;
        case UC_X86_REG_R9B:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[9], *(uint8_t *)value);
            break;
        case UC_X86_REG_R10:
            CHECK_REG_TYPE(uint64_t);
            env->regs[10] = *(uint64_t *)value;
            break;
        case UC_X86_REG_R10D:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[10], *(uint32_t *)value);
            break;
        case UC_X86_REG_R10W:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[10], *(uint16_t *)value);
            break;
        case UC_X86_REG_R10B:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[10], *(uint8_t *)value);
            break;
        case UC_X86_REG_R11:
            CHECK_REG_TYPE(uint64_t);
            env->regs[11] = *(uint64_t *)value;
            break;
        case UC_X86_REG_R11D:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[11], *(uint32_t *)value);
            break;
        case UC_X86_REG_R11W:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[11], *(uint16_t *)value);
            break;
        case UC_X86_REG_R11B:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[11], *(uint8_t *)value);
            break;
        case UC_X86_REG_R12:
            CHECK_REG_TYPE(uint64_t);
            env->regs[12] = *(uint64_t *)value;
            break;
        case UC_X86_REG_R12D:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[12], *(uint32_t *)value);
            break;
        case UC_X86_REG_R12W:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[12], *(uint16_t *)value);
            break;
        case UC_X86_REG_R12B:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[12], *(uint8_t *)value);
            break;
        case UC_X86_REG_R13:
            CHECK_REG_TYPE(uint64_t);
            env->regs[13] = *(uint64_t *)value;
            break;
        case UC_X86_REG_R13D:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[13], *(uint32_t *)value);
            break;
        case UC_X86_REG_R13W:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[13], *(uint16_t *)value);
            break;
        case UC_X86_REG_R13B:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[13], *(uint8_t *)value);
            break;
        case UC_X86_REG_R14:
            CHECK_REG_TYPE(uint64_t);
            env->regs[14] = *(uint64_t *)value;
            break;
        case UC_X86_REG_R14D:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[14], *(uint32_t *)value);
            break;
        case UC_X86_REG_R14W:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[14], *(uint16_t *)value);
            break;
        case UC_X86_REG_R14B:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[14], *(uint8_t *)value);
            break;
        case UC_X86_REG_R15:
            CHECK_REG_TYPE(uint64_t);
            env->regs[15] = *(uint64_t *)value;
            break;
        case UC_X86_REG_R15D:
            CHECK_REG_TYPE(uint32_t);
            WRITE_DWORD(env->regs[15], *(uint32_t *)value);
            break;
        case UC_X86_REG_R15W:
            CHECK_REG_TYPE(uint16_t);
            WRITE_WORD(env->regs[15], *(uint16_t *)value);
            break;
        case UC_X86_REG_R15B:
            CHECK_REG_TYPE(uint8_t);
            WRITE_BYTE_L(env->regs[15], *(uint8_t *)value);
            break;
        case UC_X86_REG_IDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            env->idt.limit = (uint16_t)((uc_x86_mmr *)value)->limit;
            env->idt.base = ((uc_x86_mmr *)value)->base;
            break;
        case UC_X86_REG_GDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            env->gdt.limit = (uint16_t)((uc_x86_mmr *)value)->limit;
            env->gdt.base = ((uc_x86_mmr *)value)->base;
            break;
        case UC_X86_REG_LDTR:
            CHECK_REG_TYPE(uc_x86_mmr);
            env->ldt.limit = ((uc_x86_mmr *)value)->limit;
            env->ldt.base = ((uc_x86_mmr *)value)->base;
            env->ldt.selector = (uint16_t)((uc_x86_mmr *)value)->selector;
            env->ldt.flags = ((uc_x86_mmr *)value)->flags;
            break;
        case UC_X86_REG_TR:
            CHECK_REG_TYPE(uc_x86_mmr);
            env->tr.limit = ((uc_x86_mmr *)value)->limit;
            env->tr.base = ((uc_x86_mmr *)value)->base;
            env->tr.selector = (uint16_t)((uc_x86_mmr *)value)->selector;
            env->tr.flags = ((uc_x86_mmr *)value)->flags;
            break;
        case UC_X86_REG_MSR:
            CHECK_REG_TYPE(uc_x86_msr);
            x86_msr_write(env, (uc_x86_msr *)value);
            break;
        case UC_X86_REG_MXCSR:
            CHECK_REG_TYPE(uint32_t);
            cpu_set_mxcsr(env, *(uint32_t *)value);
            break;
        case UC_X86_REG_XMM8:
        case UC_X86_REG_XMM9:
        case UC_X86_REG_XMM10:
        case UC_X86_REG_XMM11:
        case UC_X86_REG_XMM12:
        case UC_X86_REG_XMM13:
        case UC_X86_REG_XMM14:
        case UC_X86_REG_XMM15: {
            CHECK_REG_TYPE(float64[2]);
            float64 *src = (float64 *)value;
            ZMMReg *reg = (ZMMReg *)&env->xmm_regs[regid - UC_X86_REG_XMM0];
            reg->ZMM_Q(0) = src[0];
            reg->ZMM_Q(1) = src[1];
            break;
        }
        case UC_X86_REG_FS_BASE:
            CHECK_REG_TYPE(uint64_t);
            env->segs[R_FS].base = *(uint64_t *)value;
            return 0;
        case UC_X86_REG_GS_BASE:
            CHECK_REG_TYPE(uint64_t);
            env->segs[R_GS].base = *(uint64_t *)value;
            return 0;
        }
        break;
#endif
    }

    return ret;
}

static uc_err reg_read_batch(CPUX86State *env, unsigned int *regs,
                             void *const *vals, size_t *sizes, int count,
                             int mode)
{
    int i;
    uc_err err;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        void *value = vals[i];
        err = reg_read(env, regid, value, sizes ? sizes + i : NULL, mode);
        if (err) {
            return err;
        }
    }

    return UC_ERR_OK;
}

static uc_err reg_write_batch(CPUX86State *env, unsigned int *regs,
                              const void *const *vals, size_t *sizes, int count,
                              int mode, int *setpc)
{
    int i;
    uc_err err;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        const void *value = vals[i];
        err =
            reg_write(env, regid, value, sizes ? sizes + i : NULL, mode, setpc);
        if (err) {
            return err;
        }
    }

    return UC_ERR_OK;
}

int x86_reg_read(struct uc_struct *uc, unsigned int *regs, void *const *vals,
                 size_t *sizes, int count)
{
    CPUX86State *env = &(X86_CPU(uc->cpu)->env);
    return reg_read_batch(env, regs, vals, sizes, count, uc->mode);
}

int x86_reg_write(struct uc_struct *uc, unsigned int *regs,
                  const void *const *vals, size_t *sizes, int count)
{
    CPUX86State *env = &(X86_CPU(uc->cpu)->env);
    int setpc = 0;
    uc_err err =
        reg_write_batch(env, regs, vals, sizes, count, uc->mode, &setpc);
    if (err) {
        return err;
    }
    if (setpc) {
        // force to quit execution and flush TB
        uc->quit_request = true;
        break_translation_loop(uc);
    }

    return UC_ERR_OK;
}

DEFAULT_VISIBILITY
int x86_context_reg_read(struct uc_context *ctx, unsigned int *regs,
                         void *const *vals, size_t *sizes, int count)
{
    CPUX86State *env = (CPUX86State *)ctx->data;
    return reg_read_batch(env, regs, vals, sizes, count, ctx->mode);
}

DEFAULT_VISIBILITY
int x86_context_reg_write(struct uc_context *ctx, unsigned int *regs,
                          const void *const *vals, size_t *sizes, int count)
{
    CPUX86State *env = (CPUX86State *)ctx->data;
    int setpc = 0;
    return reg_write_batch(env, regs, vals, sizes, count, ctx->mode, &setpc);
}

static bool x86_stop_interrupt(struct uc_struct *uc, int intno)
{
    switch (intno) {
    default:
        return false;
    case EXCP06_ILLOP:
        return true;
    }
}

static bool x86_insn_hook_validate(uint32_t insn_enum)
{
    // for x86 we can only hook IN, OUT, and SYSCALL
    if (insn_enum != UC_X86_INS_IN && insn_enum != UC_X86_INS_OUT &&
        insn_enum != UC_X86_INS_SYSCALL && insn_enum != UC_X86_INS_SYSENTER &&
        insn_enum != UC_X86_INS_CPUID) {
        return false;
    }
    return true;
}

static bool x86_opcode_hook_invalidate(uint32_t op, uint32_t flags)
{
    if (op != UC_TCG_OP_SUB) {
        return false;
    }

    switch (op) {
    case UC_TCG_OP_SUB:

        if ((flags & UC_TCG_OP_FLAG_CMP) && (flags & UC_TCG_OP_FLAG_DIRECT)) {
            return false;
        }

        break;

    default:
        return false;
    }

    return true;
}

static int x86_cpus_init(struct uc_struct *uc, const char *cpu_model)
{

    X86CPU *cpu;

    cpu = cpu_x86_init(uc);
    if (cpu == NULL) {
        return -1;
    }

    return 0;
}

DEFAULT_VISIBILITY
void x86_uc_init(struct uc_struct *uc)
{
    uc->reg_read = x86_reg_read;
    uc->reg_write = x86_reg_write;
    uc->reg_reset = x86_reg_reset;
    uc->release = x86_release;
    uc->set_pc = x86_set_pc;
    uc->get_pc = x86_get_pc;
    uc->stop_interrupt = x86_stop_interrupt;
    uc->insn_hook_validate = x86_insn_hook_validate;
    uc->opcode_hook_invalidate = x86_opcode_hook_invalidate;
    uc->cpus_init = x86_cpus_init;
    uc->cpu_context_size = offsetof(CPUX86State, retaddr);
    uc_common_init(uc);
}

/* vim: set ts=4 sts=4 sw=4 et:  */
