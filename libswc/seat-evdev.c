#include "compositor.h"
#include "data_device.h"
#include "event.h"
#include "internal.h"
#include "keyboard.h"
#include "launch.h"
#include "pointer.h"
#include "screen.h"
#include "seat.h"
#include "surface.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>

struct seat {
	struct swc_seat base;

	char *name;
	uint32_t capabilities;

	int mouse_fd;
	int kbd_fd;
	bool ignore;

	struct xkb_rule_names names;

	struct wl_event_source *mouse_source;
	struct wl_event_source *kbd_source;

	struct wl_listener swc_listener;

	struct wl_listener keyboard_focus_listener;
	struct pointer pointer;
	struct wl_listener data_device_listener;

	struct wl_list resources;

	wl_fixed_t abs_x;
	wl_fixed_t abs_y;
	bool abs_initialized;

	bool shared_fd;
};

static void
handle_keyboard_focus_event(struct wl_listener *listener, void *data)
{
	struct seat *seat =
	    wl_container_of(listener, seat, keyboard_focus_listener);
	struct event *ev = data;
	struct input_focus_event_data *event_data = ev->data;

	if (ev->type != INPUT_FOCUS_EVENT_CHANGED) {
		return;
	}

	if (event_data->new) {
		struct wl_client *client =
		    wl_resource_get_client(event_data->new->surface->resource);

		/* offer the selection to the new focus */
		data_device_offer_selection(seat->base.data_device, client);
	}
}

static void
handle_data_device_event(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, data_device_listener);
	struct event *ev = data;

	if (ev->type != DATA_DEVICE_EVENT_SELECTION_CHANGED) {
		return;
	}

	if (seat->base.keyboard->focus.client) {
		data_device_offer_selection(seat->base.data_device,
		                            seat->base.keyboard->focus.client);
	}
}

static void
handle_swc_event(struct wl_listener *listener, void *data)
{
	struct seat *seat = wl_container_of(listener, seat, swc_listener);
	struct event *ev = data;

	switch (ev->type) {
	case SWC_EVENT_DEACTIVATED:
		seat->ignore = true;
		keyboard_reset(seat->base.keyboard);
		break;
	case SWC_EVENT_ACTIVATED:
		seat->ignore = false;
		break;
	}
}

/* da seat */
static void
get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct seat *seat = wl_resource_get_user_data(resource);

	pointer_bind(&seat->pointer, client, wl_resource_get_version(resource), id);
}

static void
get_keyboard(struct wl_client *client, struct wl_resource *resource,
             uint32_t id)
{
	struct seat *seat = wl_resource_get_user_data(resource);

	keyboard_bind(seat->base.keyboard, client,
	              wl_resource_get_version(resource), id);
}

static void
get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
}

static struct wl_seat_interface seat_impl = {
    .get_pointer = get_pointer,
    .get_keyboard = get_keyboard,
    .get_touch = get_touch,
};

static void
bind_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct seat *seat = data;
	struct wl_resource *resource;

	if (version > 4) {
		version = 4;
	}

	resource = wl_resource_create(client, &wl_seat_interface, version, id);
	wl_resource_set_implementation(resource, &seat_impl, seat,
	                               &remove_resource);
	wl_list_insert(&seat->resources, wl_resource_get_link(resource));

	if (version >= 2) {
		wl_seat_send_name(resource, seat->name);
	}

	wl_seat_send_capabilities(resource, seat->capabilities);
}

static uint32_t
event_time_ms(const struct input_event *ev)
{
	return (uint32_t)(ev->time.tv_sec * 1000 + ev->time.tv_usec / 1000);
}

static void
handle_evdev_key(struct seat *seat, const struct input_event *ev)
{
	uint32_t state;
	uint32_t time = event_time_ms(ev);

	if (ev->value == 2) {
		return;
	}

	if (ev->code >= BTN_MISC) {
		pointer_handle_button(seat->base.pointer, time, ev->code,
		                      ev->value ? WL_POINTER_BUTTON_STATE_PRESSED
		                                : WL_POINTER_BUTTON_STATE_RELEASED);
	} else {
		if (ev->code > 255) {
			return;
		}
		state = (ev->value ? WL_KEYBOARD_KEY_STATE_PRESSED
		                   : WL_KEYBOARD_KEY_STATE_RELEASED);
		keyboard_handle_key(seat->base.keyboard, time, ev->code, state);
	}
}

static void
handle_evdev_rel(struct seat *seat, const struct input_event *ev)
{
	uint32_t time = event_time_ms(ev);
	wl_fixed_t value;

	switch (ev->code) {
	case REL_X:
		pointer_handle_relative_motion(seat->base.pointer, time,
		                               wl_fixed_from_int(ev->value), 0);
		break;
	case REL_Y:
		pointer_handle_relative_motion(seat->base.pointer, time, 0,
		                               wl_fixed_from_int(ev->value));
		break;
	case REL_WHEEL:
		value = wl_fixed_from_int(ev->value * 10);
		pointer_handle_axis(
		    seat->base.pointer, time, WL_POINTER_AXIS_VERTICAL_SCROLL,
		    WL_POINTER_AXIS_SOURCE_WHEEL, value, ev->value * 120);
		break;
	case REL_HWHEEL:
		value = wl_fixed_from_int(ev->value * 10);
		pointer_handle_axis(
		    seat->base.pointer, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
		    WL_POINTER_AXIS_SOURCE_WHEEL, value, ev->value * 120);
		break;
	default:
		break;
	}
}

static void
handle_evdev_abs(struct seat *seat, const struct input_event *ev)
{
	uint32_t time = event_time_ms(ev);

	switch (ev->code) {
	case ABS_X:
		seat->abs_x = wl_fixed_from_int(ev->value);
		seat->abs_initialized = true;
		break;
	case ABS_Y:
		seat->abs_y = wl_fixed_from_int(ev->value);
		seat->abs_initialized = true;
		break;
	default:
		return;
	}

	if (seat->abs_initialized) {
		pointer_handle_absolute_motion(seat->base.pointer, time, seat->abs_x,
		                               seat->abs_y);
	}
}

static int
handle_evdev_data(int fd, uint32_t mask, void *data)
{
	struct seat *seat = data;
	struct input_event ev;
	ssize_t n;

	while (!seat->ignore) {
		n = read(fd, &ev, sizeof(ev));
		if (n == -1) {
			if (errno == EAGAIN || errno == EINTR) {
				break;
			}
			return 0;
		}
		if (n != (ssize_t)sizeof(ev)) {
			break;
		}

		switch (ev.type) {
		case EV_KEY:
			handle_evdev_key(seat, &ev);
			break;
		case EV_REL:
			handle_evdev_rel(seat, &ev);
			break;
		case EV_ABS:
			handle_evdev_abs(seat, &ev);
			break;
		case EV_SYN:
			if (ev.code == SYN_REPORT) {
				pointer_handle_frame(seat->base.pointer);
			}
			break;
		default:
			break;
		}
	}

	return 0;
}

static bool
test_bit(const unsigned long *bits, size_t bit)
{
	return (bits[bit / (sizeof(unsigned long) * 8)] >>
	        (bit % (sizeof(unsigned long) * 8))) &
	       1;
}

static bool
contains_ci(const char *haystack, const char *needle)
{
	size_t nlen;
	const char *h;

	if (!haystack || !needle || !*needle) {
		return false;
	}

	nlen = strlen(needle);
	for (h = haystack; *h; ++h) {
		size_t i;
		for (i = 0; i < nlen; ++i) {
			unsigned char hc = (unsigned char)h[i];
			unsigned char nc = (unsigned char)needle[i];
			if (!h[i] || tolower(hc) != tolower(nc)) {
				break;
			}
		}
		if (i == nlen) {
			return true;
		}
	}
	return false;
}

static bool
is_keyboard_device(int fd)
{
	unsigned long ev_bits[(EV_MAX + 8 * sizeof(unsigned long) - 1) /
	                      (8 * sizeof(unsigned long))];
	unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long) - 1) /
	                       (8 * sizeof(unsigned long))];

	memset(ev_bits, 0, sizeof(ev_bits));
	memset(key_bits, 0, sizeof(key_bits));

	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
		return false;
	}
	if (!test_bit(ev_bits, EV_KEY)) {
		return false;
	}
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
		return false;
	}

	return test_bit(key_bits, KEY_A) && test_bit(key_bits, KEY_Z) &&
	       test_bit(key_bits, KEY_ENTER) && test_bit(key_bits, KEY_ESC) &&
	       test_bit(key_bits, KEY_SPACE);
}

static bool
is_pointer_device(int fd)
{
	unsigned long ev_bits[(EV_MAX + 8 * sizeof(unsigned long) - 1) /
	                      (8 * sizeof(unsigned long))];
	unsigned long rel_bits[(REL_MAX + 8 * sizeof(unsigned long) - 1) /
	                       (8 * sizeof(unsigned long))];
	unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long) - 1) /
	                       (8 * sizeof(unsigned long))];

	memset(ev_bits, 0, sizeof(ev_bits));
	memset(rel_bits, 0, sizeof(rel_bits));
	memset(key_bits, 0, sizeof(key_bits));

	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
		return false;
	}

	if (test_bit(ev_bits, EV_REL)) {
		if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) < 0) {
			return false;
		}
		if (test_bit(rel_bits, REL_X) && test_bit(rel_bits, REL_Y)) {
			return true;
		}
	}

	if (test_bit(ev_bits, EV_KEY)) {
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
			return false;
		}
		if (test_bit(key_bits, BTN_LEFT) && test_bit(key_bits, BTN_RIGHT)) {
			return true;
		}
	}

	return false;
}

static bool
get_ev_bits(int fd, unsigned long *ev_bits, size_t ev_bits_len)
{
	memset(ev_bits, 0, ev_bits_len);
	return ioctl(fd, EVIOCGBIT(0, ev_bits_len), ev_bits) >= 0;
}

static int
score_candidate(int fd, bool want_keyboard, const char *id_name)
{
	char name[256];
	unsigned long ev_bits[(EV_MAX + 8 * sizeof(unsigned long) - 1) /
	                      (8 * sizeof(unsigned long))];
	bool is_kbd;
	bool is_ptr;
	int score = 10;

	is_kbd = is_keyboard_device(fd);
	is_ptr = is_pointer_device(fd);

	if (want_keyboard && !is_kbd) {
		return -1;
	}
	if (!want_keyboard && !is_ptr) {
		return -1;
	}

	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
		name[0] = '\0';
	}

	if (!get_ev_bits(fd, ev_bits, sizeof(ev_bits))) {
		memset(ev_bits, 0, sizeof(ev_bits));
	}

	if (want_keyboard) {
		if (is_ptr) {
			score -= 6;
		}
		if (contains_ci(id_name, "mouse") || contains_ci(name, "mouse")) {
			score -= 12;
		}
		if (contains_ci(id_name, "kbd") || contains_ci(id_name, "keyboard")) {
			score += 4;
		}
		if (contains_ci(name, "keyboard")) {
			score += 2;
		}
		if (test_bit(ev_bits, EV_LED)) {
			score += 3;
		}
		if (test_bit(ev_bits, EV_REP)) {
			score += 1;
		}
	} else {
		if (contains_ci(id_name, "mouse") || contains_ci(name, "mouse")) {
			score += 4;
		}
		if (contains_ci(id_name, "kbd") || contains_ci(id_name, "keyboard")) {
			score -= 6;
		}
		if (contains_ci(name, "keyboard")) {
			score -= 4;
		}
	}

	return score;
}

static bool
pick_best_device(const char *dir_path, const char *name_prefix,
                 const char *name_substr, bool want_keyboard, char *out,
                 size_t out_len)
{
	DIR *dir;
	struct dirent *ent;
	bool found = false;
	int best_score = -1;
	size_t prefix_len = name_prefix ? strlen(name_prefix) : 0;

	dir = opendir(dir_path);
	if (!dir) {
		return false;
	}

	while ((ent = readdir(dir)) != NULL) {
		char path[PATH_MAX];
		int fd;
		int score;

		if (ent->d_name[0] == '.') {
			continue;
		}
		if (name_prefix && strncmp(ent->d_name, name_prefix, prefix_len) != 0) {
			continue;
		}
		if (name_substr && !strstr(ent->d_name, name_substr)) {
			continue;
		}

		snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
		fd = launch_open_device(path, O_RDONLY | O_NONBLOCK);
		if (fd == -1) {
			continue;
		}

		score = score_candidate(fd, want_keyboard, ent->d_name);
		if (score < 0) {
			close(fd);
			continue;
		}

		if (score > best_score) {
			snprintf(out, out_len, "%s", path);
			best_score = score;
			found = true;
		}

		close(fd);
	}

	closedir(dir);
	return found;
}

static bool
initialize_evdev(struct seat *seat)
{
	char kbd_path[PATH_MAX];
	char mouse_path[PATH_MAX];
	const char *kbd_dev = EVDEV_KBD_DEVICE;
	const char *mouse_dev = EVDEV_POINTER_DEVICE;

	if (pick_best_device("/dev/input/by-id", NULL, "event-kbd", true, kbd_path,
	                     sizeof(kbd_path))) {
		kbd_dev = kbd_path;
	} else if (pick_best_device("/dev/input/by-path", NULL, "event-kbd", true,
	                            kbd_path, sizeof(kbd_path))) {
		kbd_dev = kbd_path;
	} else if (pick_best_device("/dev/input", "event", NULL, true, kbd_path,
	                            sizeof(kbd_path))) {
		kbd_dev = kbd_path;
	}

	if (pick_best_device("/dev/input/by-id", NULL, "event-mouse", false,
	                     mouse_path, sizeof(mouse_path))) {
		mouse_dev = mouse_path;
	} else if (pick_best_device("/dev/input/by-path", NULL, "event-mouse",
	                            false, mouse_path, sizeof(mouse_path))) {
		mouse_dev = mouse_path;
	} else if (pick_best_device("/dev/input", "event", NULL, false, mouse_path,
	                            sizeof(mouse_path))) {
		mouse_dev = mouse_path;
	}

	DEBUG("evdev devices: keyboard=%s pointer=%s\n", kbd_dev, mouse_dev);

	seat->kbd_fd = launch_open_device(kbd_dev, O_RDONLY | O_NONBLOCK);
	if (seat->kbd_fd == -1) {
		ERROR("Could not open evdev keyboard device %s\n", kbd_dev);
		goto error0;
	}

	if (strcmp(kbd_dev, mouse_dev) == 0) {
		seat->mouse_fd = seat->kbd_fd;
		seat->shared_fd = true;
		return true;
	}

	seat->mouse_fd = launch_open_device(mouse_dev, O_RDONLY | O_NONBLOCK);
	if (seat->mouse_fd == -1) {
		ERROR("Could not open evdev pointer device %s\n", mouse_dev);
		goto error1;
	}

	return true;

error1:
	close(seat->kbd_fd);
error0:
	return false;
}

struct swc_seat *
seat_create(struct wl_display *display, const char *seat_name)
{
	struct seat *seat;

	seat = malloc(sizeof(*seat));
	if (!seat) {
		goto error0;
	}

	memset(&seat->names, 0, sizeof(seat->names));
	seat->names.rules = "base";
	seat->names.model = "pc105";
	seat->names.layout = "us";
	seat->names.variant = "basic";
	seat->shared_fd = false;

	seat->name = strdup(seat_name);
	if (!seat->name) {
		ERROR("Could not allocate seat name string\n");
		goto error1;
	}

	if (!initialize_evdev(seat)) {
		goto error2;
	}

	seat->base.global =
	    wl_global_create(display, &wl_seat_interface, 4, seat, &bind_seat);
	if (!seat->base.global) {
		goto error2;
	}
	seat->capabilities =
	    WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER;
	wl_list_init(&seat->resources);

	seat->swc_listener.notify = &handle_swc_event;
	wl_signal_add(&swc.event_signal, &seat->swc_listener);

	seat->base.data_device = data_device_create();
	if (!seat->base.data_device) {
		ERROR("could not initialize data device\n");
		goto error3;
	}
	seat->data_device_listener.notify = &handle_data_device_event;
	wl_signal_add(&seat->base.data_device->event_signal,
	              &seat->data_device_listener);

	seat->base.keyboard = keyboard_create(&seat->names);
	if (!seat->base.keyboard) {
		ERROR("could not initialize keyboard\n");
		goto error4;
	}
	seat->keyboard_focus_listener.notify = handle_keyboard_focus_event;
	wl_signal_add(&seat->base.keyboard->focus.event_signal,
	              &seat->keyboard_focus_listener);

	if (!pointer_initialize(&seat->pointer)) {
		ERROR("Could not initialize pointer\n");
		goto error5;
	}
	seat->base.pointer = &seat->pointer;

	seat->kbd_source =
	    wl_event_loop_add_fd(swc.event_loop, seat->kbd_fd, WL_EVENT_READABLE,
	                         &handle_evdev_data, seat);
	if (!seat->shared_fd) {
		seat->mouse_source =
		    wl_event_loop_add_fd(swc.event_loop, seat->mouse_fd,
		                         WL_EVENT_READABLE, &handle_evdev_data, seat);
	} else {
		seat->mouse_source = NULL;
	}

	seat->abs_initialized = false;

	return &seat->base;

error5:
	keyboard_destroy(seat->base.keyboard);
error4:
	data_device_destroy(seat->base.data_device);
error3:
	wl_global_destroy(seat->base.global);
error2:
	free(seat->name);
error1:
	free(seat);
error0:
	return NULL;
}

void
seat_destroy(struct swc_seat *seat_base)
{
	struct seat *seat = wl_container_of(seat_base, seat, base);

	if (seat->mouse_source) {
		wl_event_source_remove(seat->mouse_source);
	}
	wl_event_source_remove(seat->kbd_source);
	if (seat->mouse_source) {
		close(seat->mouse_fd);
		seat->mouse_fd = -1;
	}
	close(seat->kbd_fd);
	seat->kbd_fd = -1;

	pointer_finalize(&seat->pointer);
	keyboard_destroy(seat->base.keyboard);
	data_device_destroy(seat->base.data_device);

	wl_global_destroy(seat->base.global);
	free(seat->name);
	free(seat);
}
