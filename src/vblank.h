#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <xcb/present.h>
#include <xcb/xcb.h>

#include <ev.h>

#include "x.h"

/// An object that schedule vblank events.
struct vblank_scheduler;

struct vblank_event {
	uint64_t msc;
	uint64_t ust;
};

typedef void (*vblank_callback_t)(struct vblank_event *event, void *user_data);

/// Schedule a vblank event.
///
/// Schedule for `cb` to be called when the current vblank
/// ends.
///
/// Returns whether a new event is scheduled. If there is already an event scheduled for
/// the current vblank, this function will do n1othing and return false.
bool vblank_scheduler_schedule(struct vblank_scheduler *self, xcb_window_t window,
                               struct x_connection *c);
struct vblank_scheduler *vblank_scheduler_new(vblank_callback_t callback, void *user_data);

/// Handle PresentCompleteNotify events
///
/// Schedule the registered callback to be called when the current vblank ends.
void handle_present_complete_notify(struct vblank_scheduler *self, struct ev_loop *loop,
                                    struct x_connection *c,
                                    xcb_present_complete_notify_event_t *cne);

// NOTE(yshui): OK, this vblank scheduler abstraction is a bit leaky. The core has to call
// handle_present_complete_notify() to drive the scheduler, the scheduler doesn't drive
// itself. In theory we can add an API for the scheduler to register callbacks on specific
// X events. But that's a bit overkill for now, as we only need to handle
// PresentCompleteNotify.
