#include "decor.h"

#include "compositor.h"
#include "swc.h"
#include "window.h"

#include <stdlib.h>
#include <string.h>
#include <wld/wld.h>

#define DEFAULT_DECOR_FONT "sans-serif:size=10"

static struct wld_font_context *font_context;

static uint32_t
decor_title_length(struct wld_font *font, const char *title, uint32_t max_width)
{
	uint32_t len = 0;
	struct wld_extents extents;

	if (!title || !max_width) {
		return 0;
	}

	while (title[len]) {
		uint32_t next = len + 1;

		while ((title[next] & 0xc0) == 0x80) {
			next++;
		}

		wld_font_text_extents_n(font, title, (int32_t)next, &extents);
		if (extents.advance > max_width) {
			break;
		}
		len = next;
	}

	return len;
}

static uint32_t
utf8_next_len(const char *text, uint32_t offset)
{
	uint32_t next = offset + 1;

	while ((text[next] & 0xc0) == 0x80) {
		next++;
	}

	return next - offset;
}

static uint32_t
decor_title_stacked_length(struct wld_font *font, const char *title,
                           uint32_t max_height, uint32_t *glyph_count)
{
	uint32_t len = 0, count = 0;

	if (!title || !font->height) {
		*glyph_count = 0;
		return 0;
	}

	while (title[len] && (count + 1) * font->height <= max_height) {
		len += utf8_next_len(title, len);
		count++;
	}

	*glyph_count = count;
	return len;
}

static bool
streq(const char *a, const char *b)
{
	if (!a || !b) {
		return a == b;
	}
	return strcmp(a, b) == 0;
}

static void
close_decor_font(struct compositor_view *view)
{
	if (view->decor.font) {
		wld_font_close(view->decor.font);
		view->decor.font = NULL;
	}
	free(view->decor.font_name);
	view->decor.font_name = NULL;
}

static void
close_decor_string(struct compositor_view *view)
{
	free(view->decor.string);
	view->decor.string = NULL;
}

bool
decor_initialize(void)
{
	font_context = wld_font_create_context();
	return font_context != NULL;
}

void
decor_finalize(void)
{
	if (font_context) {
		wld_font_destroy_context(font_context);
		font_context = NULL;
	}
}

void
decor_view_initialize(struct compositor_view *view)
{
	memset(&view->decor.text, 0, sizeof(view->decor.text));
	view->decor.string = NULL;
	view->decor.font_name = NULL;
	view->decor.font = NULL;
}

void
decor_view_finalize(struct compositor_view *view)
{
	close_decor_string(view);
	close_decor_font(view);
}

void
decor_repaint(struct wld_renderer *renderer,
              const struct swc_rectangle *target_geom,
              struct compositor_view *view, pixman_region32_t *damage)
{
	const struct swc_rectangle *geom = &view->base.geometry;
	const struct swc_decor_text *text = &view->decor.text;
	struct wld_font *font = view->decor.font;
	const char *title = view->decor.string;
	uint32_t title_len, max_width, decor_size;
	int32_t x, y, base_x, base_y, advance, available_width;
	struct wld_extents extents;
	pixman_region32_t title_region;

	if (!title || !font || !text->enabled) {
		return;
	}

	switch (text->edge) {
	case SWC_DECOR_EDGE_TOP:
		if (!view->decor.top) {
			return;
		}
		base_x = geom->x - (int32_t)view->decor.left;
		base_y = geom->y - (int32_t)view->decor.top;
		max_width = geom->width + view->decor.left + view->decor.right;
		decor_size = view->decor.top;
		break;
	case SWC_DECOR_EDGE_BOTTOM:
		if (!view->decor.bottom) {
			return;
		}
		base_x = geom->x - (int32_t)view->decor.left;
		base_y = geom->y + (int32_t)geom->height;
		max_width = geom->width + view->decor.left + view->decor.right;
		decor_size = view->decor.bottom;
		break;
	case SWC_DECOR_EDGE_LEFT:
		if (!view->decor.left) {
			return;
		}
		base_x = geom->x - (int32_t)view->decor.left;
		base_y = geom->y - (int32_t)view->decor.top;
		max_width = view->decor.left;
		decor_size = geom->height + view->decor.top + view->decor.bottom;
		break;
	case SWC_DECOR_EDGE_RIGHT:
		if (!view->decor.right) {
			return;
		}
		base_x = geom->x + (int32_t)geom->width;
		base_y = geom->y - (int32_t)view->decor.top;
		max_width = view->decor.right;
		decor_size = geom->height + view->decor.top + view->decor.bottom;
		break;
	default:
		return;
	}

	x = base_x;
	y = base_y;

	pixman_region32_init_rect(&title_region, x, y, max_width, decor_size);
	pixman_region32_intersect(&title_region, &title_region, damage);
	pixman_region32_subtract(&title_region, &title_region, &view->clip);
	if (!pixman_region32_not_empty(&title_region)) {
		pixman_region32_fini(&title_region);
		return;
	}
	pixman_region32_fini(&title_region);

	if (max_width <= text->padding * 2 || decor_size < font->height) {
		return;
	}
	max_width -= text->padding * 2;

	if (text->edge == SWC_DECOR_EDGE_LEFT || text->edge == SWC_DECOR_EDGE_RIGHT) {
		uint32_t glyph_count, glyph_offset = 0, available_height, total_height;

		if (decor_size <= text->padding * 2) {
			return;
		}

		available_height = decor_size - text->padding * 2;
		title_len = decor_title_stacked_length(font, title, available_height,
		                                      &glyph_count);
		if (!title_len) {
			return;
		}

		total_height = glyph_count * font->height;
		y += (int32_t)text->padding;
		switch (text->align) {
		case SWC_DECOR_ALIGN_START:
			break;
		case SWC_DECOR_ALIGN_CENTER:
			y += (int32_t)((available_height - total_height) / 2);
			break;
		case SWC_DECOR_ALIGN_END:
			y += (int32_t)(available_height - total_height);
			break;
		}

		while (glyph_offset < title_len) {
			uint32_t glyph_len = utf8_next_len(title, glyph_offset);
			int32_t glyph_x;

			wld_font_text_extents_n(font, title + glyph_offset,
			                        (int32_t)glyph_len, &extents);
			advance = extents.advance > 0 ? extents.advance : 0;
			glyph_x = x + (int32_t)text->padding;
			if ((uint32_t)advance < max_width) {
				glyph_x += ((int32_t)max_width - advance) / 2;
			}

			wld_draw_text(renderer, font, text->color,
			              glyph_x + text->offset_x - target_geom->x,
			              y + (int32_t)font->ascent + text->offset_y -
			                  target_geom->y,
			              title + glyph_offset, glyph_len, NULL);

			glyph_offset += glyph_len;
			y += (int32_t)font->height;
		}

		return;
	}

	title_len = decor_title_length(font, title, max_width);
	if (!title_len) {
		return;
	}
	wld_font_text_extents_n(font, title, (int32_t)title_len, &extents);
	advance = extents.advance > 0 ? extents.advance : 0;
	available_width = (int32_t)max_width;

	switch (text->align) {
	case SWC_DECOR_ALIGN_START:
		x += text->padding;
		break;
	case SWC_DECOR_ALIGN_CENTER:
		x += (int32_t)text->padding + (available_width - advance) / 2;
		break;
	case SWC_DECOR_ALIGN_END:
		x += (int32_t)text->padding + (int32_t)max_width - advance;
		break;
	}

	y += (int32_t)((decor_size - font->height) / 2) + (int32_t)font->ascent;

	x += text->offset_x;
	y += text->offset_y;

	wld_draw_text(renderer, font, text->color, x - target_geom->x,
	              y - target_geom->y, title, title_len, NULL);
}

void
decor_view_set(struct compositor_view *view, const struct swc_decor *decor)
{
	const char *font_name = NULL;
	struct wld_font *font = NULL;
	struct swc_decor_text text = { 0 };
	char *owned_string = NULL;
	char *owned_font_name = NULL;
	uint32_t color = 0, top = 0, right = 0, bottom = 0, left = 0;

	if (!decor) {
		goto apply;
	}

	color = decor->color;
	top = decor->top;
	right = decor->right;
	bottom = decor->bottom;
	left = decor->left;
	text = decor->title;
	if (text.enabled) {
		font_name = text.font ? text.font : DEFAULT_DECOR_FONT;
	}

	if (view->decor.color == color && view->decor.top == top &&
	    view->decor.right == right && view->decor.bottom == bottom &&
	    view->decor.left == left &&
	    view->decor.text.enabled == text.enabled &&
	    view->decor.text.edge == text.edge &&
	    view->decor.text.align == text.align &&
	    streq(view->decor.string, text.string) &&
	    view->decor.text.color == text.color &&
	    view->decor.text.padding == text.padding &&
	    view->decor.text.offset_x == text.offset_x &&
	    view->decor.text.offset_y == text.offset_y &&
	    streq(view->decor.font_name, font_name)) {
		return;
	}

	if (text.string) {
		owned_string = strdup(text.string);
	}

	if (font_name) {
		owned_font_name = strdup(font_name);
		if (owned_font_name && font_context) {
			font = wld_font_open_name(font_context, font_name);
		}
	}

apply:
	view->decor.color = color;
	view->decor.top = top;
	view->decor.right = right;
	view->decor.bottom = bottom;
	view->decor.left = left;
	view->decor.text = text;
	view->decor.text.string = NULL;
	view->decor.text.font = NULL;
	close_decor_string(view);
	view->decor.string = owned_string;
	close_decor_font(view);
	view->decor.font_name = owned_font_name;
	view->decor.font = font;
	view->decor.damaged = true;
}

void
decor_view_damage(struct compositor_view *view)
{
	view->decor.damaged = true;
}
