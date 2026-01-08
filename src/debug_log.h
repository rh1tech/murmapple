#pragma once

#include <stdio.h>

#ifndef ENABLE_DEBUG_LOGS
#define ENABLE_DEBUG_LOGS 0
#endif

#if ENABLE_DEBUG_LOGS
#define MII_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
#define MII_DEBUG_PRINTF(...) do { } while (0)
#endif
