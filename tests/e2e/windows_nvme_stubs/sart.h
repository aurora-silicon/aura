#pragma once
#include "rtkit.h"
sart_dev_t *sart_init(const char *adt_path);
void sart_free(sart_dev_t *sart);
