package com.securitycam.util

import android.content.Context
import android.graphics.BitmapFactory
import android.util.Base64
import android.util.Log
import androidx.work.CoroutineWorker
import androidx.work.WorkerParameters
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.FirebaseFirestore
import com.google.firebase.firestore.Query
import com.securitycam.repository.MlKitRepository
import kotlinx.coroutines.tasks.await

/**
 * Worker che gira periodicamente quando l'app e' chiusa per analizzare
 * eventi non ancora validati e inviare notifiche push.
 *
 * Usa lo STESSO MlKitRepository che usa l'app in foreground, cosi' la
 * qualita' del rilevamento e' identica tra i due scenari.
 */
class EventCheckWorker(
    context: Context,
    params: WorkerParameters
) : CoroutineWorker(context, params) {

    override suspend fun doWork(): Result {
        val uid = FirebaseAuth.getInstance().currentUser?.uid ?: return Result.success()
        val db = FirebaseFirestore.getInstance()

        // Uso lo stesso repository dell'app (stesse soglie, stesso detector)
        val mlKit = MlKitRepository()

        try {
            val devices = db.collection("users/$uid/devices").get().await()

            for (deviceDoc in devices.documents) {
                val deviceId = deviceDoc.id
                val deviceName = deviceDoc.getString("name") ?: "Camera"

                @Suppress("UNCHECKED_CAST")
                val monitorCategories = deviceDoc.get("monitorCategories") as? List<String>
                    ?: listOf("person")

                val events = db.collection("users/$uid/devices/$deviceId/events")
                    .orderBy("timestamp", Query.Direction.DESCENDING)
                    .limit(5)
                    .get()
                    .await()

                for (eventDoc in events.documents) {
                    val validated = eventDoc.getBoolean("validated")
                    if (validated == true) continue

                    val frames = db.collection(
                        "users/$uid/devices/$deviceId/events/${eventDoc.id}/frames"
                    ).get().await()

                    val allDetectedCategories = mutableSetOf<String>()

                    for ((idx, frameDoc) in frames.documents.withIndex()) {
                        val b64 = frameDoc.getString("image_base64") ?: continue
                        try {
                            val bytes = Base64.decode(b64, Base64.DEFAULT)
                            val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                                ?: continue

                            // Qui uso detectAll() IDENTICO a quello che usa
                            // EventsViewModel.autoAnalyzeEvent in foreground.
                            val result = mlKit.detectAll(bitmap, idx)
                            allDetectedCategories.addAll(
                                result.detectedCategories.filter { it in monitorCategories }
                            )
                        } catch (e: Exception) {
                            Log.e("EventCheckWorker", "ML Kit error: ${e.message}")
                        }
                    }

                    // aggiorno Firestore
                    val hasDetection = allDetectedCategories.isNotEmpty()
                    db.document("users/$uid/devices/$deviceId/events/${eventDoc.id}")
                        .update("validated", hasDetection)
                        .await()

                    // notifica solo se la categoria rilevata e' tra quelle monitorate
                    val matchedCategories = allDetectedCategories.filter { it in monitorCategories }
                    if (matchedCategories.isNotEmpty()) {
                        val categoryName = matchedCategories.first()
                        NotificationHelper.sendDetectionNotification(
                            applicationContext, deviceName, eventDoc.id, categoryName
                        )
                    }
                }
            }
        } catch (e: Exception) {
            Log.e("EventCheckWorker", "Worker error: ${e.message}")
            return Result.retry()
        }

        return Result.success()
    }
}