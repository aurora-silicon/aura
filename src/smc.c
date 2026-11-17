/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "adt.h"
#include "assert.h"
#include "malloc.h"
#include "smc.h"
#include "soc.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#define SMC_READ_KEY         0x10
#define SMC_WRITE_KEY        0x11
#define SMC_GET_KEY_BY_INDEX 0x12
#define SMC_GET_KEY_INFO     0x13
#define SMC_INITIALIZE       0x17
#define SMC_NOTIFICATION     0x18
#define SMC_RW_KEY           0x20

#define SMC_MSG_TYPE GENMASK(7, 0)
#define SMC_MSG_ID   GENMASK(15, 12)

#define SMC_WRITE_KEY_SIZE GENMASK(23, 16)
#define SMC_WRITE_KEY_KEY  GENMASK(63, 32)

#define SMC_RESULT_RESULT GENMASK(7, 0)
#define SMC_RESULT_ID     GENMASK(15, 12)
#define SMC_RESULT_SIZE   GENMASK(31, 16)
#define SMC_RESULT_VALUE  GENMASK(63, 32)

#define SMC_NUM_IDS 16

#define SMC_ENDPOINT 0x20

struct smc_dev {
    asc_dev_t *asc;
    rtkit_dev_t *rtkit;

    void *shmem;
    u32 msgid;

    bool outstanding[SMC_NUM_IDS];
    u64 ret[SMC_NUM_IDS];
};

static void smc_handle_msg(smc_dev_t *smc, u64 msg)
{
    if (!smc->shmem)
        smc->shmem = (void *)msg;
    else {
        u8 result = FIELD_GET(SMC_RESULT_RESULT, msg);
        u8 id = FIELD_GET(SMC_RESULT_ID, msg);
        if (result == SMC_NOTIFICATION) {
            printf("SMC: Notification: 0x%08lx\n", FIELD_GET(SMC_RESULT_VALUE, msg));
            return;
        }
        smc->outstanding[id] = false;
        smc->ret[id] = msg;
    }
}

static int smc_work(smc_dev_t *smc)
{
    int ret;
    struct rtkit_message msg;

    ret = rtkit_recv(smc->rtkit, &msg);
    if (ret < 0)
        printf("SMC: rtkit_recv failed!\n");
    if (ret <= 0)
        return ret;

    if (msg.ep != SMC_ENDPOINT) {
        printf("SMC: received message for unexpected endpoint 0x%02x\n", msg.ep);
        return 0;
    }

    smc_handle_msg(smc, msg.msg);

    return 0;
}

static void smc_send(smc_dev_t *smc, u64 message)
{
    struct rtkit_message msg;

    msg.ep = SMC_ENDPOINT;
    msg.msg = message;

    rtkit_send(smc->rtkit, &msg);
}

static int smc_cmd(smc_dev_t *smc, u64 message)
{
    u8 id = smc->msgid++ & 0xF;
    assert(!smc->outstanding[id]);
    smc->outstanding[id] = true;

    message |= FIELD_PREP(SMC_MSG_ID, id);

    smc_send(smc, message);
    while (smc->outstanding[id]) {
        if (smc_work(smc) < 0)
            return -1;
    }

    u64 result = smc->ret[id];
    u32 ret = FIELD_GET(SMC_RESULT_RESULT, result);
    if (ret) {
        printf("SMC: smc_cmd[0x%x] failed: %u\n", id, ret);
        return ret;
    }

    return 0;
}

void smc_shutdown(smc_dev_t *smc)
{
    rtkit_quiesce(smc->rtkit);
    rtkit_free(smc->rtkit);
    asc_free(smc->asc);
    free(smc);
}

smc_dev_t *smc_init(void)
{
    smc_dev_t *smc = calloc(1, sizeof(smc_dev_t));
    if (!smc)
        return NULL;

    smc->asc = asc_init("/arm-io/smc");
    if (!smc->asc) {
        printf("SMC: failed to initialize ASC\n");
        goto out_free;
    }

    smc->rtkit = rtkit_init("smc", smc->asc, NULL, NULL, NULL, true);
    if (!smc->rtkit) {
        printf("SMC: failed to initialize RTKit\n");
        goto out_asc;
    }

    /* On T8152 (J873g) a previous stage leaves the SMC running with its RTKit
     * session established; every other machine boots it as before. */
    bool adopted = false;
    bool booted =
        chip_id == T8152 ? rtkit_boot_or_adopt(smc->rtkit, &adopted) : rtkit_boot(smc->rtkit);
    if (!booted) {
        printf("SMC: failed to boot RTKit\n");
        goto out_rtkit;
    }

    if (!rtkit_start_ep(smc->rtkit, SMC_ENDPOINT)) {
        printf("SMC: failed start SMC endpoint\n");
        goto out_rtkit;
    }

    u64 initialize =
        FIELD_PREP(SMC_MSG_TYPE, SMC_INITIALIZE) | FIELD_PREP(SMC_MSG_ID, smc->msgid++);

    smc_send(smc, initialize);

    /* An adopted session is confirmed only by the SMC endpoint answering. */
    u64 timeout = timeout_calculate(2 * USEC_PER_SEC);
    while (!smc->shmem) {
        int ret = smc_work(smc);
        if (ret < 0)
            goto out_rtkit;
        if (adopted && timeout_expired(timeout)) {
            printf("SMC: adopted session did not answer, giving up\n");
            goto out_rtkit;
        }
    }

    return smc;

out_rtkit:
    rtkit_free(smc->rtkit);
out_asc:
    asc_free(smc->asc);
out_free:
    free(smc);
    return NULL;
}

int smc_write_u32(smc_dev_t *smc, u32 key, u32 value)
{
    memcpy(smc->shmem, &value, sizeof(value));
    u64 msg = FIELD_PREP(SMC_MSG_TYPE, SMC_WRITE_KEY);
    msg |= FIELD_PREP(SMC_WRITE_KEY_SIZE, sizeof(value));
    msg |= FIELD_PREP(SMC_WRITE_KEY_KEY, key);

    return smc_cmd(smc, msg);
}

int smc_write_u64(smc_dev_t *smc, u32 key, u64 value)
{
    memcpy(smc->shmem, &value, sizeof(value));
    u64 msg = FIELD_PREP(SMC_MSG_TYPE, SMC_WRITE_KEY);
    msg |= FIELD_PREP(SMC_WRITE_KEY_SIZE, sizeof(value));
    msg |= FIELD_PREP(SMC_WRITE_KEY_KEY, key);

    return smc_cmd(smc, msg);
}

/* ADT function-reset = pKW4/pKW8: the SMC node's phandle, the function name, then its arguments */
struct smc_adt_function {
    u32 phandle;
    char four_cc[4];
    u32 key;
    u32 lo;
    u32 hi; /* pKW8 only */
};

int smc_adt_function_reset(const char *path, u32 settle_ms)
{
    struct smc_adt_function fn = {0};
    u32 len;
    int node = adt_path_offset(adt, path);
    const void *prop = node < 0 ? NULL : adt_getprop(adt, node, "function-reset", &len);

    if (!prop || len < 12)
        return 1;
    memcpy(&fn, prop, min(len, (u32)sizeof(fn)));
    bool wide = !memcmp(fn.four_cc, "8WKp", 4);
    if (!(wide && len == 20) && !(!memcmp(fn.four_cc, "4WKp", 4) && len == 16))
        return 1; /* a GPIO or other non-SMC function */

    smc_dev_t *smc = smc_init();
    if (!smc)
        return -1;
    int ret = 0;
    for (u32 level = 0; level < 2 && !ret; level++) {
        ret = wide ? smc_write_u64(smc, fn.key, ((u64)fn.hi << 32) | fn.lo | level)
                   : smc_write_u32(smc, fn.key, fn.lo | level);
        mdelay(settle_ms);
    }
    smc_shutdown(smc);
    printf("SMC: %s function-reset %c%c%c%c released: %d\n", path, fn.key >> 24, fn.key >> 16,
           fn.key >> 8, fn.key, ret);
    return ret ? -1 : 0;
}
