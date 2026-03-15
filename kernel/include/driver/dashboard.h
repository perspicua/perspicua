/*
 * dashboard.h - Public API for the system dashboard driver.
 *
 * This file defines the interface for updating and rendering the
 * system status dashboard on the screen.
 */

#ifndef PERSPICUA_DRIVER_DASHBOARD_H
#define PERSPICUA_DRIVER_DASHBOARD_H

/*
 * dashboard_update - Renders the current system status to the top of the screen.
 * Displays uptime, memory usage (PMM, Heap), and per-core scheduling status.
 */
void dashboard_update(void);

#endif /* PERSPICUA_DRIVER_DASHBOARD_H */
