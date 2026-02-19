#include <wld/wld.h>

#include "compositor.h"
#include "screen.h"
#include "swc.h"
#include "swc_wallpaper-server-protocol.h"
#include "util.h"
#include "wallpaper.h"
#include "wayland_buffer.h"

#define MAX_WALLPAPER_SCREENS 32

static struct wld_buffer *wallbuf = NULL;
static struct wld_buffer *screen_wallbuf[MAX_WALLPAPER_SCREENS];
uint32_t bgcolor = 0xff000000;

static void
set_buffer_slot(struct wld_buffer **slot, struct wld_buffer *buffer)
{
	if (buffer) {
		wld_buffer_reference(buffer);
	}
	if (*slot) {
		wld_buffer_unreference(*slot);
	}

	*slot = buffer;
}

struct wld_buffer *
swc_wallpaper_buffer_for_screen(struct screen *screen)
{
	if (screen && screen->id < ARRAY_LENGTH(screen_wallbuf) &&
	    screen_wallbuf[screen->id]) {
		return screen_wallbuf[screen->id];
	}

	return wallbuf;
}

EXPORT void
swc_wallpaper_set_buffer(struct wld_buffer *buffer)
{
	set_buffer_slot(&wallbuf, buffer);
	compositor_damage_all();
}

EXPORT void
swc_wallpaper_set_buffer_for_screen(uint8_t screen_id,
                                    struct wld_buffer *buffer)
{
	if (screen_id >= ARRAY_LENGTH(screen_wallbuf)) {
		return;
	}

	set_buffer_slot(&screen_wallbuf[screen_id], buffer);
	compositor_damage_all();
}

EXPORT void
swc_wallpaper_color_set(uint32_t color)
{
	bgcolor = color;
	compositor_damage_all();
}

static void
set_buffer(struct wl_client *client, struct wl_resource *resource,
           int32_t screen_id, struct wl_resource *buffer_resource)
{
	struct wld_buffer *buffer = NULL;

	(void)client;

	if (buffer_resource) {
		buffer = wayland_buffer_get(buffer_resource);
		if (!buffer) {
			wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			                       "buffer is not a wl_buffer");
			return;
		}
	}

	if (screen_id == -1) {
		swc_wallpaper_set_buffer(buffer);
		return;
	}

	if (screen_id < 0 || screen_id >= ARRAY_LENGTH(screen_wallbuf)) {
		return;
	}

	swc_wallpaper_set_buffer_for_screen((uint8_t)screen_id, buffer);
}

static const struct swc_wallpaper_interface wallpaper_impl = {
    .destroy = destroy_resource,
    .set_buffer = set_buffer,
};

static void
bind_wallpaper(struct wl_client *client, void *data, uint32_t version,
               uint32_t id)
{
	(void)data;

	struct wl_resource *resource;

	resource =
	    wl_resource_create(client, &swc_wallpaper_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &wallpaper_impl, NULL, NULL);
}

struct wl_global *
swc_wallpaper_manager_create(struct wl_display *display)
{
	return wl_global_create(display, &swc_wallpaper_interface, 1, NULL,
	                        bind_wallpaper);
}
