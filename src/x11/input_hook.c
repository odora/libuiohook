/* libUIOHook: Cross-platfrom userland keyboard and mouse hooking.
 * Copyright (C) 2006-2014 Alexander Barker.  All Rights Received.
 * https://github.com/kwhat/libuiohook/
 *
 * libUIOHook is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libUIOHook is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <inttypes.h>
#include <limits.h>
#ifdef USE_XRECORD_ASYNC
#include <pthread.h>
#endif
#include <stdint.h>
#include <sys/time.h>
#include <uiohook.h>
#ifdef USE_XKB
#include <X11/XKBlib.h>
#endif
#include <X11/Xlibint.h>
#include <X11/Xlib.h>
#include <X11/extensions/record.h>
#if defined(USE_XINERAMA) && !defined(USE_XRANDR)
#include <X11/extensions/Xinerama.h>
#elif defined(USE_XRANDR)
#include <X11/extensions/Xrandr.h>
#else
// TODO We may need to fallback to the xf86vm extension for things like TwinView.
#pragma message("*** Warning: Xinerama or XRandR support is required to produce cross-platform mouse coordinates for multi-head configurations!")
#pragma message("... Assuming single-head display.")
#endif

#include "logger.h"
#include "input_helper.h"


// Thread and hook handles.
#ifdef USE_XRECORD_ASYNC
static bool running;

static pthread_cond_t hook_xrecord_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t hook_xrecord_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

typedef struct _hook_info {
	struct _data {
		Display *display;
		XRecordRange *range;
	} data;
	struct _ctrl {
		Display *display;
		XRecordContext context;
	} ctrl;
} hook_info;

// Modifiers for tracking key masks.
static uint16_t current_modifiers = 0x0000;

// For this struct, refer to libxnee, requires Xlibint.h
typedef union {
	unsigned char		type;
	xEvent				event;
	xResourceReq		req;
	xGenericReply		reply;
	xError				error;
	xConnSetupPrefix	setup;
} XRecordDatum;

// Mouse globals.
static unsigned short int click_count = 0;
static long int click_time = 0;
static unsigned short int click_button = MOUSE_NOBUTTON;
static bool mouse_dragged = false;

// Structure for the current Unix epoch in milliseconds.
static struct timeval system_time;
static Time previous_time = (Time) (~0x00);
static uint64_t offset_time = 0;

// Virtual event pointer.
static uiohook_event event;

// Event dispatch callback.
static dispatcher_t dispatcher = NULL;

UIOHOOK_API void hook_set_dispatch_proc(dispatcher_t dispatch_proc) {
	logger(LOG_LEVEL_DEBUG,	"%s [%u]: Setting new dispatch callback to %#p.\n",
			__FUNCTION__, __LINE__, dispatch_proc);

	dispatcher = dispatch_proc;
}

// Send out an event if a dispatcher was set.
static inline void dispatch_event(uiohook_event *const event) {
	if (dispatcher != NULL) {
		logger(LOG_LEVEL_DEBUG,	"%s [%u]: Dispatching event type %u.\n",
				__FUNCTION__, __LINE__, event->type);

		dispatcher(event);
	}
	else {
		logger(LOG_LEVEL_WARN,	"%s [%u]: No dispatch callback set!\n",
				__FUNCTION__, __LINE__);
	}
}


// Set the native modifier mask for future events.
static inline void set_modifier_mask(uint16_t mask) {
	current_modifiers |= mask;
}

// Unset the native modifier mask for future events.
static inline void unset_modifier_mask(uint16_t mask) {
	current_modifiers ^= mask;
}

// Get the current native modifier mask state.
static inline uint16_t get_modifiers() {
	return current_modifiers;
}


static inline uint64_t get_event_timestamp(XRecordInterceptData *recorded_data) {
	// Check for event clock reset.
	if (previous_time > recorded_data->server_time) {
		// Get the local system time in UTC.
		gettimeofday(&system_time, NULL);

		// Convert the local system time to a Unix epoch in MS.
		uint64_t epoch_time = (system_time.tv_sec * 1000) + (system_time.tv_usec / 1000);

		// Calculate the offset based on the system and hook times.
		offset_time = epoch_time - recorded_data->server_time;

		logger(LOG_LEVEL_INFO,	"%s [%u]: Resynchronizing event clock. (%" PRIu64 ")\n",
				__FUNCTION__, __LINE__, offset_time);
	}
	// Set the previous event time for click reset check above.
	previous_time = recorded_data->server_time;

	// Set the event time to the server time + offset.
	return recorded_data->server_time + offset_time;
}


void hook_event_proc(XPointer closeure, XRecordInterceptData *recorded_data) {
	// Calculate Unix epoch from native time source.
	uint64_t timestamp = get_event_timestamp(recorded_data);

	if (recorded_data->category == XRecordStartOfData) {
		// Populate the hook start event.
		event.time = timestamp;
		event.reserved = 0x00;

		event.type = EVENT_HOOK_ENABLED;
		event.mask = 0x00;

		// Fire the hook start event.
		dispatch_event(&event);
	}
	else if (recorded_data->category == XRecordEndOfData) {
		// Populate the hook stop event.
		event.time = timestamp;
		event.reserved = 0x00;

		event.type = EVENT_HOOK_DISABLED;
		event.mask = 0x00;

		// Fire the hook stop event.
		dispatch_event(&event);
	}
	else if (recorded_data->category == XRecordFromServer || recorded_data->category == XRecordFromClient) {
		// Get XRecord data.
		XRecordDatum *data = (XRecordDatum *) recorded_data->data;
	
		if (data->type == KeyPress) {
			// The X11 KeyCode associated with this event.
			KeyCode keycode = (KeyCode) data->event.u.u.detail;
			KeySym keysym = keycode_to_keysym(keycode, data->event.u.keyButtonPointer.state);
			unsigned short int scancode = keycode_to_scancode(keycode);

			// TODO If you have a better suggestion for this ugly, let me know.
			if		(scancode == VC_SHIFT_L)	{ set_modifier_mask(MASK_SHIFT_L);	}
			else if (scancode == VC_SHIFT_R)	{ set_modifier_mask(MASK_SHIFT_R);	}
			else if (scancode == VC_CONTROL_L)	{ set_modifier_mask(MASK_CTRL_L);	}
			else if (scancode == VC_CONTROL_R)	{ set_modifier_mask(MASK_CTRL_R);	}
			else if (scancode == VC_ALT_L)		{ set_modifier_mask(MASK_ALT_L);	}
			else if (scancode == VC_ALT_R)		{ set_modifier_mask(MASK_ALT_R);	}
			else if (scancode == VC_META_L)		{ set_modifier_mask(MASK_META_L);	}
			else if (scancode == VC_META_R)		{ set_modifier_mask(MASK_META_R);	}

			// Populate key pressed event.
			event.time = timestamp;
			event.reserved = 0x00;

			event.type = EVENT_KEY_PRESSED;
			event.mask = get_modifiers();

			event.data.keyboard.keycode = scancode;
			event.data.keyboard.rawcode = keysym;
			event.data.keyboard.keychar = CHAR_UNDEFINED;

			logger(LOG_LEVEL_INFO,	"%s [%u]: Key %#X pressed. (%#X)\n",
					__FUNCTION__, __LINE__, event.data.keyboard.keycode, event.data.keyboard.rawcode);

			// Fire key pressed event.
			dispatch_event(&event);

			// If the pressed event was not consumed...
			if (event.reserved ^ 0x01) {
				wchar_t buffer[1];
				
				// Check to make sure the key is printable.
				size_t count = keysym_to_unicode(keysym, buffer, sizeof(buffer));
				if (count > 0) {
					// NOTE This will currently always be a single iteration.
					//for (unsigned int i = 0; i < count; i++) {
						// Populate key typed event.
						event.time = timestamp;
						event.reserved = 0x00;

						event.type = EVENT_KEY_TYPED;
						event.mask = get_modifiers();

						event.data.keyboard.keycode = VC_UNDEFINED;
						event.data.keyboard.rawcode = keysym;
						//event.data.keyboard.keychar = buffer[i];
						event.data.keyboard.keychar = buffer[0];

						logger(LOG_LEVEL_INFO,	"%s [%u]: Key %#X typed. (%lc)\n",
								__FUNCTION__, __LINE__, event.data.keyboard.keycode, (wint_t) event.data.keyboard.keychar);

						// Fire key typed event.
						dispatch_event(&event);
					//}
				}
			}
		}
		else if (data->type == KeyRelease) {
			// The X11 KeyCode associated with this event.
			KeyCode keycode = (KeyCode) data->event.u.u.detail;
			KeySym keysym = keycode_to_keysym(keycode, data->event.u.keyButtonPointer.state);
			unsigned short int scancode = keycode_to_scancode(keycode);

			// TODO If you have a better suggestion for this ugly, let me know.
			if		(scancode == VC_SHIFT_L)	{ unset_modifier_mask(MASK_SHIFT_L);	}
			else if (scancode == VC_SHIFT_R)	{ unset_modifier_mask(MASK_SHIFT_R);	}
			else if (scancode == VC_CONTROL_L)	{ unset_modifier_mask(MASK_CTRL_L);		}
			else if (scancode == VC_CONTROL_R)	{ unset_modifier_mask(MASK_CTRL_R);		}
			else if (scancode == VC_ALT_L)		{ unset_modifier_mask(MASK_ALT_L);		}
			else if (scancode == VC_ALT_R)		{ unset_modifier_mask(MASK_ALT_R);		}
			else if (scancode == VC_META_L)		{ unset_modifier_mask(MASK_META_L);		}
			else if (scancode == VC_META_R)		{ unset_modifier_mask(MASK_META_R);		}


			// Populate key released event.
			event.time = timestamp;
			event.reserved = 0x00;

			event.type = EVENT_KEY_RELEASED;
			event.mask = get_modifiers();

			event.data.keyboard.keycode = keycode;
			event.data.keyboard.rawcode = keysym;
			event.data.keyboard.keychar = CHAR_UNDEFINED;

			logger(LOG_LEVEL_INFO, "%s [%u]: Key %#X released. (%#X)\n",
					__FUNCTION__, __LINE__, event.data.keyboard.keycode, event.data.keyboard.rawcode);

			// Fire key released event.
			dispatch_event(&event);
		}
		else if (data->type == ButtonPress) {
			// X11 handles wheel events as button events.
			if (data->event.u.u.detail == WheelUp || data->event.u.u.detail == WheelDown) {
				// Reset the click count and previous button.
				click_count = 1;
				click_button = MOUSE_NOBUTTON;

				/* Scroll wheel release events.
				 * Scroll type: WHEEL_UNIT_SCROLL
				 * Scroll amount: 3 unit increments per notch
				 * Units to scroll: 3 unit increments
				 * Vertical unit increment: 15 pixels
				 */

				// Populate mouse wheel event.
				event.time = timestamp;
				event.reserved = 0x00;

				event.type = EVENT_MOUSE_WHEEL;
				event.mask = get_modifiers();

				event.data.wheel.clicks = click_count;
				event.data.wheel.x = data->event.u.keyButtonPointer.rootX;
				event.data.wheel.y = data->event.u.keyButtonPointer.rootY;

				#if defined(USE_XINERAMA) || defined(USE_XRANDR)
				uint8_t count;
				screen_data *screens = hook_get_screen_info(&count);
				if (count > 1) {
					event.data.wheel.x -= screens[0].x;
					event.data.wheel.y -= screens[0].y;
				}
				#endif

				/* X11 does not have an API call for acquiring the mouse scroll type.  This
				 * maybe part of the XInput2 (XI2) extention but I will wont know until it
				 * is available on my platform.  For the time being we will just use the
				 * unit scroll value.
				 */
				event.data.wheel.type = WHEEL_UNIT_SCROLL;

				/* Some scroll wheel properties are available via the new XInput2 (XI2)
				 * extension.  Unfortunately the extension is not available on my
				 * development platform at this time.  For the time being we will just
				 * use the Windows default value of 3.
				 */
				event.data.wheel.amount = 3;

				// MS assumption is more natural (follows the cartesian coordinate system)
				// FIXME I don't understand the above adjustment and comment...
				if (data->event.u.u.detail == WheelUp) {
					// Wheel Rotated Up and Away.
					event.data.wheel.rotation = -1;
				}
				else { // data->event.u.u.detail == WheelDown
					// Wheel Rotated Down and Towards.
					event.data.wheel.rotation = 1;
				}

				logger(LOG_LEVEL_INFO,	"%s [%u]: Mouse wheel type %u, rotated %i units at %u, %u.\n",
						__FUNCTION__, __LINE__, event.data.wheel.type,
						event.data.wheel.amount * event.data.wheel.rotation,
						event.data.wheel.x, event.data.wheel.y );

				// Fire mouse wheel event.
				dispatch_event(&event);
			}
			else {
				/* This information is all static for X11, its up to the WM to
				 * decide how to interpret the wheel events.
				 */
				uint16_t button = MOUSE_NOBUTTON;
				switch (data->event.u.u.detail) {
					// FIXME This should use a lookup table to handle button remapping.
					case Button1:
						button = MOUSE_BUTTON1;
						set_modifier_mask(MASK_BUTTON1);
						break;

					case Button2:
						button = MOUSE_BUTTON2;
						set_modifier_mask(MASK_BUTTON2);
						break;

					case Button3:
						button = MOUSE_BUTTON3;
						set_modifier_mask(MASK_BUTTON3);
						break;

					case XButton1:
						button = MOUSE_BUTTON4;
						set_modifier_mask(MASK_BUTTON5);
						break;

					case XButton2:
						button = MOUSE_BUTTON5;
						set_modifier_mask(MASK_BUTTON5);
						break;

					default:
						// Extra buttons are at # - 4 starting after WheelUp and WheelDown.
						if (data->event.u.u.detail - 4 <= UINT16_MAX) {
							button = data->event.u.u.detail - 4;
							
							if (button + 7 < 16) {
								set_modifier_mask(1 << (button + 7));
							}
						}
						break;
				}


				// Track the number of clicks, the button must match the previous button.
				if (button == click_button && (long int) (timestamp - click_time) <= hook_get_multi_click_time()) {
					if (click_count < USHRT_MAX) {
						click_count++;
					}
					else {
						logger(LOG_LEVEL_WARN, "%s [%u]: Click count overflow detected!\n",
								__FUNCTION__, __LINE__);
					}
				}
				else {
					// Reset the click count.
					click_count = 1;

					// Set the previous button.
					click_button = button;
				}

				// Save this events time to calculate the click_count.
				click_time = timestamp;


				// Populate mouse pressed event.
				event.time = timestamp;
				event.reserved = 0x00;

				event.type = EVENT_MOUSE_PRESSED;
				event.mask = get_modifiers();

				event.data.mouse.button = button;
				event.data.mouse.clicks = click_count;
				event.data.mouse.x = data->event.u.keyButtonPointer.rootX;
				event.data.mouse.y = data->event.u.keyButtonPointer.rootY;

				#if defined(USE_XINERAMA) || defined(USE_XRANDR)
				uint8_t count;
				screen_data *screens = hook_get_screen_info(&count);
				if (count > 1) {
					event.data.mouse.x -= screens[0].x;
					event.data.mouse.y -= screens[0].y;
				}
				#endif

				logger(LOG_LEVEL_INFO,	"%s [%u]: Button %u  pressed %u time(s). (%u, %u)\n",
						__FUNCTION__, __LINE__, event.data.mouse.button, event.data.mouse.clicks,
						event.data.mouse.x, event.data.mouse.y);

				// Fire mouse pressed event.
				dispatch_event(&event);
			}
		}
		else if (data->type == ButtonRelease) {
			// X11 handles wheel events as button events.
			if (data->event.u.u.detail != WheelUp && data->event.u.u.detail != WheelDown) {
				/* This information is all static for X11, its up to the WM to
				 * decide how to interpret the wheel events.
				 */
				uint16_t button = MOUSE_NOBUTTON;
				switch (data->event.u.u.detail) {
					// FIXME This should use a lookup table to handle button remapping.
					case Button1:
						button = MOUSE_BUTTON1;
						unset_modifier_mask(MASK_BUTTON1);
						break;

					case Button2:
						button = MOUSE_BUTTON2;
						unset_modifier_mask(MASK_BUTTON2);
						break;

					case Button3:
						button = MOUSE_BUTTON3;
						unset_modifier_mask(MASK_BUTTON3);
						break;

					case XButton1:
						button = MOUSE_BUTTON4;
						unset_modifier_mask(MASK_BUTTON5);
						break;

					case XButton2:
						button = MOUSE_BUTTON5;
						unset_modifier_mask(MASK_BUTTON5);
						break;

					default:
						// Extra buttons are at # - 4 starting after WheelUp and WheelDown.
						if (data->event.u.u.detail - 4 <= UINT16_MAX) {
							button = data->event.u.u.detail - 4;
							
							if (button + 7 < 16) {
								unset_modifier_mask(1 << (button + 7));
							}
						}
						break;
				}
				
				// Populate mouse released event.
				event.time = timestamp;
				event.reserved = 0x00;

				event.type = EVENT_MOUSE_RELEASED;
				event.mask = get_modifiers();

				event.data.mouse.button = button;
				event.data.mouse.clicks = click_count;
				event.data.mouse.x = data->event.u.keyButtonPointer.rootX;
				event.data.mouse.y = data->event.u.keyButtonPointer.rootY;

				#if defined(USE_XINERAMA) || defined(USE_XRANDR)
				uint8_t count;
				screen_data *screens = hook_get_screen_info(&count);
				if (count > 1) {
					event.data.mouse.x -= screens[0].x;
					event.data.mouse.y -= screens[0].y;
				}
				#endif

				logger(LOG_LEVEL_INFO,	"%s [%u]: Button %u released %u time(s). (%u, %u)\n",
						__FUNCTION__, __LINE__, event.data.mouse.button,
						event.data.mouse.clicks,
						event.data.mouse.x, event.data.mouse.y);

				// Fire mouse released event.
				dispatch_event(&event);

				// If the pressed event was not consumed...
				if (event.reserved ^ 0x01 && mouse_dragged != true) {
					// Populate mouse clicked event.
					event.time = timestamp;
					event.reserved = 0x00;

					event.type = EVENT_MOUSE_CLICKED;
					event.mask = get_modifiers();

					event.data.mouse.button = button;
					event.data.mouse.clicks = click_count;
					event.data.mouse.x = data->event.u.keyButtonPointer.rootX;
					event.data.mouse.y = data->event.u.keyButtonPointer.rootY;

					#if defined(USE_XINERAMA) || defined(USE_XRANDR)
					uint8_t count;
					screen_data *screens = hook_get_screen_info(&count);
					if (count > 1) {
						event.data.mouse.x -= screens[0].x;
						event.data.mouse.y -= screens[0].y;
					}
					#endif

					logger(LOG_LEVEL_INFO,	"%s [%u]: Button %u clicked %u time(s). (%u, %u)\n",
							__FUNCTION__, __LINE__, event.data.mouse.button,
							event.data.mouse.clicks,
							event.data.mouse.x, event.data.mouse.y);

					// Fire mouse clicked event.
					dispatch_event(&event);
				}
			}
		}
		else if (data->type == MotionNotify) {
			// Reset the click count.
			if (click_count != 0 && (long int) (event.time - click_time) > hook_get_multi_click_time()) {
				click_count = 0;
			}

			// Populate mouse move event.
			event.time = timestamp;
			event.reserved = 0x00;

			event.mask = get_modifiers();

			// Check the upper half of virtual modifiers for non-zero
			// values and set the mouse dragged flag.
			mouse_dragged = (event.mask >> 8 > 0);
			if (mouse_dragged) {
				// Create Mouse Dragged event.
				event.type = EVENT_MOUSE_DRAGGED;
			}
			else {
				// Create a Mouse Moved event.
				event.type = EVENT_MOUSE_MOVED;
			}

			event.data.mouse.button = MOUSE_NOBUTTON;
			event.data.mouse.clicks = click_count;
			event.data.mouse.x = data->event.u.keyButtonPointer.rootX;
			event.data.mouse.y = data->event.u.keyButtonPointer.rootY;

			#if defined(USE_XINERAMA) || defined(USE_XRANDR)
			uint8_t count;
			screen_data *screens = hook_get_screen_info(&count);
			if (count > 1) {
				event.data.mouse.x -= screens[0].x;
				event.data.mouse.y -= screens[0].y;
			}
			#endif

			logger(LOG_LEVEL_INFO,	"%s [%u]: Mouse %s to %i, %i. (%#X)\n",
					__FUNCTION__, __LINE__, mouse_dragged ? "dragged" : "moved",
					event.data.mouse.x, event.data.mouse.y, event.mask);

			// Fire mouse move event.
			dispatch_event(&event);
		}
		else {
			// In theory this *should* never execute.
			logger(LOG_LEVEL_WARN,	"%s [%u]: Unhandled X11 event! (%#X)\n",
					__FUNCTION__, __LINE__, (unsigned int) data->type);
		}
	}
	else {
		logger(LOG_LEVEL_WARN,	"%s [%u]: Unhandled X11 hook category! (%#X)\n",
				__FUNCTION__, __LINE__, recorded_data->category);
	}

	// TODO There is no way to consume the XRecord event.

	XRecordFreeData(recorded_data);
}

static hook_info *hook;
UIOHOOK_API int hook_run() {
	int status = UIOHOOK_FAILURE;
	
	// Hook data for future cleanup.
	hook = malloc(sizeof(hook_info));
	if (hook != NULL) {
		// Open the control display for XRecord.
		hook->ctrl.display = XOpenDisplay(NULL);

		// Open a data display for XRecord.
		// NOTE This display must be opened on the same thread as XRecord.
		hook->data.display = XOpenDisplay(NULL);
		if (hook->ctrl.display != NULL && hook->data.display != NULL) {
			logger(LOG_LEVEL_DEBUG,	"%s [%u]: XOpenDisplay successful.\n",
					__FUNCTION__, __LINE__);
			
			// Attempt to setup detectable autorepeat.
			// NOTE: is_auto_repeat is NOT stdbool!
			Bool is_auto_repeat = False;
			#ifdef USE_XKB
			// Enable detectable autorepeat.
			XkbSetDetectableAutoRepeat(hook->ctrl.display, True, &is_auto_repeat);
			#else
			XAutoRepeatOn(hook->ctrl.display);

			XKeyboardState kb_state;
			XGetKeyboardControl(hook->ctrl.display, &kb_state);

			is_auto_repeat = (kb_state.global_auto_repeat == AutoRepeatModeOn);
			#endif

			if (is_auto_repeat) {
				logger(LOG_LEVEL_DEBUG,	"%s [%u]: Successfully enabled detectable autorepeat.\n",
						__FUNCTION__, __LINE__);
			}
			else {
				logger(LOG_LEVEL_WARN,	"%s [%u]: Could not enable detectable auto-repeat!\n",
						__FUNCTION__, __LINE__);
			}
			
			
			// Check to make sure XRecord is installed and enabled.
			int major, minor;
			if (XRecordQueryVersion(hook->ctrl.display, &major, &minor) != 0) {
				logger(LOG_LEVEL_INFO,	"%s [%u]: XRecord version: %i.%i.\n",
						__FUNCTION__, __LINE__, major, minor);
				
				// Make sure the data display is synchronized to prevent late event delivery!
				// See Bug 42356 for more information.
				// https://bugs.freedesktop.org/show_bug.cgi?id=42356#c4
				XSynchronize(hook->data.display, True);

				// Setup XRecord range.
				XRecordClientSpec clients = XRecordAllClients;
				hook->data.range = XRecordAllocRange();
				if (hook->data.range != NULL) {
					logger(LOG_LEVEL_DEBUG,	"%s [%u]: XRecordAllocRange successful.\n",
							__FUNCTION__, __LINE__);

					// Create XRecord Context.
					hook->data.range->device_events.first = KeyPress;
					hook->data.range->device_events.last = MotionNotify;
					
					// Note that the documentation for this function is incorrect,
					// hook->data.display should be used!
					// See: http://www.x.org/releases/X11R7.6/doc/libXtst/recordlib.txt
					hook->ctrl.context = XRecordCreateContext(hook->data.display, XRecordFromServerTime, &clients, 1, &hook->data.range, 1);
					if (hook->ctrl.context != 0) {
						logger(LOG_LEVEL_DEBUG,	"%s [%u]: XRecordCreateContext successful.\n",
								__FUNCTION__, __LINE__);
						
						// Save the data display associated with this hook so it is passed to each event.
						//XPointer closeure = (XPointer) (ctrl_display);
						XPointer closeure = NULL;

						#ifdef USE_XRECORD_ASYNC
						// Async requires that we loop so that our thread does not return.
						if (XRecordEnableContextAsync(hook->data.display, context, hook_event_proc, closeure) != 0) {
							// Time in MS to sleep the runloop.
							int timesleep = 100;

							// Allow the thread loop to block.
							pthread_mutex_lock(&hook_xrecord_mutex);
							running = true;

							do {
								// Unlock the mutex from the previous iteration.
								pthread_mutex_unlock(&hook_xrecord_mutex);

								XRecordProcessReplies(hook->data.display);

								// Prevent 100% CPU utilization.
								struct timeval tv;
								gettimeofday(&tv, NULL);

								struct timespec ts;
								ts.tv_sec = time(NULL) + timesleep / 1000;
								ts.tv_nsec = tv.tv_usec * 1000 + 1000 * 1000 * (timesleep % 1000);
								ts.tv_sec += ts.tv_nsec / (1000 * 1000 * 1000);
								ts.tv_nsec %= (1000 * 1000 * 1000);

								pthread_mutex_lock(&hook_xrecord_mutex);
								pthread_cond_timedwait(&hook_xrecord_cond, &hook_xrecord_mutex, &ts);
							} while (running);

							// Unlock after loop exit.
							pthread_mutex_unlock(&hook_xrecord_mutex);

							// Set the exit status.
							status = NULL;
						}
						#else
						// Sync blocks until XRecordDisableContext() is called.
						if (XRecordEnableContext(hook->data.display, hook->ctrl.context, hook_event_proc, closeure) != 0) {
							status = UIOHOOK_SUCCESS;
						}
						#endif
						else {
							logger(LOG_LEVEL_ERROR,	"%s [%u]: XRecordEnableContext failure!\n",
								__FUNCTION__, __LINE__);

							#ifdef USE_XRECORD_ASYNC
							// Reset the running state.
							pthread_mutex_lock(&hook_xrecord_mutex);
							running = false;
							pthread_mutex_unlock(&hook_xrecord_mutex);
							#endif

							// Set the exit status.
							status = UIOHOOK_ERROR_X_RECORD_ENABLE_CONTEXT;
						}
						
						// Free up the context if it was set.
						XRecordFreeContext(hook->data.display, hook->ctrl.context);
					}
					else {
						logger(LOG_LEVEL_ERROR,	"%s [%u]: XRecordCreateContext failure!\n",
								__FUNCTION__, __LINE__);

						// Set the exit status.
						status = UIOHOOK_ERROR_X_RECORD_CREATE_CONTEXT;
					}

					// Free the XRecord range if it was set.
					XFree(hook->data.range);
				}
				else {
					logger(LOG_LEVEL_ERROR,	"%s [%u]: XRecordAllocRange failure!\n",
							__FUNCTION__, __LINE__);

					// Set the exit status.
					status = UIOHOOK_ERROR_X_RECORD_ALLOC_RANGE;
				}
			}
			else {
				logger(LOG_LEVEL_ERROR,	"%s [%u]: XRecord is not currently available!\n",
						__FUNCTION__, __LINE__);

				status = UIOHOOK_ERROR_X_RECORD_NOT_FOUND;
			}

			
			// FIXME We wouldn't need either null check if we reuse the X
			// display from the properties that is created on library load.
			
			// Close down the XRecord data display.
			if (hook->data.display != NULL) {
				XCloseDisplay(hook->data.display);
			}

			// Close down any open displays.
			if (hook->ctrl.display) {
				XCloseDisplay(hook->ctrl.display);
			}
		}
		else {	
			logger(LOG_LEVEL_ERROR,	"%s [%u]: XOpenDisplay failure!\n",
					__FUNCTION__, __LINE__);

			// Set the exit status.
			status = UIOHOOK_ERROR_X_OPEN_DISPLAY;
		}

		// Free data associated with this hook.
		free(hook);
		hook = NULL;	
	}
	else {	
		logger(LOG_LEVEL_ERROR,	"%s [%u]: Failed to allocate memory for hook structure!\n",
				__FUNCTION__, __LINE__);
		
		status = UIOHOOK_ERROR_OUT_OF_MEMORY;
	}


	logger(LOG_LEVEL_DEBUG,	"%s [%u]: Something, something, something, complete.\n",
			__FUNCTION__, __LINE__);

	return status;
}

UIOHOOK_API int hook_stop() {
	int status = UIOHOOK_FAILURE;

	if (hook != NULL && hook->ctrl.display != NULL && hook->ctrl.context != 0) {
		// We need to make sure the context is still valid.
		XRecordState *state = malloc(sizeof(XRecordState));
		if (state != NULL) {
			if (XRecordGetContext(hook->ctrl.display, hook->ctrl.context, &state) != 0) {
				// Try to exit the thread naturally.
				if (state->enabled && XRecordDisableContext(hook->ctrl.display, hook->ctrl.context) != 0) {
					#ifdef USE_XRECORD_ASYNC
					pthread_mutex_lock(&hook_xrecord_mutex);
					running = false;
					pthread_cond_signal(&hook_xrecord_cond);
					pthread_mutex_unlock(&hook_xrecord_mutex);
					#endif

					// See Bug 42356 for more information.
					// https://bugs.freedesktop.org/show_bug.cgi?id=42356#c4
					XFlush(hook->ctrl.display);
					//XSync(hook->ctrl.display, True);

					status = UIOHOOK_SUCCESS;
				}
			}
			else {
				logger(LOG_LEVEL_ERROR,	"%s [%u]: XRecordGetContext failure!\n",
						__FUNCTION__, __LINE__);
										
				status = UIOHOOK_ERROR_X_RECORD_GET_CONTEXT;
			}

			free(state);
		}
		else {	
			logger(LOG_LEVEL_ERROR,	"%s [%u]: Failed to allocate memory for XRecordState!\n",
				__FUNCTION__, __LINE__);
					
			status = UIOHOOK_ERROR_OUT_OF_MEMORY;
		}
		
		return status;
	}

	logger(LOG_LEVEL_DEBUG,	"%s [%u]: Status: %#X.\n",
			__FUNCTION__, __LINE__, status);

	return status;
}
