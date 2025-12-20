#include <pixman.h>
#include <wld/wld.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#include "../stb/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../stb/stb_image_resize2.h"

#include "swc.h"
#include "internal.h"
#include "drm.h"
#include "util.h"
#include "shm.h"
#include "screen.h"

unsigned char *wallpaper = NULL;
struct wld_buffer *wallbuf = NULL;

uint32_t bgcolor = 0xff000000;

EXPORT void
swc_wallpaper_init(char* path)
{
	int width, height, chan;
	unsigned char *loaded;
	struct screen *screen;
	int target_width = 0, target_height = 0;

	loaded = stbi_load(path, &width, &height, &chan, 4);
	if (!loaded)
		return;

	/* get screen dimensions */
	wl_list_for_each(screen, &swc.screens, link) {
		target_width = screen->base.geometry.width;
		target_height = screen->base.geometry.height;
		break;
	}

	/* If we have a screen and dimensions wrong  scale  */
	if (target_width > 0 && target_height > 0 &&
	    (width != target_width || height != target_height)) {
		wallpaper = stbir_resize_uint8_srgb(loaded, width, height, 0,
		                                     NULL, target_width, target_height, 0,
		                                     STBIR_RGBA);
		stbi_image_free(loaded);
		width = target_width;
		height = target_height;
	} else {
		wallpaper = loaded;
	}

	/* swap color channels to be compatible */
	for(int i = 0; i < width * height; i++) {
		unsigned char r = wallpaper[i*4];
		wallpaper[i*4] = wallpaper[(i*4)+2];
		wallpaper[(i*4)+2] = r;
	}

	union wld_object obj;
	obj.ptr = (uint32_t*)wallpaper;

	wallbuf = wld_import_buffer(swc.shm->context,
			WLD_OBJECT_DATA,
			obj,
			width, height,
			WLD_FORMAT_ARGB8888,
			width * 4);
}

EXPORT void
swc_wallpaper_color_set(uint32_t color)
{
	bgcolor = color;
}
