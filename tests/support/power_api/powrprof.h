#pragma once

struct SYSTEM_POWER_CAPABILITIES {
  unsigned char SystemS1, SystemS2, SystemS3, FastSystemS4;
#ifdef SUNSHINE_TEST_LEGACY_POWER_CAPABILITIES
  unsigned char spare2[3];
#else
  unsigned char Hiberboot, WakeAlarmPresent, AoAc;
#endif
};

BOOL GetPwrCapabilities(SYSTEM_POWER_CAPABILITIES *);
BOOL SetSuspendState(BOOL, BOOL, BOOL);
