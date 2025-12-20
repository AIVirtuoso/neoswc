#include <pixman.h>
#include <wld/wld.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#include "../stb/stb_image.h"

#include "swc.h"
#include "internal.h"
#include "drm.h"
#include "util.h"
#include "shm.h"

unsigned char *wallpaper = NULL;
struct wld_buffer *wallbuf = NULL;

uint32_t bgcolor = 0xff000000;

EXPORT void
swc_wallpaper_init(char* path)
{
	int width, height, chan;

	wallpaper = stbi_load(path, &width, &height, &chan, 4);

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
