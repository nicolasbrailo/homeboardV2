#pragma once

#include <stdint.h>

struct ambience_config {
  uint32_t transition_time_s;
  uint32_t rotation; // 0, 90, 180, 270
};

int ambience_config_load(const char *path, struct ambience_config *cfg);
