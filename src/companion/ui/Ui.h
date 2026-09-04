#pragma once
#if CROSSPOINT_COMPANION

// The firmware singletons the companion screens need. main.cpp defines all
// three; ActivityManager already publishes itself the same way, so declaring the
// other two here keeps the fork's UI out of the upstream headers.

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"  // ActivityManager's inline ctor needs the complete type
#include "activities/ActivityManager.h"

extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

#endif  // CROSSPOINT_COMPANION
