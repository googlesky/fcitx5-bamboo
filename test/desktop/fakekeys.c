/*
 * SPDX-FileCopyrightText: 2026 googlesky
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
// Sends evdev key events to a KWin through org_kde_kwin_fake_input, which
// needs KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 in a nested KWin.
// Arguments: d<code> key down, u<code> key up, w<ms> wait.
#include "fake-input-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

static struct org_kde_kwin_fake_input *fake;

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0) {
        fake =
            wl_registry_bind(registry, name, &org_kde_kwin_fake_input_interface,
                             version < 4 ? version : 4);
    }
}

static void global_remove(void *data, struct wl_registry *registry,
                          uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener listener = {global, global_remove};

int main(int argc, char **argv) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "no wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, NULL);
    wl_display_roundtrip(display);
    if (!fake) {
        fprintf(stderr, "no fake input\n");
        return 1;
    }
    org_kde_kwin_fake_input_authenticate(fake, "imetest", "input method test");
    wl_display_roundtrip(display);
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        long value = strtol(arg + 1, NULL, 10);
        if (arg[0] == 'd' || arg[0] == 'u') {
            org_kde_kwin_fake_input_keyboard_key(fake, (uint32_t)value,
                                                 arg[0] == 'd');
            wl_display_flush(display);
        } else if (arg[0] == 'w') {
            wl_display_roundtrip(display);
            struct timespec ts = {value / 1000, (value % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
    }
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
