/* swc: test/transaction_test.c
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

/*
 * Exercises the transaction barrier against a bare wl_event_loop. No DRM, no
 * compositor, no windows -- the barrier deals in opaque keys precisely so it
 * can be driven like this.
 */

#include "transaction.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>

static int failures;
static const char *current_test;

#define CHECK(cond)                                                            \
	do {                                                                       \
		if (!(cond)) {                                                         \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
			++failures;                                                        \
		}                                                                      \
	} while (0)

#define RUN(fn)                                                                \
	do {                                                                       \
		int before = failures;                                                 \
		current_test = #fn;                                                    \
		fn();                                                                  \
		printf("%-4s %s\n", failures == before ? "ok" : "FAIL", current_test); \
	} while (0)

/* ------------------------------------------------------------------------ */

struct result {
	int calls;
	bool timed_out;
};

static void
record(struct transaction *transaction, bool timed_out, void *data)
{
	struct result *result = data;

	(void)transaction;
	++result->calls;
	result->timed_out = timed_out;
}

static const struct transaction_handler record_handler = {.complete = record};

/*
 * Stands in for a swc window's deferred move. window.c keeps exactly this
 * state (move.pending / move.x / move.y) and applies it in flush().
 */
struct fake_window {
	int32_t x, y;
	int32_t pending_x, pending_y;
	bool move_pending;
	int flushes;
};

static void
fake_move(struct fake_window *window, int32_t x, int32_t y)
{
	window->pending_x = x;
	window->pending_y = y;
	window->move_pending = true;
}

static void
fake_flush(struct fake_window *window)
{
	if (!window->move_pending) {
		return;
	}
	window->x = window->pending_x;
	window->y = window->pending_y;
	window->move_pending = false;
	++window->flushes;
}

/* ------------------------------------------------------------------------ */

static struct wl_display *display;
static struct wl_event_loop *loop;

/* Pump the loop until the transaction completes or we give up. */
static void
pump(struct transaction *transaction, int max_iterations)
{
	int i;

	for (i = 0; i < max_iterations && !transaction_complete(transaction); ++i) {
		wl_event_loop_dispatch(loop, 200);
	}
}

/* ------------------------------------------------------------------------ */

/*
 * The spike's headline question. Two windows are told to move; the barrier
 * must hold both moves until both have acked, rather than applying each one
 * the instant its own ack lands (which is what window.c does today).
 */
static void
test_relayout_is_atomic(void)
{
	struct fake_window left = {0}, right = {0};
	struct result result = {0};
	struct transaction *transaction;

	transaction = transaction_create(loop, &record_handler, &result);
	CHECK(transaction != NULL);

	fake_move(&left, 0, 0);
	fake_move(&right, 960, 0);
	CHECK(transaction_add(transaction, &left, 101));
	CHECK(transaction_add(transaction, &right, 102));
	transaction_commit(transaction, 1000);

	/* First window responds promptly. Nothing may be applied yet. */
	transaction_ack(transaction, &left, 101);
	CHECK(result.calls == 0);
	CHECK(left.flushes == 0);
	CHECK(right.flushes == 0);
	CHECK(transaction_pending(transaction) == 1);

	/* Second responds; the cohort releases synchronously. */
	transaction_ack(transaction, &right, 102);
	CHECK(result.calls == 1);
	CHECK(!result.timed_out);

	fake_flush(&left);
	fake_flush(&right);
	CHECK(left.x == 0 && left.y == 0);
	CHECK(right.x == 960 && right.y == 0);
	CHECK(left.flushes == 1 && right.flushes == 1);

	transaction_destroy(transaction);
}

/* A straggler must not stall the cohort forever. */
static void
test_timeout_releases_cohort(void)
{
	struct fake_window quick = {0}, slow = {0};
	struct result result = {0};
	struct transaction *transaction;

	transaction = transaction_create(loop, &record_handler, &result);
	CHECK(transaction_add(transaction, &quick, 1));
	CHECK(transaction_add(transaction, &slow, 2));
	transaction_commit(transaction, 20);

	transaction_ack(transaction, &quick, 1);
	CHECK(result.calls == 0);

	pump(transaction, 10);

	CHECK(result.calls == 1);
	CHECK(result.timed_out);
	/* The handler can still tell who made it and who did not. */
	CHECK(transaction_acked(transaction, &quick));
	CHECK(!transaction_acked(transaction, &slow));
	CHECK(transaction_pending(transaction) == 1);

	transaction_destroy(transaction);
}

/* A cohort that is already satisfied must not wait for the event loop. */
static void
test_empty_cohort_completes_synchronously(void)
{
	struct result result = {0};
	struct transaction *transaction;

	transaction = transaction_create(loop, &record_handler, &result);
	transaction_commit(transaction, 1000);

	CHECK(result.calls == 1);
	CHECK(!result.timed_out);
	CHECK(transaction_size(transaction) == 0);

	transaction_destroy(transaction);
}

/* Acks may land between add() and commit(). */
static void
test_ack_before_commit(void)
{
	struct fake_window window = {0};
	struct result result = {0};
	struct transaction *transaction;

	transaction = transaction_create(loop, &record_handler, &result);
	CHECK(transaction_add(transaction, &window, 7));

	transaction_ack(transaction, &window, 7);
	CHECK(result.calls == 0); /* not committed yet */
	CHECK(transaction_pending(transaction) == 0);

	transaction_commit(transaction, 1000);
	CHECK(result.calls == 1);
	CHECK(!result.timed_out);

	transaction_destroy(transaction);
}

/*
 * A window reconfigured after enrolment belongs to a later cohort. Its ack for
 * the superseded configure must not release this one.
 */
static void
test_stale_serial_is_ignored(void)
{
	struct fake_window window = {0};
	struct result result = {0};
	struct transaction *transaction;

	transaction = transaction_create(loop, &record_handler, &result);
	CHECK(transaction_add(transaction, &window, 42));
	transaction_commit(transaction, 20);

	transaction_ack(transaction, &window, 41);
	CHECK(transaction_pending(transaction) == 1);
	transaction_ack(transaction, &window, 43);
	CHECK(transaction_pending(transaction) == 1);
	CHECK(result.calls == 0);

	pump(transaction, 10);
	CHECK(result.calls == 1);
	CHECK(result.timed_out);

	transaction_destroy(transaction);
}

static void
test_duplicate_ack_is_idempotent(void)
{
	struct fake_window a = {0}, b = {0};
	struct result result = {0};
	struct transaction *transaction;

	transaction = transaction_create(loop, &record_handler, &result);
	CHECK(transaction_add(transaction, &a, 1));
	CHECK(transaction_add(transaction, &b, 2));
	transaction_commit(transaction, 1000);

	transaction_ack(transaction, &a, 1);
	transaction_ack(transaction, &a, 1);
	transaction_ack(transaction, &a, 1);
	CHECK(transaction_pending(transaction) == 1);
	CHECK(result.calls == 0);

	transaction_ack(transaction, &b, 2);
	CHECK(result.calls == 1);

	transaction_destroy(transaction);
}

/*
 * The classic way to wedge a barrier: a window is destroyed while the cohort
 * is waiting on it, so the ack can never arrive.
 */
static void
test_destroyed_window_does_not_wedge(void)
{
	struct fake_window survivor = {0}, doomed = {0};
	struct result result = {0};
	struct transaction *transaction;

	transaction = transaction_create(loop, &record_handler, &result);
	CHECK(transaction_add(transaction, &survivor, 1));
	CHECK(transaction_add(transaction, &doomed, 2));
	transaction_commit(transaction, 1000);

	transaction_ack(transaction, &survivor, 1);
	CHECK(result.calls == 0);

	/* The window goes away before it can respond. */
	transaction_remove(transaction, &doomed);

	CHECK(result.calls == 1);
	CHECK(!result.timed_out); /* released cleanly, not by expiry */
	CHECK(transaction_size(transaction) == 1);

	transaction_destroy(transaction);
}

/* Removing the only unacked participant of an uncommitted cohort is inert. */
static void
test_remove_before_commit(void)
{
	struct fake_window a = {0}, b = {0};
	struct result result = {0};
	struct transaction *transaction;

	transaction = transaction_create(loop, &record_handler, &result);
	CHECK(transaction_add(transaction, &a, 1));
	CHECK(transaction_add(transaction, &b, 2));
	transaction_remove(transaction, &b);
	CHECK(result.calls == 0);
	CHECK(transaction_size(transaction) == 1);
	CHECK(transaction_pending(transaction) == 1);

	transaction_ack(transaction, &a, 1);
	transaction_commit(transaction, 1000);
	CHECK(result.calls == 1);

	transaction_destroy(transaction);
}

static void
test_rejects_duplicate_and_late_participants(void)
{
	struct fake_window a = {0}, b = {0};
	struct result result = {0};
	struct transaction *transaction;

	transaction = transaction_create(loop, &record_handler, &result);
	CHECK(transaction_add(transaction, &a, 1));
	CHECK(!transaction_add(transaction, &a, 2)); /* already enrolled */
	CHECK(transaction_size(transaction) == 1);

	transaction_commit(transaction, 1000);
	CHECK(!transaction_add(transaction, &b, 3)); /* cohort is sealed */
	CHECK(transaction_size(transaction) == 1);

	transaction_destroy(transaction);
}

/* The handler must fire once even if acks keep arriving after expiry. */
static void
test_completes_exactly_once(void)
{
	struct fake_window a = {0}, b = {0};
	struct result result = {0};
	struct transaction *transaction;

	transaction = transaction_create(loop, &record_handler, &result);
	CHECK(transaction_add(transaction, &a, 1));
	CHECK(transaction_add(transaction, &b, 2));
	transaction_commit(transaction, 20);

	pump(transaction, 10);
	CHECK(result.calls == 1);
	CHECK(result.timed_out);

	/* Late acks from both stragglers. */
	transaction_ack(transaction, &a, 1);
	transaction_ack(transaction, &b, 2);
	CHECK(result.calls == 1);

	/* A second commit is a no-op rather than a re-arm. */
	transaction_commit(transaction, 20);
	CHECK(result.calls == 1);

	transaction_destroy(transaction);
}

/*
 * The realistic integration shape: the completion handler tears the
 * transaction down. Anything that touches the object after dispatching the
 * handler is a use-after-free, so this runs under ASan in CI.
 */
static void
destroy_from_handler(struct transaction *transaction, bool timed_out,
                     void *data)
{
	struct result *result = data;

	(void)timed_out;
	++result->calls;
	transaction_destroy(transaction);
}

static void
test_destroy_from_handler(void)
{
	static const struct transaction_handler handler = {
	    .complete = destroy_from_handler,
	};
	struct fake_window window = {0};
	struct result result = {0};
	struct transaction *transaction;

	/* via the synchronous path */
	transaction = transaction_create(loop, &handler, &result);
	CHECK(transaction_add(transaction, &window, 1));
	transaction_commit(transaction, 1000);
	transaction_ack(transaction, &window, 1);
	CHECK(result.calls == 1);

	/* via the timer path */
	result.calls = 0;
	transaction = transaction_create(loop, &handler, &result);
	CHECK(transaction_add(transaction, &window, 1));
	transaction_commit(transaction, 20);
	wl_event_loop_dispatch(loop, 200);
	CHECK(result.calls == 1);
}

int
main(void)
{
	if (!(display = wl_display_create())) {
		fprintf(stderr, "failed to create wl_display\n");
		return 99;
	}
	loop = wl_display_get_event_loop(display);

	RUN(test_relayout_is_atomic);
	RUN(test_timeout_releases_cohort);
	RUN(test_empty_cohort_completes_synchronously);
	RUN(test_ack_before_commit);
	RUN(test_stale_serial_is_ignored);
	RUN(test_duplicate_ack_is_idempotent);
	RUN(test_destroyed_window_does_not_wedge);
	RUN(test_remove_before_commit);
	RUN(test_rejects_duplicate_and_late_participants);
	RUN(test_completes_exactly_once);
	RUN(test_destroy_from_handler);

	wl_display_destroy(display);

	if (failures) {
		fprintf(stderr, "\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
