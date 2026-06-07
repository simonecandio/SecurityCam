package com.securitycam.repository

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Base64
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.DocumentSnapshot
import com.google.firebase.firestore.FirebaseFirestore
import com.google.firebase.firestore.ListenerRegistration
import com.google.firebase.firestore.Query
import com.securitycam.model.EventFrame
import com.securitycam.model.SecurityEvent
import kotlinx.coroutines.tasks.await

/**
 * Repository per eventi Firestore con struttura nestata:
 * users/{uid}/devices/{device_id}/events/evt_XXXXXX/frames/frame_N
 */
class EventRepository {

    private val db = FirebaseFirestore.getInstance()
    private val auth = FirebaseAuth.getInstance()
    private var snapshotListener: ListenerRegistration? = null

    // uid dell'owner dei dati: vuoto = usa il mio uid
    private var dataOwnerUid: String = ""

    // cursore per la paginazione: ultimo documento caricato
    private var lastLoadedDocument: DocumentSnapshot? = null

    fun setDataOwner(ownerUid: String) {
        dataOwnerUid = ownerUid
    }

    private fun effectiveUid(): String {
        return if (dataOwnerUid.isNotEmpty()) dataOwnerUid
        else auth.currentUser?.uid ?: ""
    }

    private fun eventsPath(deviceId: String): String {
        val uid = effectiveUid()
        if (uid.isEmpty()) return ""
        return "users/$uid/devices/$deviceId/events"
    }

    /**
     * Carica i primi N eventi, ordinati per event_id decrescente.
     * Resetta il cursore di paginazione.
     */
    suspend fun getRecentEvents(deviceId: String, limit: Int = 20): List<SecurityEvent> {
        val path = eventsPath(deviceId)
        android.util.Log.d("EVT_REPO",
            "getRecentEvents path='$path' limit=$limit dataOwnerUid='$dataOwnerUid'")
        if (path.isEmpty()) {
            android.util.Log.w("EVT_REPO", "Path vuoto, ritorno lista vuota")
            return emptyList()
        }

        // resetto il cursore
        lastLoadedDocument = null

        return try {
            val snapshot = db.collection(path)
                .orderBy("event_id", Query.Direction.DESCENDING)
                .limit(limit.toLong())
                .get()
                .await()

            android.util.Log.d("EVT_REPO",
                "Query OK: ${snapshot.documents.size} documenti")
            snapshot.documents.forEach { doc ->
                android.util.Log.d("EVT_REPO",
                    "  doc.id=${doc.id} event_id=${doc.getLong("event_id")} " +
                            "ts=${doc.getLong("timestamp")}")
            }

            // salvo l'ultimo documento per la paginazione
            if (snapshot.documents.isNotEmpty()) {
                lastLoadedDocument = snapshot.documents.last()
            }

            snapshot.documents.map { doc -> docToEvent(doc) }
        } catch (e: Exception) {
            android.util.Log.e("EVT_REPO", "getRecentEvents FAILED: ${e.message}", e)
            emptyList()
        }
    }

    /**
     * Carica i prossimi N eventi piu' vecchi (paginazione).
     * Usa il cursore salvato da getRecentEvents o dal loadMore precedente.
     * Ritorna lista vuota se non ci sono altri eventi.
     */
    suspend fun loadMoreEvents(deviceId: String, limit: Int = 20): List<SecurityEvent> {
        val path = eventsPath(deviceId)
        val cursor = lastLoadedDocument
        if (path.isEmpty() || cursor == null) return emptyList()

        return try {
            val snapshot = db.collection(path)
                .orderBy("event_id", Query.Direction.DESCENDING)
                .startAfter(cursor)
                .limit(limit.toLong())
                .get()
                .await()

            if (snapshot.documents.isNotEmpty()) {
                lastLoadedDocument = snapshot.documents.last()
            }

            snapshot.documents.map { doc -> docToEvent(doc) }
        } catch (e: Exception) {
            emptyList()
        }
    }

    private fun docToEvent(doc: DocumentSnapshot): SecurityEvent {
        return SecurityEvent(
            id = doc.id,
            event_id = doc.getLong("event_id") ?: 0,
            timestamp = doc.getLong("timestamp") ?: 0,
            frame_count = (doc.getLong("frame_count") ?: 0).toInt(),
            pir_value = (doc.getLong("pir_value") ?: 0).toInt(),
            validated = doc.getBoolean("validated") ?: false
        )
    }

    /**
     * Entra nella sottocollezione frames di un evento preciso e vado a ordinare
     * per frames_index in modo decrescente
     */
    suspend fun getEventFrames(deviceId: String, eventId: String): List<EventFrame> {
        val uid = effectiveUid()
        android.util.Log.d("EVT_REPO", "getEventFrames uid='$uid' deviceId='$deviceId' eventId='$eventId'")
        if (uid.isEmpty()) {
            android.util.Log.w("EVT_REPO", "uid vuoto, ritorno lista vuota")
            return emptyList()
        }

        return try {
            val path = "users/$uid/devices/$deviceId/events/$eventId/frames"
            android.util.Log.d("EVT_REPO", "getEventFrames path=$path")
            val snapshot = db.collection(path)
                .orderBy("frame_index")
                .get()
                .await()
            android.util.Log.d("EVT_REPO", "getEventFrames OK, ${snapshot.documents.size} frame")

            snapshot.documents.map { doc ->
                EventFrame(
                    id = doc.id,
                    image_base64 = doc.getString("image_base64") ?: "",
                    jpeg_size = (doc.getLong("jpeg_size") ?: 0).toInt(),
                    frame_index = (doc.getLong("frame_index") ?: 0).toInt()
                )
            }
        } catch (e: Exception) {
            android.util.Log.e("EVT_REPO", "getEventFrames FAILED: ${e.message}", e)
            emptyList()
        }
    }

    /**
     * Converto in Bitmap perchè su firestore è salvato in base64
     */
    fun decodeFrame(base64String: String): Bitmap? {
        return try {
            val bytes = Base64.decode(base64String, Base64.DEFAULT)
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        } catch (e: Exception) {
            null
        }
    }

    /**
     * Aggiorno il campo booleano "validated" di un evento
     */
    suspend fun updateValidation(deviceId: String, eventId: String, personDetected: Boolean) {
        val uid = effectiveUid()
        if (uid.isEmpty()) return
        try {
            db.document("users/$uid/devices/$deviceId/events/$eventId")
                .update("validated", personDetected)
                .await()
        } catch (_: Exception) {}
    }

    /**
     * Prima di registrare il nuovo listener chiama stopListening() per evitare
     * listener duplicati.
     *
     * Il listener e' limitato all'ultimo evento (limit(1)) perche' ci interessa
     * solo sapere quando ne arriva uno nuovo, non rileggere tutto lo storico.
     */
    fun startListening(deviceId: String, onNewEvent: (SecurityEvent) -> Unit) {
        stopListening()
        val path = eventsPath(deviceId)
        if (path.isEmpty()) return

        snapshotListener = db.collection(path)
            .orderBy("event_id", Query.Direction.DESCENDING)
            .limit(1)
            .addSnapshotListener { snapshots, error ->
                if (error != null || snapshots == null) return@addSnapshotListener
                for (change in snapshots.documentChanges) {
                    if (change.type == com.google.firebase.firestore.DocumentChange.Type.ADDED) {
                        val doc = change.document
                        onNewEvent(docToEvent(doc))
                    }
                }
            }
    }

    fun stopListening() {
        snapshotListener?.remove()
        snapshotListener = null
    }
}