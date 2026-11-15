#ifndef CUBECELL_UTILS_H
#define CUBECELL_UTILS_H

#include <stdint.h>

// Persist Port-4 sleep interval across power cycles (SFLASH user rows).
// Returns true if a stored interval was applied to dutyCycleMs.
bool loadPersistedSleepTime(uint32_t &dutyCycleMs);
void persistSleepTime(uint16_t sleepSeconds);

#endif
