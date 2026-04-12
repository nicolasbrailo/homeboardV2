#include "ld2410s_uart/ld2410s.h"
#include "rpigpio/rpigpio.h"

#include <json-c/json.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_quit;

struct config {
  char device[64];
  bool debug;
  uint16_t cal_trigger;
  uint16_t cal_retention;
  uint16_t cal_duration;
};

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

static void report_handler(const struct LD2410S_report *r, void *ud) {
  (void)ud;
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  printf("%02d:%02d:%02d Report: Occupancy=%s Distance=%u\n",
         tm.tm_hour, tm.tm_min, tm.tm_sec,
         r->occupied ? "True" : "False", r->distance);
}

static void calibration_progress(uint16_t progress, void *ud) {
  (void)ud;
  printf("Calibration progress: %u%%\n", progress);
}

static int load_config(const char *path, struct config *cfg) {
  struct json_object *root = json_object_from_file(path);
  if (!root) {
    fprintf(stderr, "Failed to parse config: %s\n", path);
    return -1;
  }

  /* Defaults */
  strncpy(cfg->device, "/dev/ttyUSB0", sizeof(cfg->device));
  cfg->debug = false;
  cfg->cal_trigger = 2;
  cfg->cal_retention = 1;
  cfg->cal_duration = 120;

  struct json_object *val;
  if (json_object_object_get_ex(root, "device", &val))
    strncpy(cfg->device, json_object_get_string(val), sizeof(cfg->device) - 1);
  if (json_object_object_get_ex(root, "debug", &val))
    cfg->debug = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "calibration_trigger", &val))
    cfg->cal_trigger = (uint16_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "calibration_retention", &val))
    cfg->cal_retention = (uint16_t)json_object_get_int(val);
  if (json_object_object_get_ex(root, "calibration_duration", &val))
    cfg->cal_duration = (uint16_t)json_object_get_int(val);

  json_object_put(root);
  return 0;
}

static int apply_config(struct LD2410S *s, const char *path) {
  struct json_object *root = json_object_from_file(path);
  if (!root)
    return -1;

  static const char *param_keys[] = {
      "farthest_gate",      "nearest_gate",       "unmanned_delay",
      "status_report_freq", "dist_report_freq",   "response_speed",
      NULL,
  };

  for (const char **k = param_keys; *k; k++) {
    struct json_object *val;
    if (!json_object_object_get_ex(root, *k, &val))
      continue;

    uint32_t v = (uint32_t)json_object_get_int(val);
    printf("Setting %s = %u\n", *k, v);
    if (ld2410s_set_param(s, *k, v) < 0)
      fprintf(stderr, "Failed to set %s\n", *k);
    else
      printf("ACK set %s = %u\n", *k, v);
  }

  json_object_put(root);
  return 0;
}

static void run_calibration(struct LD2410S *s, const struct config *cfg) {
  printf("Starting calibration (trigger=%u retention=%u duration=%us)\n", cfg->cal_trigger, cfg->cal_retention,
         cfg->cal_duration);
  if (ld2410s_start_calibration(s, cfg->cal_trigger, cfg->cal_retention, cfg->cal_duration) < 0)
    fprintf(stderr, "Calibration failed to start\n");
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
    return 1;
  }

  int sensor_report_gpio = 2; // Pin 3, GPIO2
  const struct RpiGpioPin pins[] = {
      {sensor_report_gpio, RPIGPIO_INPUT, 0},
  };
  const int num_pins = sizeof(pins) / sizeof(pins[0]);
  struct RpiGpio *gpio = rpigpio_open(0, "ld2410s_occupancy", pins, num_pins);

  setvbuf(stdout, NULL, _IOLBF, 0);

  struct config cfg;
  if (load_config(argv[1], &cfg) < 0)
    return 1;

  struct LD2410S *sensor = ld2410s_init(cfg.device, cfg.debug, report_handler, NULL, calibration_progress, NULL);
  if (!sensor)
    return 1;

  if (ld2410s_start(sensor) < 0) {
    ld2410s_free(sensor);
    return 1;
  }

  /* Apply sensor params from config */
  apply_config(sensor, argv[1]);

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  int last_cal_yday = -1;
  bool calibration_requested = false;

  while (!g_quit) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    /* Calibrate at 3:00 AM, once per day */
    if (tm->tm_hour == 3 && tm->tm_min == 0) {
      calibration_requested = false;
    }
    if (tm->tm_hour == 3 && tm->tm_yday != last_cal_yday && !calibration_requested) {
      if (ld2410s_get_vacant_reports_count(sensor) >= 5) {
        last_cal_yday = tm->tm_yday;
        calibration_requested = true;
        run_calibration(sensor, &cfg);
      }
    }

    printf("GPIO = %zu\n", rpigpio_read(gpio, sensor_report_gpio));
    sleep(1);
  }

  ld2410s_free(sensor);
  return 0;
}
