#ifndef SWC_WALLPAPER_H
#define SWC_WALLPAPER_H

struct wl_display;
struct wl_global;
struct wld_buffer;
struct screen;

struct wl_global *swc_wallpaper_manager_create(struct wl_display *display);
struct wld_buffer *swc_wallpaper_buffer_for_screen(struct screen *screen);

#endif
