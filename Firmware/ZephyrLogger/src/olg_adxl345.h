#ifndef OLG_ADXL345_H
#define OLG_ADXL345_H

#include <stdint.h>

int olg_adxl345_init(void);
uint32_t olg_adxl345_sample_count(void);
uint32_t olg_adxl345_drain_count(void);
uint32_t olg_adxl345_i2c_failures(void);

#endif
