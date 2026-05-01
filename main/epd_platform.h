#pragma once

#include "slint-platform.h"
#include "epaper.h"

/* Install the slint platform that targets the e-paper. Must be called
 * before any Slint components are created. */
void epd_platform_init(epd_handle_t epd, slint::PhysicalSize size);

/* Render the current Slint UI state into the e-paper framebuffer and flush
 * the panel. Synchronous: returns after the panel finishes refreshing.
 * No event loop, no tasks, no semaphores, no ISRs. */
void epd_platform_render(void);
