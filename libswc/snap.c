#include "snap.h"
#include "internal.h"
#include "screen.h"
#include "compositor.h"
#include "shm.h"
#include "seat.h"
#include "pointer.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wld/wld.h>
#include "swc_snap-server-protocol.h"

static void
ppm(int fd, const uint8_t *pixels, uint32_t width, uint32_t height,
    uint32_t pitch)
{
	FILE *f = fdopen(fd, "wb");
	if (!f) {
		close(fd);
		return;
	}

	/* ppm  header */
	fprintf(f, "P6\n%u %u\n255\n", width, height);

	/* pixel data convert argb8888 to rgb) */
	for (uint32_t y = 0; y < height; y++) {
		const uint32_t *row = (const uint32_t *)(pixels + ((size_t)y * pitch));

		for (uint32_t x = 0; x < width; x++) {
			uint32_t pixel = row[x];
			unsigned char rgb[3] = {
				(pixel >> 16) & 0xFF,  
				(pixel >> 8) & 0xFF,   
				pixel & 0xFF           
			};
			fwrite(rgb, 1, 3, f);
		}
	}

	fclose(f); 
}


/* get cursor */
static void
cursor(uint8_t *dst, uint32_t dst_width, uint32_t dst_height,
       uint32_t dst_pitch, struct screen *screen)
{
	struct pointer *pointer = swc.seat ? swc.seat->pointer : NULL;
	struct wld_buffer *cursor_buf;
	const uint8_t *src;
	int32_t dst_x, dst_y;
	int32_t src_x = 0, src_y = 0;
	uint32_t copy_w, copy_h;

	if (!pointer || !pointer->cursor.buffer || !pointer->cursor.view.buffer)
		return;

	if (!(pointer->cursor.view.screens & screen_mask(screen)))
		return;

	cursor_buf = pointer->cursor.buffer;
	if (!wld_map(cursor_buf) || !cursor_buf->map)
		return;

	dst_x = pointer->cursor.view.geometry.x - screen->base.geometry.x;
	dst_y = pointer->cursor.view.geometry.y - screen->base.geometry.y;

	if (dst_x >= (int32_t)dst_width || dst_y >= (int32_t)dst_height ||
	    dst_x + (int32_t)cursor_buf->width <= 0 ||
	    dst_y + (int32_t)cursor_buf->height <= 0) {
		wld_unmap(cursor_buf);
		return;
	}

	if (dst_x < 0) {
		src_x = -dst_x;
		dst_x = 0;
	}
	if (dst_y < 0) {
		src_y = -dst_y;
		dst_y = 0;
	}

	copy_w = cursor_buf->width - (uint32_t)src_x;
	if (copy_w > dst_width - (uint32_t)dst_x)
		copy_w = dst_width - (uint32_t)dst_x;
	copy_h = cursor_buf->height - (uint32_t)src_y;
	if (copy_h > dst_height - (uint32_t)dst_y)
		copy_h = dst_height - (uint32_t)dst_y;

	src = cursor_buf->map;

	for (uint32_t y = 0; y < copy_h; y++) {
		const uint32_t *src_row = (const uint32_t *)(src + ((size_t)(src_y + (int32_t)y) * cursor_buf->pitch)) + src_x;
		uint32_t *dst_row = (uint32_t *)(dst + ((size_t)(dst_y + (int32_t)y) * dst_pitch)) + dst_x;

		for (uint32_t x = 0; x < copy_w; x++) {
			uint32_t src_px = src_row[x];
			uint32_t a = src_px >> 24;

			if (a == 0)
				continue;
			if (a == 255) {
				dst_row[x] = 0xFF000000 | (src_px & 0x00FFFFFF);
				continue;
			}

			uint32_t dst_px = dst_row[x];
			uint32_t inv = 255 - a;
			uint32_t r = ((src_px >> 16) & 0xFF) + ((((dst_px >> 16) & 0xFF) * inv + 127) / 255);
			uint32_t g = ((src_px >> 8) & 0xFF) + ((((dst_px >> 8) & 0xFF) * inv + 127) / 255);
			uint32_t b = (src_px & 0xFF) + (((dst_px & 0xFF) * inv + 127) / 255);

			dst_row[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
		}
	}

	wld_unmap(cursor_buf);
}

static void
capture(struct wl_client *client, struct wl_resource *resource, int32_t fd)
{
	struct screen *screen;
	struct wld_buffer *shm_buffer;
	uint8_t *pixels;
	uint32_t width, height;

	if (wl_list_empty(&swc.screens)) {
		fprintf(stderr, "snap: no screens available\n");
		close(fd);
		return;
	}

	screen = wl_container_of(swc.screens.next, screen, link);
	width = screen->base.geometry.width;
	height = screen->base.geometry.height;

	/* put compositor in shm*/
	shm_buffer = compositor_render_to_shm(screen);
	if (!shm_buffer) {
		fprintf(stderr, "snap: failed to render to SHM\n");
		close(fd);
		return;
	}

	/* get pixel data from shm */
	if (!wld_map(shm_buffer) || !shm_buffer->map) {
		fprintf(stderr, "snap: failed to map buffer data\n");
		wld_buffer_unreference(shm_buffer);
		close(fd);
		return;
	}

	pixels = shm_buffer->map;

	cursor(pixels, width, height, shm_buffer->pitch, screen);

	ppm(fd, pixels, width, height, shm_buffer->pitch);

	wld_unmap(shm_buffer);
	wld_buffer_unreference(shm_buffer);
}

static const struct swc_snap_interface snap_impl = {
	.capture = capture,
};

static void
bind_snap(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &swc_snap_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &snap_impl, NULL, NULL);
}

struct wl_global *
snap_manager_create(struct wl_display *display)
{
	return wl_global_create(display, &swc_snap_interface, 1, NULL, &bind_snap);
}
