/* SPDX-License-Identifier: MIT */

#ifndef SART_H
#define SART_H

#include "types.h"

typedef struct sart_dev sart_dev_t;

sart_dev_t *sart_init(const char *adt_path);
void sart_free(sart_dev_t *asc);

bool sart_add_allowed_region(sart_dev_t *sart, void *paddr, size_t sz);
bool sart_get_allowed(sart_dev_t *sart, int index, u64 *paddr, size_t *sz);
bool sart_clear_entry(sart_dev_t *sart, int index);
bool sart_set_allowed(sart_dev_t *sart, int index, u64 paddr, size_t sz);
void sart_release(sart_dev_t *sart);
bool sart_remove_allowed_region(sart_dev_t *sart, void *paddr, size_t sz);

#endif
