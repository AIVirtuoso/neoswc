/* swc: libswc/seat_ws_openbsd.c
 *
 * Copyright (c) 2013, 2014 Michael Forney
 * Copyright (c) 2019 Nia Alarie
 * Copyright (c) 2015 Martin Pieuchot <mpi@openbsd.org>
 *
 * Credits to libopeninput for parts of implementation:
 * https://github.com/sizeofvoid/libopeninput
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "internal.h"
#include "keyboard.h"
#include "launch.h"
#include "util.h"
#include <dev/wscons/wsconsio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wayland-server.h>

struct ws_keyboards {
	struct wl_list devices;
	struct keyboard **keyboard;
	const bool *ignore;
	unsigned key_refs[256];
};

struct ws_keyboard {
	struct wl_list link;
	struct ws_keyboards *keyboards;
	struct wl_event_source *source;
	int fd;
	unsigned type;
	bool held[256];
};

int
ws_to_xkb(unsigned type, int key);

static void
release_keys(struct ws_keyboard *kbd)
{
	struct ws_keyboards *keyboards = kbd->keyboards;

	for (unsigned key = 0; key < 256; ++key) {
		if (!kbd->held[key]) {
			continue;
		}
		kbd->held[key] = false;
		if (--keyboards->key_refs[key] == 0 && *keyboards->keyboard) {
			keyboard_handle_key(*keyboards->keyboard, get_time(), key,
			                    WL_KEYBOARD_KEY_STATE_RELEASED);
		}
	}
}

static void
close_keyboard(struct ws_keyboard *kbd)
{
	if (kbd->source) {
		wl_event_source_remove(kbd->source);
		kbd->source = NULL;
	}
	if (kbd->fd >= 0) {
		release_keys(kbd);
		close(kbd->fd);
		kbd->fd = -1;
	}
}

static int
handle_keyboard_data(int fd, uint32_t mask, void *data)
{
	struct ws_keyboard *kbd = data;
	struct ws_keyboards *keyboards = kbd->keyboards;
	struct wscons_event ev;
	ssize_t n;
	int key;
	uint32_t time;

	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		close_keyboard(kbd);
		return 0;
	}

	while (!*keyboards->ignore) {
		n = read(fd, &ev, sizeof(ev));
		if (n != sizeof(ev)) {
			if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				close_keyboard(kbd);
			}
			break;
		}
		if (ev.type == WSCONS_EVENT_ALL_KEYS_UP) {
			release_keys(kbd);
			continue;
		}
		if (ev.type != WSCONS_EVENT_KEY_DOWN &&
		    ev.type != WSCONS_EVENT_KEY_UP) {
			continue;
		}
		key = ws_to_xkb(kbd->type, ev.value);
		if (key < 0) {
			continue;
		}
		time =
		    (uint32_t)(ev.time.tv_sec * 1000ULL + ev.time.tv_nsec / 1000000L);
		if (ev.type == WSCONS_EVENT_KEY_DOWN && !kbd->held[key]) {
			kbd->held[key] = true;
			if (++keyboards->key_refs[key] == 1) {
				keyboard_handle_key(*keyboards->keyboard, time, key,
				                    WL_KEYBOARD_KEY_STATE_PRESSED);
			}
		} else if (ev.type == WSCONS_EVENT_KEY_UP && kbd->held[key]) {
			kbd->held[key] = false;
			if (--keyboards->key_refs[key] == 0) {
				keyboard_handle_key(*keyboards->keyboard, time, key,
				                    WL_KEYBOARD_KEY_STATE_RELEASED);
			}
		}
	}
	return 0;
}

void
ws_keyboards_reset(struct ws_keyboards *keyboards)
{
	struct ws_keyboard *kbd;

	memset(keyboards->key_refs, 0, sizeof(keyboards->key_refs));
	wl_list_for_each(kbd, &keyboards->devices, link)
	    memset(kbd->held, 0, sizeof(kbd->held));
}

void
ws_keyboards_close(struct ws_keyboards *keyboards)
{
	struct ws_keyboard *kbd, *next;

	if (!keyboards) {
		return;
	}
	wl_list_for_each_safe(kbd, next, &keyboards->devices, link)
	{
		close_keyboard(kbd);
		wl_list_remove(&kbd->link);
		free(kbd);
	}
	free(keyboards);
}

struct ws_keyboards *
ws_keyboards_open(struct keyboard **keyboard, const bool *ignore,
                  kbd_t *encoding)
{
	struct ws_keyboards *keyboards;
	struct ws_keyboard *kbd;
	char path[32];

	keyboards = calloc(1, sizeof(*keyboards));
	if (!keyboards) {
		return NULL;
	}
	wl_list_init(&keyboards->devices);
	keyboards->keyboard = keyboard;
	keyboards->ignore = ignore;

	for (int i = 0; i < 10; ++i) {
		snprintf(path, sizeof(path), "/dev/wskbd%d", i);
		kbd = calloc(1, sizeof(*kbd));
		if (!kbd) {
			break;
		}
		kbd->keyboards = keyboards;
		kbd->fd = launch_open_device(path, O_RDWR | O_NONBLOCK);
		if (kbd->fd < 0) {
			free(kbd);
			continue;
		}
		if (ioctl(kbd->fd, WSKBDIO_GTYPE, &kbd->type) == -1 ||
		    (kbd->type != WSKBD_TYPE_PC_XT && kbd->type != WSKBD_TYPE_PC_AT &&
		     kbd->type != WSKBD_TYPE_USB)) {
			close_keyboard(kbd);
			free(kbd);
			continue;
		}
		if (!*encoding) {
			kbd_t layout;
			if (ioctl(kbd->fd, WSKBDIO_GETENCODING, &layout) == 0) {
				*encoding = layout;
			}
		}

		kbd->source =
		    wl_event_loop_add_fd(swc.event_loop, kbd->fd, WL_EVENT_READABLE,
		                         handle_keyboard_data, kbd);
		if (!kbd->source) {
			close_keyboard(kbd);
			free(kbd);
			continue;
		}
		wl_list_insert(&keyboards->devices, &kbd->link);
		fprintf(stderr, "wscons: opened %s (type %u)\n", path, kbd->type);
	}
	if (wl_list_empty(&keyboards->devices)) {
		ERROR("Could not open any keyboard device\n");
		ws_keyboards_close(keyboards);
		return NULL;
	}
	return keyboards;
}
