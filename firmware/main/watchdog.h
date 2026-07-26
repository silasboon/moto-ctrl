#pragma once

/* Task watchdog init/feed. Hardware brownout detection is handled by
 * ESP-IDF's built-in brownout detector (enabled via sdkconfig, not here).
 *
 * mc_watchdog_init() only initializes the watchdog subsystem — it does
 * not subscribe the calling task. app_main() returns (and its task gets
 * deleted) shortly after calling this, so subscribing it would risk a
 * spurious trigger; long-running tasks (e.g. the input-polling task)
 * must call esp_task_wdt_add(NULL) for themselves and then
 * mc_watchdog_feed() periodically. */
void mc_watchdog_init(void);
void mc_watchdog_feed(void);
