#include "ld2410s_uart/ld2410s.h"
#include "rpigpio/rpigpio.h"

#include <stdatomic.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define DBUS_SERVICE   "io.homeboard.Occupancy"
#define DBUS_PATH      "/io/homeboard/Occupancy"
#define DBUS_INTERFACE "io.homeboard.Occupancy1"

static sd_bus *g_bus;

static int dbus_init(void) {
  int r = sd_bus_open_system(&g_bus);
  if (r < 0) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    return -1;
  }
  r = sd_bus_request_name(g_bus, DBUS_SERVICE, 0);
  if (r < 0) {
    fprintf(stderr, "sd_bus_request_name(%s): %s\n", DBUS_SERVICE, strerror(-r));
    sd_bus_unref(g_bus);
    g_bus = NULL;
    return -1;
  }
  return 0;
}

static volatile sig_atomic_t g_quit;
static atomic_size_t g_last_reported_distance;
static atomic_size_t g_last_reported_occupancy;

struct config {
  bool enable_uart;
  char device[64];
  bool debug;
  int sensor_report_gpio;
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
  atomic_store_explicit(&g_last_reported_distance, r->distance, memory_order_relaxed);
  atomic_store_explicit(&g_last_reported_occupancy, r->occupied, memory_order_relaxed);
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
  cfg->enable_uart = false;
  strncpy(cfg->device, "/dev/ttyUSB0", sizeof(cfg->device));
  cfg->debug = false;
  cfg->sensor_report_gpio = -1;
  cfg->cal_trigger = 2;
  cfg->cal_retention = 1;
  cfg->cal_duration = 120;

  struct json_object *val;
  if (json_object_object_get_ex(root, "enable_uart", &val))
    cfg->enable_uart = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "device", &val))
    strncpy(cfg->device, json_object_get_string(val), sizeof(cfg->device) - 1);
  if (json_object_object_get_ex(root, "debug", &val))
    cfg->debug = json_object_get_boolean(val);
  if (json_object_object_get_ex(root, "sensor_report_gpio", &val))
    cfg->sensor_report_gpio = json_object_get_int(val);
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

static void announce(struct tm *t, bool occupied, size_t dist) {
  printf("%02d:%02d:%02d Occupancy=%s Distance=%u\n",
         t->tm_hour, t->tm_min, t->tm_sec,
         occupied? "True" : "False", dist);

  if (!g_bus)
    return;
  int r = sd_bus_emit_signal(g_bus, DBUS_PATH, DBUS_INTERFACE, "StateChanged",
                             "bu", (int)occupied, dist);
  if (r < 0)
    fprintf(stderr, "sd_bus_emit_signal: %s\n", strerror(-r));
  sd_bus_flush(g_bus);
}

int main(int argc, char *argv[]) {
  setvbuf(stdout, NULL, _IOLBF, 0);

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
    return 1;
  }

  atomic_init(&g_last_reported_distance, 0);
  atomic_init(&g_last_reported_occupancy, 0);

  struct config cfg;
  if (load_config(argv[1], &cfg) < 0)
    return 1;

  if (cfg.sensor_report_gpio < 0 && !cfg.enable_uart) {
    fprintf(stderr, "GPIO and UART interfaces disabled, nothing to monitor\n");
    return 1;
  }

  if (dbus_init() < 0) {
    return 1;
  }

  struct RpiGpio *gpio = NULL;
  if (cfg.sensor_report_gpio >= 0) {
    const struct RpiGpioPin pins[] = {
        {cfg.sensor_report_gpio, RPIGPIO_INPUT, 0},
    };
    const int num_pins = sizeof(pins) / sizeof(pins[0]);
    gpio = rpigpio_open(0, "ld2410s_occupancy", pins, num_pins);
    if (!gpio) {
      fprintf(stderr, "Failed to enable GPIO pin %d monitoring.\n", cfg.sensor_report_gpio);
      sd_bus_flush_close_unref(g_bus);
      return 1;
    }
    printf("Enabled GPIO pin %d monitoring.\n", cfg.sensor_report_gpio);
  }

  struct LD2410S *uart;
  if (cfg.enable_uart) {
    uart = ld2410s_init(cfg.device, cfg.debug, report_handler, NULL, calibration_progress, NULL);
    if (!uart || ld2410s_start(uart) < 0) {
      sd_bus_flush_close_unref(g_bus);
      rpigpio_close(gpio);
      return 1;
    }

    apply_config(uart, argv[1]);
  }

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  bool calibration_force = false;
  if (argc > 2 && (strcmp(argv[2], "--calibrate") == 0)) {
    printf("Will force calibration as soon as the room is empty (if UART is enabled)\n");
    calibration_force = true;
  }

  bool last_announced_occupancy = false;
  size_t last_announced_distance = 0;
  int last_cal_yday = -1;
  bool calibration_requested = false;

  while (!g_quit) {
    bool gpio_reports_occupancy = false;
    if (gpio) {
      gpio_reports_occupancy = rpigpio_read(gpio, cfg.sensor_report_gpio);
    }

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    const bool occupancy = (gpio_reports_occupancy ||
                            atomic_load_explicit(&g_last_reported_occupancy, memory_order_relaxed));
    const size_t last_dist = atomic_load_explicit(&g_last_reported_distance, memory_order_relaxed);
    if (occupancy != last_announced_occupancy || last_dist != last_announced_distance) {
      last_announced_occupancy = occupancy;
      last_announced_distance = last_dist;
      announce(&tm, occupancy, last_dist);
    }

    /* Calibrate at 3:00 AM, once per day */
    if (uart) {
      if (tm.tm_hour == 3 && tm.tm_min == 0) {
        calibration_requested = false;
      }
      if ((tm.tm_hour == 3 && tm.tm_yday != last_cal_yday) || calibration_force) {
        if (!calibration_requested) {
          if (ld2410s_get_vacant_reports_count(uart) >= 30) {
            last_cal_yday = tm.tm_yday;
            calibration_requested = true;
            run_calibration(uart, &cfg);
          }
        }
      }
    }

    sleep(1);
  }

  sd_bus_flush_close_unref(g_bus);
  ld2410s_free(uart);
  rpigpio_close(gpio);
  return 0;
}
