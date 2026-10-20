#pragma once

struct SYSTEM_POWER_CAPABILITIES {
  BOOL SystemS1, SystemS2, SystemS3, AoAc;
};

BOOL GetPwrCapabilities(SYSTEM_POWER_CAPABILITIES *);
BOOL SetSuspendState(BOOL, BOOL, BOOL);
