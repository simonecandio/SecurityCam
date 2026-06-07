package com.securitycam.util

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import com.securitycam.R
import com.securitycam.model.DetectionCategories

object NotificationHelper {

    private const val CHANNEL_ID = "security_alerts"

    fun createChannel(context: Context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                context.getString(R.string.notif_channel_name),
                NotificationManager.IMPORTANCE_HIGH
            ).apply {
                description = context.getString(R.string.notif_channel_desc)
                enableVibration(true)
            }
            val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            manager.createNotificationChannel(channel)
        }
    }

    fun sendDetectionNotification(
        context: Context,
        deviceName: String,
        eventId: String,
        category: String,
        deviceId: String = "",
        ownerUid: String = ""
    ) {
        val (titleRes, textRes, icon) = when (category) {
            DetectionCategories.PERSON -> Triple(
                R.string.notif_person_title,
                R.string.notif_person_text,
                android.R.drawable.ic_dialog_alert
            )
            DetectionCategories.DOG -> Triple(
                R.string.notif_dog_title,
                R.string.notif_dog_text,
                android.R.drawable.ic_dialog_info
            )
            DetectionCategories.CAT -> Triple(
                R.string.notif_cat_title,
                R.string.notif_cat_text,
                android.R.drawable.ic_dialog_info
            )
            else -> Triple(
                R.string.notif_generic_title,
                R.string.notif_generic_text,
                android.R.drawable.ic_dialog_alert
            )
        }

        val intent = context.packageManager.getLaunchIntentForPackage(context.packageName)?.apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK
            putExtra("navigate_to", "events")
            putExtra("event_id", eventId)
            putExtra("device_id", deviceId)
            putExtra("device_name", deviceName)
            putExtra("owner_uid", ownerUid)
        }
        val pendingIntent = PendingIntent.getActivity(
            context, eventId.hashCode(), intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(icon)
            .setContentTitle(context.getString(titleRes))
            .setContentText(context.getString(textRes, deviceName))
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setAutoCancel(true)
            .setContentIntent(pendingIntent)
            .setVibrate(longArrayOf(0, 500, 200, 500))
            .build()

        try {
            NotificationManagerCompat.from(context).notify(
                eventId.hashCode(),
                notification
            )
        } catch (_: SecurityException) {}
    }

    fun sendPersonDetectedNotification(
        context: Context,
        deviceName: String,
        eventId: String
    ) {
        sendDetectionNotification(context, deviceName, eventId, DetectionCategories.PERSON)
    }
}