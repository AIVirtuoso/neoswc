/* swc: libswc/transaction.c
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

#include "transaction.h"

#include <stdlib.h>
#include <wayland-server.h>

struct participant {
	struct wl_list link;
	void *key;
	uint32_t serial;
	bool acked;
};

struct transaction {
	struct wl_event_loop *loop;
	const struct transaction_handler *handler;
	void *data;

	struct wl_list participants;
	struct wl_event_source *timer;

	size_t size, pending;
	bool committed, complete;
};

static struct participant *
find(struct transaction *transaction, void *key)
{
	struct participant *participant;

	wl_list_for_each (participant, &transaction->participants, link) {
		if (participant->key == key) {
			return participant;
		}
	}

	return NULL;
}

/*
 * Run the completion handler, at most once per transaction.
 *
 * The handler is allowed to destroy the transaction, so nothing may touch
 * `transaction` after the call returns. The complete flag is therefore set
 * before dispatching, which also makes this reentrancy-safe: an ack delivered
 * from within the handler cannot drive a second completion.
 */
static void
finish(struct transaction *transaction, bool timed_out)
{
	if (transaction->complete) {
		return;
	}
	transaction->complete = true;

	/*
	 * Disarm rather than destroy: transaction_destroy() still owns the
	 * source, and the handler may well be the thing that calls it.
	 */
	if (transaction->timer) {
		wl_event_source_timer_update(transaction->timer, 0);
	}

	if (transaction->handler && transaction->handler->complete) {
		transaction->handler->complete(transaction, timed_out,
		                               transaction->data);
	}
}

static int
handle_timeout(void *data)
{
	finish(data, true);
	return 0;
}

struct transaction *
transaction_create(struct wl_event_loop *loop,
                   const struct transaction_handler *handler, void *data)
{
	struct transaction *transaction;

	if (!loop) {
		return NULL;
	}

	if (!(transaction = malloc(sizeof(*transaction)))) {
		return NULL;
	}

	transaction->loop = loop;
	transaction->handler = handler;
	transaction->data = data;
	transaction->timer = NULL;
	transaction->size = 0;
	transaction->pending = 0;
	transaction->committed = false;
	transaction->complete = false;
	wl_list_init(&transaction->participants);

	return transaction;
}

void
transaction_destroy(struct transaction *transaction)
{
	struct participant *participant, *tmp;

	if (!transaction) {
		return;
	}

	if (transaction->timer) {
		wl_event_source_remove(transaction->timer);
	}

	wl_list_for_each_safe (participant, tmp, &transaction->participants,
	                       link) {
		wl_list_remove(&participant->link);
		free(participant);
	}

	free(transaction);
}

bool
transaction_add(struct transaction *transaction, void *key, uint32_t serial)
{
	struct participant *participant;

	if (!transaction || transaction->committed || find(transaction, key)) {
		return false;
	}

	if (!(participant = malloc(sizeof(*participant)))) {
		return false;
	}

	participant->key = key;
	participant->serial = serial;
	participant->acked = false;
	wl_list_insert(&transaction->participants, &participant->link);
	++transaction->size;
	++transaction->pending;

	return true;
}

void
transaction_remove(struct transaction *transaction, void *key)
{
	struct participant *participant;

	if (!transaction || !(participant = find(transaction, key))) {
		return;
	}

	wl_list_remove(&participant->link);
	--transaction->size;
	if (!participant->acked) {
		--transaction->pending;
	}
	free(participant);

	if (transaction->committed && transaction->pending == 0) {
		finish(transaction, false);
	}
}

void
transaction_ack(struct transaction *transaction, void *key, uint32_t serial)
{
	struct participant *participant;

	if (!transaction || !(participant = find(transaction, key))) {
		return;
	}

	/*
	 * An exact serial match. A window that was reconfigured after being
	 * enrolled belongs to a later cohort, and its ack for the superseded
	 * configure must not release this one.
	 */
	if (participant->serial != serial || participant->acked) {
		return;
	}

	participant->acked = true;
	--transaction->pending;

	if (transaction->committed && transaction->pending == 0) {
		finish(transaction, false);
	}
}

void
transaction_commit(struct transaction *transaction, uint32_t timeout_ms)
{
	if (!transaction || transaction->committed) {
		return;
	}
	transaction->committed = true;

	/*
	 * Nothing outstanding: complete synchronously rather than burning a
	 * frame waiting for a timer that would fire with no work to do. This is
	 * the common case for a manage sequence that only reordered windows.
	 */
	if (transaction->pending == 0) {
		finish(transaction, false);
		return;
	}

	transaction->timer =
	    wl_event_loop_add_timer(transaction->loop, handle_timeout, transaction);
	if (!transaction->timer) {
		/*
		 * Without a timer the barrier could stall forever on a window that
		 * never acks, which is strictly worse than applying early.
		 */
		finish(transaction, true);
		return;
	}

	wl_event_source_timer_update(transaction->timer, timeout_ms);
}

bool
transaction_committed(struct transaction *transaction)
{
	return transaction && transaction->committed;
}

bool
transaction_complete(struct transaction *transaction)
{
	return transaction && transaction->complete;
}

bool
transaction_acked(struct transaction *transaction, void *key)
{
	struct participant *participant;

	if (!transaction || !(participant = find(transaction, key))) {
		return false;
	}

	return participant->acked;
}

size_t
transaction_pending(struct transaction *transaction)
{
	return transaction ? transaction->pending : 0;
}

size_t
transaction_size(struct transaction *transaction)
{
	return transaction ? transaction->size : 0;
}
