#pragma once

#include <stdbool.h>
#include <sys/types.h>

// Release an existing lease (if expected_owner is a valid pid), or force-release if expected_owner <= 0
void drm_lease_release(pid_t expected_owner);
bool drm_lease_is_alive();
int drm_lease_assign(pid_t peer);
pid_t drm_lease_get_active();
