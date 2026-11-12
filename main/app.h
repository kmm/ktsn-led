#pragma once
#include "dsl.h"

extern channel_t g_channels[];
extern int       g_nchannels;
extern globals_t g_globals;
extern env_t     g_env;

/* Compile and run a program once in a temporary channel (global buffer setup). Returns 0, or -1 with err set. */
int autoexec_run(const char *src, char *err, size_t errlen);
