/* swc: data.h
 *
 * Copyright (c) 2013 Michael Forney
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef SWC_DATA_H
#define SWC_DATA_H

#include <stdbool.h>
#include <stdint.h>

struct wl_client;
struct wl_resource;

struct wl_resource *
data_source_new(struct wl_client *client, uint32_t version, uint32_t id);
struct wl_resource *
data_offer_new(struct wl_client *client, struct wl_resource *source,
               uint32_t version);
void
data_send_mime_types(struct wl_resource *source, struct wl_resource *offer);

/*
 * The ext-data-control-v1 halves. Same backing store as the wl_data_source
 * ones above, so a selection set through either protocol reads through both.
 */
struct wl_resource *
data_control_source_new(struct wl_client *client, uint32_t version,
                        uint32_t id);
struct wl_resource *
data_control_offer_new(struct wl_client *client, struct wl_resource *source,
                       uint32_t version);
void
data_control_send_mime_types(struct wl_resource *source,
                             struct wl_resource *offer);

/* Send `cancelled` on whichever interface the source actually is. */
void
data_source_cancel(struct wl_resource *source);

/*
 * Claim a source for a selection. False if it has already been used, which is
 * the ext_data_control_device_v1 `used_source` error -- a source carries one
 * selection and cannot be recycled.
 */
bool
data_source_mark_used(struct wl_resource *source);

#endif
