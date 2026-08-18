package com.motoctrl.app.ble

import android.content.Intent
import android.os.Build
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod

/**
 * JS bridge for BleWatchService. Two calls, both fire-and-forget (no
 * promise/callback -- the service's own notification is the only feedback
 * either side needs, and neither call has a failure mode JS could usefully
 * react to). Driven entirely by BoardSession's state machine, wired up in
 * index.js -- this module has no BLE-state opinion of its own.
 */
class BleWatchModule(reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

    override fun getName(): String = "BleWatchService"

    /** Starts the foreground service if it isn't running, or just updates
     * its notification text if it already is -- see BleWatchService's
     * onStartCommand, which handles both the same way. */
    @ReactMethod
    fun ensureRunning(label: String) {
        val context = reactApplicationContext
        val intent = Intent(context, BleWatchService::class.java).apply {
            putExtra(BleWatchService.EXTRA_LABEL, label)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(intent)
        } else {
            context.startService(intent)
        }
    }

    @ReactMethod
    fun stop() {
        reactApplicationContext.stopService(Intent(reactApplicationContext, BleWatchService::class.java))
    }
}
