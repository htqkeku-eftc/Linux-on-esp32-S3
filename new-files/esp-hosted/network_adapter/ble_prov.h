// SPDX-License-Identifier: Apache-2.0

#ifndef __BLE_PROV_H__
#define __BLE_PROV_H__

#include <stdint.h>
#include "esp_err.h"

esp_err_t ble_prov_init(void);

int ble_prov_send(const char *data, int len);

int ble_prov_to_linux(const char *data, int len);

void ble_prov_from_linux(const uint8_t *payload, uint16_t len);

#endif
