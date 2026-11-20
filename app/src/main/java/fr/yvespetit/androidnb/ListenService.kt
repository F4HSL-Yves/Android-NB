package fr.yvespetit.androidnb

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder

/**
 * Keeps the reception going with the app in the background or the screen
 * off: a foreground service (media playback) with its notification, and a
 * Wi-Fi lock so the stream does not stall when the screen turns off. It does
 * no work itself: the connection and the audio live in [MainActivity]'s
 * NbConnection; this only stops Android from freezing or killing the app.
 * Started at START, stopped at STOP. Only the transmission is cut in the
 * background (MainActivity.onStop).
 */
class ListenService : Service() {
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, "Réception", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Réception QO-100 en cours"
                setShowBadge(false)
            },
        )
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val n = Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_headset)
            .setContentTitle("QO-100 NB")
            .setContentText("Réception en cours. STOP dans l'appli pour arrêter.")
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(ID, n)
        }
        if (wifiLock == null) {
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            @Suppress("DEPRECATION")
            val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
            wifiLock = runCatching { wm.createWifiLock(mode, "qo100nb:listen").apply { setReferenceCounted(false); acquire() } }.getOrNull()
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        runCatching { wifiLock?.release() }
        wifiLock = null
        super.onDestroy()
    }

    companion object {
        private const val CHANNEL = "listen"
        private const val ID = 1

        /** Runs the service while [on]; errors (start refused in the background) are ignored. */
        fun set(context: Context, on: Boolean) {
            val i = Intent(context, ListenService::class.java)
            runCatching {
                if (on) context.startForegroundService(i) else context.stopService(i)
            }
        }
    }
}
