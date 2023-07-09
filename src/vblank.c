#include <assert.h>

#include <ev.h>
#include <inttypes.h>

#include "compiler.h"
#include "list.h"        // for container_of
#include "log.h"
#include "vblank.h"
#include "x.h"

enum vblank_scheduler_type {
	/// X Present extension based vblank events
	PRESENT_VBLANK_SCHEDULER,
	/// GLX_SGI_video_sync based vblank events
	SGI_VIDEO_VSYNC_VBLANK_SCHEDULER,
};

struct vblank_scheduler {
	enum vblank_scheduler_type type;
	void *user_data;
	vblank_callback_t callback;
};

struct present_vblank_scheduler {
	struct vblank_scheduler base;

	uint64_t last_msc;
	/// The timestamp for the end of last vblank.
	uint64_t last_ust;
	ev_timer callback_timer;
	bool vblank_event_requested;
};

static void present_vblank_callback(EV_P_ ev_timer *w, int revents) {
	auto sched = container_of(w, struct present_vblank_scheduler, callback_timer);
	sched->base.callback(
	    &(struct vblank_event){
	        .msc = sched->last_msc,
	        .ust = sched->last_ust,
	    },
	    sched->base.user_data);
}

static struct present_vblank_scheduler *
present_vblank_scheduler_new(vblank_callback_t callback, void *user_data) {
	auto sched = ccalloc(1, struct present_vblank_scheduler);
	sched->base.user_data = user_data;
	sched->base.callback = callback;
	sched->base.type = PRESENT_VBLANK_SCHEDULER;
	ev_timer_init(&sched->callback_timer, present_vblank_callback, 0, 0);
	return sched;
}

static bool present_vblank_scheduler_schedule(struct present_vblank_scheduler *sched,
                                              xcb_window_t window, struct x_connection *c) {
	x_request_vblank_event(c, window, sched->last_msc + 1);
	return true;
}

struct vblank_scheduler *vblank_scheduler_new(vblank_callback_t callback, void *user_data) {
	return &present_vblank_scheduler_new(callback, user_data)->base;
}

bool vblank_scheduler_schedule(struct vblank_scheduler *sched, xcb_window_t window,
                               struct x_connection *c) {
	switch (sched->type) {
	case PRESENT_VBLANK_SCHEDULER:
		return present_vblank_scheduler_schedule(
		    (struct present_vblank_scheduler *)sched, window, c);
	case SGI_VIDEO_VSYNC_VBLANK_SCHEDULER: return false;
	default: assert(false);
	}
}

/// Handle PresentCompleteNotify events
///
/// Schedule the registered callback to be called when the current vblank ends.
void handle_present_complete_notify(struct vblank_scheduler *self, struct ev_loop *loop,
                                    struct x_connection *c,
                                    xcb_present_complete_notify_event_t *cne) {
	assert(self->type == PRESENT_VBLANK_SCHEDULER);

	auto sched = (struct present_vblank_scheduler *)self;
	if (cne->kind != XCB_PRESENT_COMPLETE_KIND_NOTIFY_MSC) {
		return;
	}

	assert(sched->vblank_event_requested);

	// X sometimes sends duplicate/bogus MSC events, when screen has just been turned
	// off. Don't use the msc value in these events. We treat this as not receiving a
	// vblank event at all, and try to get a new one.
	//
	// See:
	// https://gitlab.freedesktop.org/xorg/xserver/-/issues/1418
	bool event_is_invalid = cne->msc <= sched->last_msc || cne->ust == 0;
	if (event_is_invalid) {
		log_debug("Invalid PresentCompleteNotify event, %" PRIu64 " %" PRIu64,
		          cne->msc, cne->ust);
		x_request_vblank_event(c, cne->window, sched->last_msc + 1);
		return;
	}

	sched->vblank_event_requested = false;

	sched->last_ust = cne->ust;
	sched->last_msc = cne->msc;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	auto now_us = now.tv_sec * 1000000UL + now.tv_nsec / 1000;
	if (now_us > cne->ust) {
		sched->base.callback(
		    &(struct vblank_event){
		        .msc = cne->msc,
		        .ust = cne->ust,
		    },
		    sched->base.user_data);
	} else {
		// Wait until the end of the current vblank to call
		// handle_end_of_vblank. If we call it too early, it can
		// mistakenly think the render missed the vblank, and doesn't
		// schedule render for the next vblank, causing frame drops.
		log_trace("The end of this vblank is %" PRIi64 " us into the "
		          "future",
		          (int64_t)cne->ust - now_us);
		assert(!ev_is_active(&sched->callback_timer));
		ev_timer_set(&sched->callback_timer,
		             (double)(cne->ust - now_us) / 1000000.0, 0);
		ev_timer_start(loop, &sched->callback_timer);
	}
}
