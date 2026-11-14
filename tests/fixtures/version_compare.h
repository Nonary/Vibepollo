// Resolve the frozen updater's quoted include without adding src/ to -I.
// On MinGW that search path shadows the CRT's <process.h> with our process.h.
#include "../../src/version_compare.h"
