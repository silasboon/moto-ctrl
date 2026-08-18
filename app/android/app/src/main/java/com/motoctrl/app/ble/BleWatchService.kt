package com.motoctrl.app.ble

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import com.motoctrl.app.MainActivity
import com.motoctrl.app.R

/**
 * Keeps the app process alive in the background so the JS-side BLE
 * reconnect watcher (src/ble/BoardSession.ts) keeps running: without a
 * foreground service, Android freezes or kills a backgrounded app's
 * process (Doze / App Standby / the general background-execution limits
 * introduced in Android 8+), which would silently stop phone-as-key from
 * ever reconnecting once the app itself isn't on screen.
 *
 * This service does no BLE work of its own -- react-native-ble-plx's
 * GATT calls keep working normally as long as the process is alive, which
 * is the one thing a foreground service buys. It exists purely to hold a
 * persistent, low-priority notification so the rider always has a visible,
 * honest indicator that MOTO-CTRL is watching for their board in the
 * background, and to keep the process in Android's foreground priority
 * bucket while that's true.
 *
 * Started/stopped/updated from JS via BleWatchModule, driven by
 * BoardSession's state machine (index.js wires the two together) --
 * this class has no BLE-state opinion of its own, just a label to show.
 */
class BleWatchService : Service() {
    companion object {
        private const val TAG = "BleWatchService"
        private const val CHANNEL_ID = "moto_ctrl_ble_watch"
        private const val NOTIFICATION_ID = 1001
        const val EXTRA_LABEL = "label"
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val label = intent?.getStringExtra(EXTRA_LABEL) ?: getString(R.string.ble_watch_default_label)
        startInForeground(label)
        /* START_STICKY: if the OS kills this process under memory pressure while a
         * board should still be watched for, ask it to recreate the service (with a
         * null intent -- the default label is used until JS reasserts the real one,
         * which BoardSession.start() does again once the recreated app boots). A
         * killed *foreground* service is a rare event (that's the protection this
         * class exists to provide), so this is a backstop, not the normal path. */
        return START_STICKY
    }

    /* Also called on every ensureRunning() from JS while already running, so the
     * notification text can be updated (watching -> connecting -> connected) without
     * tearing the service down and losing the foreground-priority window. */
    private fun startInForeground(label: String) {
        ensureChannel()
        val notification = buildNotification(label)
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(
                    NOTIFICATION_ID,
                    notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
                )
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
        } catch (e: Exception) {
            /* Never let a foreground-service permission/OEM quirk crash the app --
             * BLE control over BLE still works fine in the foreground either way,
             * this only affects whether reconnect survives backgrounding. */
            Log.e(TAG, "startForeground failed; background reconnect may not survive backgrounding", e)
        }
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = getSystemService(NotificationManager::class.java) ?: return
        if (manager.getNotificationChannel(CHANNEL_ID) != null) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.ble_watch_channel_name),
            NotificationManager.IMPORTANCE_MIN,
        ).apply {
            description = getString(R.string.ble_watch_channel_description)
            setShowBadge(false)
        }
        manager.createNotificationChannel(channel)
    }

    private fun buildNotification(label: String): Notification {
        val openApp = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or
            (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_IMMUTABLE else 0)
        val pendingIntent = PendingIntent.getActivity(this, 0, openApp, flags)

        return Notification.Builder(this, CHANNEL_ID).apply {
            setContentTitle(getString(R.string.app_name))
            setContentText(label)
            setSmallIcon(R.drawable.ic_notification)
            setContentIntent(pendingIntent)
            setOngoing(true)
            setOnlyAlertOnce(true)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                setShowWhen(false)
            }
        }.build()
    }
}
