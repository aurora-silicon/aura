#pragma once
#include "rtkit.h"
sart_dev_t *sart_init(const char *adt_path);
void sart_free(sart_dev_t *sart);
struct sart_entry {
    u8 index;
    u8 flags;
    u64 paddr;
    u64 size;
};
bool sart_get_allowed(sart_dev_t *sart, int index, u64 *paddr, size_t *sz);
bool sart_clear_entry(sart_dev_t *sart, int index);
bool sart_set_allowed(sart_dev_t *sart, int index, u64 paddr, size_t sz);
void sart_release(sart_dev_t *sart);
int sart_get_entries(sart_dev_t *sart, struct sart_entry *entries, int max);
bool sart_set_entries(sart_dev_t *sart, const struct sart_entry *entries, int count);
