/*
 * dashboard.h - Public API for the system status dashboard.
 *
 * This header defines the interface for rendering real-time system metrics
 * directly to the framebuffer.
 */

#ifndef PERSPICUA_DRIVER_DASHBOARD_H
#define PERSPICUA_DRIVER_DASHBOARD_H

/* --- Function Prototypes --- */

/*
 * dashboard_update - Renders system metrics (uptime, memory, CPU) to the screen.
 */
void dashboard_update(void);

#endif /* PERSPICUA_DRIVER_DASHBOARD_H */
