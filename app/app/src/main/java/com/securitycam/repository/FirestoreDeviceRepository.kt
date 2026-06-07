package com.securitycam.repository

import android.util.Log
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.FirebaseFirestore
import com.google.firebase.firestore.WriteBatch
import com.securitycam.model.Device
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.tasks.await
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
/**
 * Repository per i DEVICE su Firestore, gestisce i device come entità.
 * Tutto quello che riguarda la registrazione e gestione dei dispositivi nell'account
 */

class FirestoreDeviceRepository {

    private val db = FirebaseFirestore.getInstance()
    private val auth = FirebaseAuth.getInstance()

    private fun devicesCollection() = db.collection("users/${auth.currentUser?.uid}/devices")

    /**
     * Vado a leggere tutti i device dell'utente loggato, ordinati dal più recente al più vecchio
     * in sostanza, per ogni documento letto si costruisce un oggetto device
     */
    suspend fun getDevices(): List<Device> {
        val uid = auth.currentUser?.uid ?: return emptyList()
        return try {
            val snapshot = devicesCollection()
                .orderBy("addedAt", com.google.firebase.firestore.Query.Direction.DESCENDING)
                .get()
                .await()

            snapshot.documents.map { doc ->
                @Suppress("UNCHECKED_CAST")
                val categories = doc.get("monitorCategories") as? List<String> ?: listOf("person")
                Device(
                    id = doc.id,
                    name = doc.getString("name") ?: "Senza nome",
                    ip = doc.getString("ip") ?: "",
                    addedAt = doc.getLong("addedAt") ?: 0,
                    lastSeen = doc.getLong("lastSeen") ?: 0,
                    monitorCategories = categories,
                    ownerUid = doc.getString("ownerUid") ?: ""
                )
            }
        } catch (e: Exception) {
            emptyList()
        }
    }

    suspend fun addDevice(deviceId: String, name: String, ip: String, categories: List<String> = listOf("person")): Boolean {
        return try {
            val data = hashMapOf(
                "name" to name,
                "ip" to ip,
                "addedAt" to System.currentTimeMillis(),
                "lastSeen" to 0L,
                "monitorCategories" to categories
            )
            devicesCollection().document(deviceId).set(data).await()
            true
        } catch (e: Exception) {
            false
        }
    }

    suspend fun updateMonitorCategories(deviceId: String, categories: List<String>): Boolean {
        return try {
            devicesCollection().document(deviceId).update("monitorCategories", categories).await()
            true
        } catch (e: Exception) {
            false
        }
    }

    suspend fun removeDevice(deviceId: String): Boolean {
        return try {
            devicesCollection().document(deviceId).delete().await()
            true
        } catch (e: Exception) {
            false
        }
    }

    suspend fun updateDeviceIp(deviceId: String, ip: String) {
        try {
            devicesCollection().document(deviceId).update("ip", ip).await()
        } catch (_: Exception) {}
    }

    /**
     * Al primo login vado a creare il documento su firebase con udi in users/{uid}
     */
    suspend fun ensureUserDocument() {
        val user = auth.currentUser ?: return
        try {
            val doc = db.document("users/${user.uid}").get().await()
            if (!doc.exists()) {
                db.document("users/${user.uid}").set(
                    hashMapOf(
                        "email" to (user.email ?: ""),
                        "createdAt" to System.currentTimeMillis()
                    )
                ).await()
            }
        } catch (_: Exception) {}
    }

    /**
     * Elimina un dispositivo e TUTTI i suoi dati.
     *
     * Cancello PRIMA il documento principale del device, POI
     * faccio cleanup best-effort delle sottocollezioni.
     *
     * Questo perchè il documento principale users/{uid}/devices/{id} e' l'unico
     * che determina se il device appare nella lista (getDevices() filtra
     * per addedAt). Le varie sottocollezioni residue creano solo dei documenti fantasma
     * , che le query Firestore con orderBy NON ritornano.
     *
     * Se il cleanup delle sottocollezioni fallisce a meta' per timeout
     * (puo' succedere con molti eventi e frame base64), il device NON
     * ricompare nella lista alla riapertura dell'app perche' il documento
     * principale e' gia' stato cancellato come prima cosa.
     */
    suspend fun removeDeviceWithData(deviceId: String): Boolean = withContext(Dispatchers.IO) {
        val TAG = "REMOVE_FS"
        Log.d(TAG, "=== START removeDeviceWithData(deviceId=$deviceId) ===")

        val uid = auth.currentUser?.uid
        if (uid == null) {
            Log.e(TAG, "FAIL: utente non autenticato")
            return@withContext false
        }
        if (deviceId.isBlank()) {
            Log.e(TAG, "FAIL: deviceId vuoto")
            return@withContext false
        }

        try {
            // === STEP 0: gestione device condivisi ===
            // Se e' un device che mi e' stato condiviso da un altro utente,
            // cancello SOLO il riferimento sotto il mio uid; i dati reali
            // sono sotto l'uid dell'owner e non li tocco.
            val deviceDoc = devicesCollection().document(deviceId).get().await()
            val ownerUid = deviceDoc.getString("ownerUid") ?: ""
            if (ownerUid.isNotEmpty()) {
                Log.d(TAG, "Device condiviso, cancello solo il riferimento locale")
                devicesCollection().document(deviceId).delete().await()
                Log.d(TAG, "=== FINE OK (condiviso) ===")
                return@withContext true
            }

            // === STEP 1: cancello SUBITO il documento principale ===
            // Operazione singola, atomica, veloce (~200ms). Se fallisce
            // ritorno false cosi' la UI puo' riprovare. Se va a buon fine,
            // anche se tutto il resto fallisse, il device e' invisibile
            // nell'app.
            try {
                val t0 = System.currentTimeMillis()
                devicesCollection().document(deviceId).delete().await()
                Log.d(TAG, "Documento principale cancellato in " +
                        "${System.currentTimeMillis() - t0}ms")
            } catch (e: Exception) {
                Log.e(TAG, "FATALE: cancellazione doc principale fallita: ${e.message}", e)
                return@withContext false
            }

            // === STEP 2: cleanup best-effort delle sottocollezioni ===
            // Da qui in poi, anche se qualcosa fallisce, restano dati orfani
            // su Firestore ma il device NON ricompare nella lista.
            cleanupDeviceSubcollections(uid, deviceId)

            Log.d(TAG, "=== FINE OK ===")
            true
        } catch (e: Exception) {
            Log.e(TAG, "ERRORE GENERALE: ${e.message}", e)
            false
        }
    }

    /**
     * Cleanup best-effort: cancella eventi, frame, status, pending_config.
     */
    private suspend fun cleanupDeviceSubcollections(uid: String, deviceId: String) {
        val TAG = "REMOVE_FS"
        val basePath = "users/$uid/devices/$deviceId"

        try {
            val refsToDelete = mutableListOf<com.google.firebase.firestore.DocumentReference>()

            // Eventi + frame in parallelo
            val eventsSnap = try {
                db.collection("$basePath/events").get().await()
            } catch (e: Exception) {
                Log.w(TAG, "Cleanup: lettura eventi fallita: ${e.message}")
                null
            }

            if (eventsSnap != null) {
                Log.d(TAG, "Cleanup: ${eventsSnap.size()} eventi da pulire")
                val t0 = System.currentTimeMillis()
                val frameRefs = coroutineScope {
                    eventsSnap.documents.map { eventDoc ->
                        async {
                            try {
                                db.collection("$basePath/events/${eventDoc.id}/frames")
                                    .get().await()
                                    .documents.map { it.reference }
                            } catch (_: Exception) {
                                emptyList()
                            }
                        }
                    }.awaitAll().flatten()
                }
                Log.d(TAG, "Cleanup: letti ${frameRefs.size} frame in " +
                        "${System.currentTimeMillis() - t0}ms")
                refsToDelete.addAll(frameRefs)
                refsToDelete.addAll(eventsSnap.documents.map { it.reference })
            }

            try {
                db.collection("$basePath/status").get().await()
                    .documents.forEach { refsToDelete.add(it.reference) }
            } catch (_: Exception) {}
            try {
                db.collection("$basePath/pending_config").get().await()
                    .documents.forEach { refsToDelete.add(it.reference) }
            } catch (_: Exception) {}

            // Batch da 400. Se uno fallisce, continuo con i successivi
            // (best-effort, NON return false).
            val chunks = refsToDelete.chunked(400)
            Log.d(TAG, "Cleanup: ${refsToDelete.size} doc in ${chunks.size} batch")

            for ((index, chunk) in chunks.withIndex()) {
                val tBatch = System.currentTimeMillis()
                val ok = withTimeoutOrNull(15_000) {
                    val batch: WriteBatch = db.batch()
                    for (ref in chunk) batch.delete(ref)
                    batch.commit().await()
                    true
                }
                val elapsed = System.currentTimeMillis() - tBatch
                if (ok == null) {
                    Log.w(TAG, "Cleanup: batch ${index + 1}/${chunks.size} " +
                            "TIMEOUT dopo ${elapsed}ms (ignoro)")
                } else {
                    Log.d(TAG, "Cleanup: batch ${index + 1}/${chunks.size} " +
                            "OK in ${elapsed}ms")
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Cleanup: errore generale (ignoro): ${e.message}")
        }
    }

    // === SISTEMA DI INVITI ===

    suspend fun createInvite(device: Device): String? {
        val uid = auth.currentUser?.uid ?: return null
        return try {
            var code: String
            do {
                code = (100000..999999).random().toString()
                val existing = db.document("invites/$code").get().await()
            } while (existing.exists())

            val data = hashMapOf(
                "ownerUid" to uid,
                "deviceId" to device.id,
                "deviceName" to device.name,
                "deviceIp" to device.ip,
                "monitorCategories" to device.monitorCategories,
                "createdAt" to System.currentTimeMillis(),
                "expiresAt" to (System.currentTimeMillis() + 24 * 60 * 60 * 1000)
            )
            db.document("invites/$code").set(data).await()
            code
        } catch (e: Exception) {
            null
        }
    }

    suspend fun acceptInvite(code: String): Result<String> {
        val uid = auth.currentUser?.uid ?: return Result.failure(Exception("Non autenticato"))
        return try {
            val doc = db.document("invites/$code").get().await()
            if (!doc.exists()) {
                return Result.failure(Exception("Codice non valido"))
            }

            val expiresAt = doc.getLong("expiresAt") ?: 0
            if (System.currentTimeMillis() > expiresAt) {
                db.document("invites/$code").delete().await()
                return Result.failure(Exception("Codice scaduto"))
            }

            val ownerUid = doc.getString("ownerUid") ?: ""
            val deviceId = doc.getString("deviceId") ?: ""
            val deviceName = doc.getString("deviceName") ?: "Camera"
            val deviceIp = doc.getString("deviceIp") ?: ""
            @Suppress("UNCHECKED_CAST")
            val categories = doc.get("monitorCategories") as? List<String> ?: listOf("person")

            if (ownerUid == uid) {
                return Result.failure(Exception("Questo dispositivo è già tuo"))
            }

            val existing = devicesCollection().document(deviceId).get().await()
            if (existing.exists()) {
                return Result.failure(Exception("Dispositivo già presente"))
            }

            val data = hashMapOf(
                "name" to deviceName,
                "ip" to deviceIp,
                "addedAt" to System.currentTimeMillis(),
                "lastSeen" to 0L,
                "monitorCategories" to categories,
                "ownerUid" to ownerUid
            )
            devicesCollection().document(deviceId).set(data).await()

            db.document("invites/$code").delete().await()

            Result.success(deviceName)
        } catch (e: Exception) {
            Result.failure(e)
        }
    }
    // === FACTORY RESET REMOTO ===

    /**
     * Scrive un comando di factory reset su Firestore che l'ESP
     * leggera' al prossimo check_remote_config (~30s).
     *
     * Usa lo stesso canale gia' usato per la configurazione remota
     * (cooldown, frame_count, jpeg_quality). L'ESP nel suo
     * firebase_check_remote_config() controlla se c'e' il campo
     * "factory_reset" a true e, se si', esegue un wipe completo
     * dell'NVS e riavvia in modalita' SoftAP.
     *
     * Ritorna true se la scrittura su Firestore e' andata a buon fine.
     */
    suspend fun requestRemoteFactoryReset(deviceId: String): Boolean {
        val uid = auth.currentUser?.uid ?: return false
        return try {
            val data = hashMapOf(
                "factory_reset" to true,
                "timestamp" to System.currentTimeMillis()
            )
            db.document("users/$uid/devices/$deviceId/pending_config/current")
                .set(data)
                .await()
            true
        } catch (e: Exception) {
            Log.e("REMOVE_FS", "requestRemoteFactoryReset fallito: ${e.message}")
            false
        }
    }

}