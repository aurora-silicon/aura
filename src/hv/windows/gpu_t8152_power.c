/* SPDX-License-Identifier: MIT */

/*
 * G1 GPU power-on and the boot-wide marker record.  See gpu_t8152_power.h.  No value of the
 * power recipe is in this file: it walks whatever gpu_t8152_psd_recipe() returns.
 */

#include "gpu_t8152_power.h"
#include "adt.h"
#include "gpu_t8152_admit.h"
#include "string.h"
#include "utils.h"

#define MAX_NODE_REGS 1024

static u64 block_of(const struct gpu_psd_recipe *r, u32 service)
{
    return r->base + (u64)service * r->stride;
}

static bool pmgr_covers(u64 base, u64 size)
{
    int armio = adt_path_offset(adt, "/arm-io");
    if (armio < 0)
        return false;
    int child = armio;
    ADT_FOREACH_CHILD(adt, child)
    {
        const char *name = adt_get_name(adt, child);
        if (!name || strncmp(name, "pmgr", 4))
            continue;
        int path[3] = {armio, child, 0};
        for (int idx = 0; idx < MAX_NODE_REGS; idx++) {
            u64 b, s;
            if (adt_get_reg(adt, path, "reg", idx, &b, &s) < 0)
                break;
            if (s && b <= base && base - b <= s && size <= s - (base - b))
                return true;
        }
    }
    return false;
}

int gpu_t8152_psd_check(const struct gpu_psd_recipe *r)
{
    if (!r || !r->available) {
        printf("HV: GPU: this carrier was built without the private PSD recipe "
               "(J804_PRIVATE_DATA/aura/g18g-j804-gpu-power.h): GPU power-on is disabled\n");
        return -1;
    }
    u32 regs[3] = {r->state_off, r->request_off, r->ack_off};
    bool ok = r->nsteps > 0 && r->nsteps <= GPU_PSD_MAX_STEPS && r->stride && r->block_size &&
              r->state_mask && r->ack_bit && r->polls && r->polls <= 100000 && r->poll_us &&
              r->poll_us <= 10000 && (r->cold_idle & ~r->state_mask) == 0 &&
              (r->read_order == GPU_PSD_READ_ACK_FIRST ||
               r->read_order == GPU_PSD_READ_STATE_FIRST);
    for (u32 i = 0; ok && i < 3; i++)
        ok = !(regs[i] & 3) && regs[i] + 4 <= r->block_size;
    for (u32 i = 0; ok && i < r->nsteps; i++) {
        const struct gpu_psd_step *s = &r->step[i];
        ok = (s->target & ~r->state_mask) == 0 && s->target != r->cold_idle && s->name;
        for (u32 j = 0; ok && j < i; j++)
            ok = r->step[j].service != s->service;
    }
    if (!ok) {
        printf("HV: GPU: the PSD recipe (%s) is malformed\n", r->source ? r->source : "?");
        return -1;
    }
    for (u32 i = 0; i < r->nsteps; i++) {
        u64 b = block_of(r, r->step[i].service);
        if (b < r->base || !pmgr_covers(b, r->block_size)) {
            printf("HV: GPU: PSD block of %s (service %u) at 0x%lx is not inside a pmgr register "
                   "range of this ADT: not touching it\n",
                   r->step[i].name, r->step[i].service, b);
            return -1;
        }
    }
    return 0;
}

static void sample_one(const struct gpu_psd_recipe *r, const struct gpu_psd_ops *ops,
                       u32 service, struct gpu_psd_sample *s)
{
    u64 b = block_of(r, service);
    s->service = service;
    if (r->read_order == GPU_PSD_READ_ACK_FIRST) {
        s->ack = ops->read32(b + r->ack_off);
        s->state = ops->read32(b + r->state_off) & r->state_mask;
    } else {
        s->state = ops->read32(b + r->state_off) & r->state_mask;
        s->ack = ops->read32(b + r->ack_off);
    }
}

void gpu_t8152_psd_sample(const struct gpu_psd_recipe *r, const struct gpu_psd_ops *ops,
                          struct gpu_psd_sample *out)
{
    for (u32 i = 0; i < r->nsteps; i++)
        sample_one(r, ops, r->step[i].service, &out[i]);
}

bool gpu_t8152_psd_all_cold(const struct gpu_psd_recipe *r, const struct gpu_psd_sample *s)
{
    for (u32 i = 0; i < r->nsteps; i++)
        if (s[i].state != r->cold_idle || (s[i].ack & r->ack_bit))
            return false;
    return true;
}

int gpu_t8152_psd_power_on(const struct gpu_psd_recipe *r, const struct gpu_psd_ops *ops,
                           struct gpu_psd_result *res)
{
    memset(res, 0, sizeof(*res));
    res->failed_step = -1;
    gpu_t8152_psd_sample(r, ops, res->before);
    for (u32 i = 0; i < r->nsteps; i++) {
        const struct gpu_psd_step *s = &r->step[i];
        u64 b = block_of(r, s->service);
        struct gpu_psd_sample now;
        bool earlier_on = true;
        for (u32 j = 0; j < i; j++) {
            struct gpu_psd_sample e;
            sample_one(r, ops, r->step[j].service, &e);
            earlier_on &= e.state == r->step[j].target;
        }
        sample_one(r, ops, s->service, &now);
        if (now.state != r->cold_idle || (now.ack & r->ack_bit) || !earlier_on) {
            printf("HV: GPU: PSD %s (service %u) refused before its request: state %u ack 0x%x "
                   "(precondition state %u wanted)%s\n",
                   s->name, s->service, now.state, now.ack, r->cold_idle,
                   earlier_on ? "" : "; an earlier service is no longer on");
            res->failed_step = (int)i;
            break;
        }
        ops->write32(b + r->request_off, s->request);
        u32 ack = 0, poll = 0;
        for (; poll < r->polls; poll++) {
            ack = ops->read32(b + r->ack_off);
            if (ack & r->ack_bit)
                break;
            ops->delay_us(r->poll_us);
        }
        res->polls[i] = poll;
        if (!(ack & r->ack_bit)) {
            printf("HV: GPU: PSD %s (service %u) did not acknowledge request %u within %u x %u us; "
                   "left as requested, nothing else touched\n",
                   s->name, s->service, s->request, r->polls, r->poll_us);
            res->failed_step = (int)i;
            break;
        }
        u32 state = ops->read32(b + r->state_off) & r->state_mask;
        printf("HV: GPU: PSD %s (service %u): request %u acknowledged after %u polls, state %u\n",
               s->name, s->service, s->request, poll, state);
        if (state != s->target) {
            printf("HV: GPU: PSD %s (service %u) acknowledged state %u, not %u; stopping\n",
                   s->name, s->service, state, s->target);
            res->failed_step = (int)i;
            break;
        }
    }
    gpu_t8152_psd_sample(r, ops, res->after);
    if (res->failed_step >= 0)
        return -1;
    for (u32 i = 0; i < r->nsteps; i++)
        if (res->after[i].state != r->step[i].target) {
            printf("HV: GPU: PSD %s (service %u) reads state %u after the sequence, not %u\n",
                   r->step[i].name, r->step[i].service, res->after[i].state, r->step[i].target);
            res->failed_step = (int)i;
            return -1;
        }
    return 0;
}

void gpu_t8152_marker_seal(struct gpu_boot_marker *m)
{
    m->magic = GPU_BOOT_MARKER_MAGIC;
    m->version = GPU_BOOT_MARKER_VERSION;
    m->crc = gpu_t8152_crc32(m, offsetof(struct gpu_boot_marker, crc));
}

bool gpu_t8152_marker_valid(const struct gpu_boot_marker *m)
{
    return m->magic == GPU_BOOT_MARKER_MAGIC && m->version == GPU_BOOT_MARKER_VERSION &&
           m->crc == gpu_t8152_crc32(m, offsetof(struct gpu_boot_marker, crc));
}
