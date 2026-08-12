#pragma once

#include "sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

int gc0328_detect(int slv_addr, sensor_id_t *id);
int gc0328_init(sensor_t *sensor);

#ifdef __cplusplus
}
#endif
