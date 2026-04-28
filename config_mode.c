/*
 * Configuration-mode hardware and helper integration.
 */
#include "config_mode.h"
#include "util.h"

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONFIG_GPIO_LINE 13
#define CONFIG_DEBOUNCE_FRAMES 4

/* Debian bookworm / libgpiod 1.x: chip + line API. libgpiod 2.x: line_request API. */
#if defined(GPIOD_LINE_BULK_MAX_LINES)
#define CONFIG_MODE_GPIOD_V1 1
#endif

static const char *status_path_default(void) {
    const char *p = getenv("CONFIG_STATUS_PATH");
    return (p && *p) ? p : "/tmp/arrival_board_config_status";
}

static void write_status(ConfigMode *cm, const char *status) {
    if (!cm || !cm->status_path[0] || !status) return;
    FILE *f = fopen(cm->status_path, "w");
    if (!f) return;
    fprintf(f, "%s\n", status);
    fclose(f);
}

int config_mode_init(ConfigMode *cm) {
    if (!cm) return -1;
    memset(cm, 0, sizeof(*cm));
    snprintf(cm->status_path, sizeof(cm->status_path), "%s", status_path_default());
    cm->last_value = 1;

    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) {
        logf_("CONFIG_MODE GPIO unavailable: gpiochip0 open failed: %s", strerror(errno));
        return -1;
    }

#ifdef CONFIG_MODE_GPIOD_V1
    struct gpiod_line *line = gpiod_chip_get_line(chip, CONFIG_GPIO_LINE);
    if (!line) {
        logf_("CONFIG_MODE GPIO unavailable: line %d not found", CONFIG_GPIO_LINE);
        gpiod_chip_close(chip);
        return -1;
    }
    if (gpiod_line_request_input_flags(line, "arrival-board-config",
                                       GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) != 0) {
        logf_("CONFIG_MODE GPIO unavailable: line request failed: %s", strerror(errno));
        gpiod_chip_close(chip);
        return -1;
    }
    cm->chip = chip;
    cm->request = line;
#else
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    if (!settings || !line_cfg || !req_cfg) {
        logf_("CONFIG_MODE GPIO unavailable: gpiod allocation failed");
        if (settings) gpiod_line_settings_free(settings);
        if (line_cfg) gpiod_line_config_free(line_cfg);
        if (req_cfg) gpiod_request_config_free(req_cfg);
        gpiod_chip_close(chip);
        return -1;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
    unsigned int offsets[1] = { CONFIG_GPIO_LINE };
    if (gpiod_line_config_add_line_settings(line_cfg, offsets, 1, settings) != 0) {
        logf_("CONFIG_MODE GPIO unavailable: line settings failed: %s", strerror(errno));
        gpiod_line_settings_free(settings);
        gpiod_line_config_free(line_cfg);
        gpiod_request_config_free(req_cfg);
        gpiod_chip_close(chip);
        return -1;
    }
    gpiod_request_config_set_consumer(req_cfg, "arrival-board-config");

    struct gpiod_line_request *request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_cfg);
    gpiod_request_config_free(req_cfg);
    if (!request) {
        logf_("CONFIG_MODE GPIO unavailable: line request failed: %s", strerror(errno));
        gpiod_chip_close(chip);
        return -1;
    }

    cm->chip = chip;
    cm->request = request;
#endif
    cm->gpio_ready = 1;
    cm->last_value = 1;
    cm->stable_count = 0;
    write_status(cm, "Ready");
    logf_("CONFIG_MODE GPIO13 active-low input ready with pull-up bias");
    return 0;
}

void config_mode_destroy(ConfigMode *cm) {
    if (!cm) return;
#ifdef CONFIG_MODE_GPIOD_V1
    if (cm->request) {
        gpiod_line_release((struct gpiod_line *)cm->request);
        cm->request = NULL;
    }
#else
    if (cm->request) {
        gpiod_line_request_release((struct gpiod_line_request *)cm->request);
        cm->request = NULL;
    }
#endif
    if (cm->chip) {
        gpiod_chip_close((struct gpiod_chip *)cm->chip);
        cm->chip = NULL;
    }
    cm->gpio_ready = 0;
}

int config_mode_poll_pressed(ConfigMode *cm) {
    if (!cm || !cm->gpio_ready || !cm->request) return 0;

    int value;
#ifdef CONFIG_MODE_GPIOD_V1
    int v = gpiod_line_get_value((struct gpiod_line *)cm->request);
    if (v < 0) {
        logf_("CONFIG_MODE GPIO read failed: %s", strerror(errno));
        return 0;
    }
    value = v;
#else
    enum gpiod_line_value line_value =
        gpiod_line_request_get_value((struct gpiod_line_request *)cm->request, CONFIG_GPIO_LINE);
    if (line_value < 0) {
        logf_("CONFIG_MODE GPIO read failed: %s", strerror(errno));
        return 0;
    }
    value = (line_value == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
#endif

    if (value == cm->last_value) {
        if (cm->stable_count < CONFIG_DEBOUNCE_FRAMES)
            cm->stable_count++;
    } else {
        cm->last_value = value;
        cm->stable_count = 0;
    }

    if (value == 0 && cm->stable_count == CONFIG_DEBOUNCE_FRAMES) {
        cm->stable_count++;
        return 1;
    }
    return 0;
}

int config_mode_start_helper(ConfigMode *cm) {
    if (!cm) return -1;
    write_status(cm, "Starting hotspot");

    pid_t pid = fork();
    if (pid < 0) {
        logf_("CONFIG_MODE helper fork failed: %s", strerror(errno));
        write_status(cm, "Could not start setup helper");
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        setenv("CONFIG_STATUS_PATH", cm->status_path, 1);
        execl("/bin/bash", "bash", "tools/config_mode.sh", "start", (char *)NULL);
        _exit(127);
    }

    cm->helper_pid = pid;
    logf_("CONFIG_MODE helper started pid=%ld", (long)pid);
    return 0;
}

void config_mode_stop_helper(ConfigMode *cm) {
    if (!cm) return;

    if (cm->helper_pid > 0) {
        kill(-cm->helper_pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            int status = 0;
            pid_t rc = waitpid(cm->helper_pid, &status, WNOHANG);
            if (rc == cm->helper_pid || rc < 0) break;
            usleep(50000);
        }
        kill(-cm->helper_pid, SIGKILL);
        waitpid(cm->helper_pid, NULL, WNOHANG);
        cm->helper_pid = 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/sudo", "sudo", "-n", "tools/config_network.sh", "stop-ap", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
    write_status(cm, "Ready");
    logf_("CONFIG_MODE helper stopped");
}

void config_mode_read_status(const ConfigMode *cm, char *dst, size_t dstsz) {
    if (!dst || dstsz == 0) return;
    snprintf(dst, dstsz, "Starting hotspot");
    if (!cm || !cm->status_path[0]) return;

    FILE *f = fopen(cm->status_path, "r");
    if (!f) return;
    if (fgets(dst, (int)dstsz, f)) {
        size_t n = strlen(dst);
        while (n > 0 && (dst[n - 1] == '\n' || dst[n - 1] == '\r'))
            dst[--n] = '\0';
    }
    fclose(f);
}

int config_mode_ap_client_connected(void) {
    const char *env_iface = getenv("CONFIG_WIFI_IFACE");
    const char *iface = (env_iface && *env_iface) ? env_iface : "wlan0";
    char safe_iface[32];
    size_t n = 0;
    for (const char *p = iface; *p && n + 1 < sizeof(safe_iface); p++) {
        unsigned char ch = (unsigned char)*p;
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.'))
            return 0;
        safe_iface[n++] = (char)ch;
    }
    safe_iface[n] = '\0';
    if (!safe_iface[0]) return 0;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "iw dev %s station dump 2>/dev/null", safe_iface);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    char line[128];
    int connected = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Station ", 8) == 0) {
            connected = 1;
            break;
        }
    }
    pclose(fp);
    return connected;
}
