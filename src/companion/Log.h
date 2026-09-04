#pragma once
#if CROSSPOINT_COMPANION

// Logging shim: the device build uses the repo's Logging.h; host tests compile the
// same sources without Arduino and get no-ops.

#if defined(ARDUINO)
#include <Logging.h>
#define CLOG_ERR(fmt, ...) LOG_ERR("CMP", fmt, ##__VA_ARGS__)
#define CLOG_INF(fmt, ...) LOG_INF("CMP", fmt, ##__VA_ARGS__)
#define CLOG_DBG(fmt, ...) LOG_DBG("CMP", fmt, ##__VA_ARGS__)
#else
#define CLOG_ERR(fmt, ...) ((void)0)
#define CLOG_INF(fmt, ...) ((void)0)
#define CLOG_DBG(fmt, ...) ((void)0)
#endif

#endif  // CROSSPOINT_COMPANION
