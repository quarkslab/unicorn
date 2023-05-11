/* Unicorn Emulator Engine */
/* By Nguyen Anh Quynh <aquynh@gmail.com>, 2015 */
/* Modified for Unicorn Engine by Chen Huitao<chenhuitao@hfmrit.com>, 2020 */

#include "sysemu/cpus.h"
#include "cpu.h"
#include "unicorn_common.h"
#include "uc_priv.h"
#include "unicorn.h"

static bool sparc_stop_interrupt(struct uc_struct *uc, int intno)
{
    switch (intno) {
    default:
        return false;
    case TT_ILL_INSN:
        return true;
    }
}

static void sparc_set_pc(struct uc_struct *uc, uint64_t address)
{
    ((CPUSPARCState *)uc->cpu->env_ptr)->pc = address;
    ((CPUSPARCState *)uc->cpu->env_ptr)->npc = address + 4;
}

static uint64_t sparc_get_pc(struct uc_struct *uc)
{
    return ((CPUSPARCState *)uc->cpu->env_ptr)->pc;
}

static void sparc_release(void *ctx)
{
    int i;
    TCGContext *tcg_ctx = (TCGContext *)ctx;
    SPARCCPU *cpu = (SPARCCPU *)tcg_ctx->uc->cpu;
    CPUTLBDesc *d = cpu->neg.tlb.d;
    CPUTLBDescFast *f = cpu->neg.tlb.f;
    CPUTLBDesc *desc;
    CPUTLBDescFast *fast;

    release_common(ctx);
    for (i = 0; i < NB_MMU_MODES; i++) {
        desc = &(d[i]);
        fast = &(f[i]);
        g_free(desc->iotlb);
        g_free(fast->table);
    }
}

void sparc_reg_reset(struct uc_struct *uc)
{
    CPUArchState *env = uc->cpu->env_ptr;

    memset(env->gregs, 0, sizeof(env->gregs));
    memset(env->fpr, 0, sizeof(env->fpr));
    memset(env->regbase, 0, sizeof(env->regbase));

    env->pc = 0;
    env->npc = 0;
    env->regwptr = env->regbase;
}

static uc_err reg_read(CPUSPARCState *env, unsigned int regid, void *value,
                       size_t *size)
{
    uc_err ret = UC_ERR_ARG;

    if (regid >= UC_SPARC_REG_G0 && regid <= UC_SPARC_REG_G7) {
        CHECK_REG_TYPE(uint32_t);
        *(uint32_t *)value = env->gregs[regid - UC_SPARC_REG_G0];
    } else if (regid >= UC_SPARC_REG_O0 && regid <= UC_SPARC_REG_O7) {
        CHECK_REG_TYPE(uint32_t);
        *(uint32_t *)value = env->regwptr[regid - UC_SPARC_REG_O0];
    } else if (regid >= UC_SPARC_REG_L0 && regid <= UC_SPARC_REG_L7) {
        CHECK_REG_TYPE(uint32_t);
        *(uint32_t *)value = env->regwptr[8 + regid - UC_SPARC_REG_L0];
    } else if (regid >= UC_SPARC_REG_I0 && regid <= UC_SPARC_REG_I7) {
        *(uint32_t *)value = env->regwptr[16 + regid - UC_SPARC_REG_I0];
    } else {
        switch (regid) {
        default:
            break;
        case UC_SPARC_REG_PC:
            CHECK_REG_TYPE(uint32_t);
            *(uint32_t *)value = env->pc;
            break;
        }
    }

    return ret;
}

static uc_err reg_write(CPUSPARCState *env, unsigned int regid,
                        const void *value, size_t *size)
{
    uc_err ret = UC_ERR_ARG;

    if (regid >= UC_SPARC_REG_G0 && regid <= UC_SPARC_REG_G7) {
        CHECK_REG_TYPE(uint32_t);
        env->gregs[regid - UC_SPARC_REG_G0] = *(uint32_t *)value;
    } else if (regid >= UC_SPARC_REG_O0 && regid <= UC_SPARC_REG_O7) {
        CHECK_REG_TYPE(uint32_t);
        env->regwptr[regid - UC_SPARC_REG_O0] = *(uint32_t *)value;
    } else if (regid >= UC_SPARC_REG_L0 && regid <= UC_SPARC_REG_L7) {
        CHECK_REG_TYPE(uint32_t);
        env->regwptr[8 + regid - UC_SPARC_REG_L0] = *(uint32_t *)value;
    } else if (regid >= UC_SPARC_REG_I0 && regid <= UC_SPARC_REG_I7) {
        CHECK_REG_TYPE(uint32_t);
        env->regwptr[16 + regid - UC_SPARC_REG_I0] = *(uint32_t *)value;
    } else {
        switch (regid) {
        default:
            break;
        case UC_SPARC_REG_PC:
            CHECK_REG_TYPE(uint32_t);
            env->pc = *(uint32_t *)value;
            env->npc = *(uint32_t *)value + 4;
            break;
        }
    }

    return ret;
}

int sparc_reg_read(struct uc_struct *uc, unsigned int *regs, void *const *vals,
                   size_t *sizes, int count)
{
    CPUSPARCState *env = &(SPARC_CPU(uc->cpu)->env);
    int i;
    uc_err err;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        void *value = vals[i];
        err = reg_read(env, regid, value, sizes ? sizes + i : NULL);
        if (err) {
            return err;
        }
    }

    return UC_ERR_OK;
}

int sparc_reg_write(struct uc_struct *uc, unsigned int *regs,
                    const void *const *vals, size_t *sizes, int count)
{
    CPUSPARCState *env = &(SPARC_CPU(uc->cpu)->env);
    int i;
    uc_err err;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        const void *value = vals[i];
        err = reg_write(env, regid, value, sizes ? sizes + i : NULL);
        if (err) {
            return err;
        }
        if (regid == UC_SPARC_REG_PC) {
            // force to quit execution and flush TB
            uc->quit_request = true;
            break_translation_loop(uc);
        }
    }

    return UC_ERR_OK;
}

DEFAULT_VISIBILITY
int sparc_context_reg_read(struct uc_context *ctx, unsigned int *regs,
                           void *const *vals, size_t *sizes, int count)
{
    CPUSPARCState *env = (CPUSPARCState *)ctx->data;
    int i;
    uc_err err;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        void *value = vals[i];
        err = reg_read(env, regid, value, sizes ? sizes + i : NULL);
        if (err) {
            return err;
        }
    }

    return UC_ERR_OK;
}

DEFAULT_VISIBILITY
int sparc_context_reg_write(struct uc_context *ctx, unsigned int *regs,
                            const void *const *vals, size_t *sizes, int count)
{
    CPUSPARCState *env = (CPUSPARCState *)ctx->data;
    int i;
    uc_err err;

    for (i = 0; i < count; i++) {
        unsigned int regid = regs[i];
        const void *value = vals[i];
        err = reg_write(env, regid, value, sizes ? sizes + i : NULL);
        if (err) {
            return err;
        }
    }

    return UC_ERR_OK;
}

static int sparc_cpus_init(struct uc_struct *uc, const char *cpu_model)
{
    SPARCCPU *cpu;

    cpu = cpu_sparc_init(uc);
    if (cpu == NULL) {
        return -1;
    }
    return 0;
}

DEFAULT_VISIBILITY
void sparc_uc_init(struct uc_struct *uc)
{
    uc->release = sparc_release;
    uc->reg_read = sparc_reg_read;
    uc->reg_write = sparc_reg_write;
    uc->reg_reset = sparc_reg_reset;
    uc->set_pc = sparc_set_pc;
    uc->get_pc = sparc_get_pc;
    uc->stop_interrupt = sparc_stop_interrupt;
    uc->cpus_init = sparc_cpus_init;
    uc->cpu_context_size = offsetof(CPUSPARCState, irq_manager);
    uc_common_init(uc);
}
