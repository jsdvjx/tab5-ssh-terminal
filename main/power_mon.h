// INA226 battery monitor (1S Li-ion behind a shunt). Optional hardware:
// init probes the bus and disables itself gracefully when absent.
#pragma once

#include <stdbool.h>

// Probe + configure the INA226 and start the 2s polling task.
// Returns false (and stays inert) if no INA226 answers on the bus.
bool power_mon_init(void);

// Latest cached reading. Returns false if no INA226 was found (callers
// should hide the battery indicator). Any out pointer may be NULL.
bool power_mon_get(float *volts, int *percent, bool *charging);

// Extended reading: bus volts, current (mA, positive = discharging) and
// power (mW), derived from the shunt voltage assuming POWER_MON_SHUNT_MOHM.
// Returns false if no INA226 / no valid sample yet. Out pointers may be NULL.
bool power_mon_get_ext(float *volts, float *ma, float *mw, bool *charging);
