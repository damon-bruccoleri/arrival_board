/*
 * Phone-based configuration mode: GPIO trigger, helper process launch, status.
 */
#pragma once

#include <stddef.h>
#include <sys/types.h>

typedef struct ConfigMode {
    void *chip;
    void *request;
    int gpio_ready;
    int last_value;
    unsigned int stable_count;
    pid_t helper_pid;
    char status_path[256];
} ConfigMode;

int  config_mode_init(ConfigMode *cm);
void config_mode_destroy(ConfigMode *cm);

/* Returns 1 once for a debounced GPIO13 press (active low), otherwise 0. */
int  config_mode_poll_pressed(ConfigMode *cm);

/* Starts the hotspot/web portal helper. Returns 0 on launch success. */
int  config_mode_start_helper(ConfigMode *cm);

/* Stops the setup helper and tears down the setup hotspot. */
void config_mode_stop_helper(ConfigMode *cm);

/* Reads a short status string from the helper. */
void config_mode_read_status(const ConfigMode *cm, char *dst, size_t dstsz);

/* Returns 1 when a phone/client is associated with the setup AP. */
int  config_mode_ap_client_connected(void);
