/* swc: libswc/transaction.h
 *
 * Copyright (c) 2026 neoswc contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SWC_TRANSACTION_H
#define SWC_TRANSACTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_event_loop;

/*
 * A transaction is a cohort of configures that are applied together.
 *
 * swc applies each window's pending move the instant that window's own ack
 * arrives (see handle_attach() in window.c), so a multi-window relayout lands
 * in pieces across several frames. river-window-management-v1 requires the
 * opposite: the compositor sends new state to every window, waits for the
 * responses, and only then reports the resulting dimensions and lets the
 * manager finish the render sequence.
 *
 * This object provides the "waits for the responses" step. Participants are
 * opaque keys rather than windows, so the barrier carries no dependency on the
 * window, view or compositor layers and can be exercised without a display.
 *
 * The barrier is deliberately not all-or-nothing. The protocol permits a
 * window that is slow to respond to have its dimensions reported in a later
 * render sequence, so an expired transaction still completes and reports which
 * participants made it; the caller decides what to do with the stragglers.
 * (The protocol's `unresponsive` error concerns the window manager failing to
 * finish a sequence, not a window failing to ack.)
 */
struct transaction;

struct transaction_handler {
	/*
	 * Called exactly once per committed transaction, either when every
	 * participant has acknowledged or when the timeout expires, whichever
	 * comes first. `timed_out` distinguishes the two.
	 *
	 * The transaction is still readable during this call, so the handler can
	 * use transaction_acked() to sort the participants that responded from
	 * the ones that did not. The handler may destroy the transaction.
	 */
	void (*complete)(struct transaction *transaction, bool timed_out,
	                 void *data);
};

struct transaction *
transaction_create(struct wl_event_loop *loop,
                   const struct transaction_handler *handler, void *data);

/*
 * Safe to call from within the complete handler, and safe to call on a
 * transaction that was never committed.
 */
void
transaction_destroy(struct transaction *transaction);

/*
 * Enrol `key`, waiting on the configure identified by `serial`. Must be called
 * before transaction_commit(). Returns false on allocation failure, if `key`
 * is already enrolled, or if the transaction is already committed.
 */
bool
transaction_add(struct transaction *transaction, void *key, uint32_t serial);

/*
 * Withdraw `key`, whether or not it has acknowledged. Call this when a window
 * is destroyed while the barrier is waiting on it; otherwise the cohort waits
 * on a participant that can never respond and stalls until the timeout.
 *
 * If this removes the last outstanding participant of a committed transaction,
 * the complete handler runs before this function returns.
 */
void
transaction_remove(struct transaction *transaction, void *key);

/*
 * Record an acknowledgement. Ignored unless `key` is enrolled and `serial`
 * matches the one it was enrolled with, so a stale ack for a superseded
 * configure cannot satisfy the barrier. Repeated acks are idempotent.
 *
 * If this satisfies the last outstanding participant of a committed
 * transaction, the complete handler runs before this function returns.
 */
void
transaction_ack(struct transaction *transaction, void *key, uint32_t serial);

/*
 * Seal the cohort and start the clock. No further participants may be added.
 *
 * A transaction whose participants have all acknowledged already — including
 * one with no participants at all — completes before this function returns,
 * without arming the timer or waiting for a trip through the event loop.
 */
void
transaction_commit(struct transaction *transaction, uint32_t timeout_ms);

bool
transaction_committed(struct transaction *transaction);
bool
transaction_complete(struct transaction *transaction);

/* Whether `key` is enrolled and has acknowledged. */
bool
transaction_acked(struct transaction *transaction, void *key);

/* Participants still outstanding. */
size_t
transaction_pending(struct transaction *transaction);

/* Participants enrolled, acknowledged or not. */
size_t
transaction_size(struct transaction *transaction);

#endif
