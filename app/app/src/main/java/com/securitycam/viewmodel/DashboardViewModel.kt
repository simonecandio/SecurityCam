package com.securitycam.viewmodel

import android.content.Context
import android.graphics.BitmapFactory
import android.util.Base64
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.FirebaseFirestore
import com.google.firebase.firestore.ListenerRegistration
import com.google.firebase.firestore.Query
import com.securitycam.model.DetectionCategories
import com.securitycam.model.DeviceStatus
import com.securitycam.repository.DeviceRepository
import com.securitycam.repository.FirestoreDeviceRepository
import com.securitycam.repository.MlKitRepository
import com.securitycam.util.NotificationHelper
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await


data class DashboardUiState(
    val status: DeviceStatus? = null,
    val isDeviceOnline: Boolean = false,
    val isLoading: Boolean = false,
    val error: String? = null,
    val deviceIp: String = "",
    val deviceName: String = "",
    val deviceId: String = "",
    val isRemoteStatus: Boolean = false,
    val isSharedDevice: Boolean = false,
    val statusLoaded: Boolean = false
)

class DashboardViewModel : ViewModel() {

    private val deviceRepo = DeviceRepository()
    private val firestoreRepo = FirestoreDeviceRepository()
    private val mlKitRepo = MlKitRepository()
    private val db = FirebaseFirestore.getInstance()
    private var pollingActive = false
    private var lastRemoteUptime: Long = -1
    // Inizializzato a 0 (non a System.currentTimeMillis()!) cosi' alla
    // prima lettura da Firestore, se l'uptime non e' MAI cambiato,
    // il check "now - 0 < 90s" e' SEMPRE false e il device resta offline.
    // Solo quando l'uptime cambia davvero (= l'ESP sta scrivendo
    // attivamente) questo valore viene aggiornato a now.
    private var lastUptimeChangeTime: Long = 0L
    private var eventListener: ListenerRegistration? = null
    private val analyzedEventIds = mutableSetOf<String>()
    private var monitorCategories: List<String> = listOf("person")
    private var appContext: Context? = null

    // uid dell'owner reale dei dati: per device di proprietà è il mio uid,
    // per device condivisi è l'uid di chi ha condiviso
    private var dataOwnerUid: String = ""


    private val _uiState = MutableStateFlow(DashboardUiState())
    val uiState: StateFlow<DashboardUiState> = _uiState.asStateFlow()

    /**
     * Imposta il device da monitorare.
     * ownerUid: se non vuoto, i dati (eventi/status) vengono letti
     * dal path Firestore dell'owner invece che dal proprio.
     */
    fun setDevice(name: String, ip: String, deviceId: String = "",
                  ownerUid: String = "", context: Context? = null) {
        deviceRepo.updateDeviceIp(ip)
        appContext = context?.applicationContext

        // determino l'uid da usare per leggere i dati
        val myUid = FirebaseAuth.getInstance().currentUser?.uid ?: ""
        dataOwnerUid = if (ownerUid.isNotEmpty()) ownerUid else myUid

        _uiState.value = _uiState.value.copy(
            deviceIp = ip,
            deviceName = name,
            deviceId = deviceId,
            isSharedDevice = ownerUid.isNotEmpty()
        )
        loadMonitorCategories(deviceId)
    }

    /** Path base per leggere i dati del device su Firestore */
    private fun deviceBasePath(deviceId: String): String {
        return "users/$dataOwnerUid/devices/$deviceId"
    }

    private fun loadMonitorCategories(deviceId: String) {
        viewModelScope.launch {
            try {
                val devices = firestoreRepo.getDevices()
                val device = devices.find { it.id == deviceId }
                monitorCategories = device?.monitorCategories ?: listOf("person")
            } catch (_: Exception) {}
        }
    }


    private var pollingJob: Job? = null
    fun startPolling() {
        if (pollingJob?.isActive == true) {
            return
        }
        pollingActive = true

        startEventListener()
        pollingJob = viewModelScope.launch {
            refreshStatus()       // prima lettura (salva uptime)
            delay(20000)
            refreshStatus()       // seconda lettura (confronta)
            while (pollingActive) {
                delay(5000)
                refreshStatus()
            }
        }
    }

    fun stopPolling() {
        pollingActive = false
        pollingJob?.cancel()
        pollingJob = null
        eventListener?.remove()
        eventListener = null
    }
/* Vecchio polling non si aggiornava correttamente
    fun startPolling() {
        if (pollingActive) return
        pollingActive = true
        startEventListener()
        viewModelScope.launch {
            while (pollingActive) {
                refreshStatus()
                delay(5000)
            }
        }
    }

    fun stopPolling() {
        pollingActive = false
        eventListener?.remove()
        eventListener = null
    }
*/

    /**
     * Listener real-time sugli eventi Firestore.
     * Usa il path dell'owner per i device condivisi.
     */
    private fun startEventListener() {
        val deviceId = _uiState.value.deviceId
        if (deviceId.isEmpty() || dataOwnerUid.isEmpty()) return

        eventListener?.remove()
        val basePath = deviceBasePath(deviceId)

        eventListener = db.collection("$basePath/events")
            .orderBy("timestamp", Query.Direction.DESCENDING)
            .limit(5)
            .addSnapshotListener { snapshot, error ->
                if (error != null || snapshot == null) return@addSnapshotListener

                for (change in snapshot.documentChanges) {
                    if (change.type == com.google.firebase.firestore.DocumentChange.Type.ADDED) {
                        val doc = change.document
                        val eventId = doc.id
                        val validated = doc.getBoolean("validated") ?: false

                        if (!validated && !analyzedEventIds.contains(eventId)) {
                            analyzedEventIds.add(eventId)
                            viewModelScope.launch {
                                analyzeEvent(dataOwnerUid, deviceId, eventId)
                            }
                        }
                    }
                }
            }
    }

    private suspend fun analyzeEvent(ownerUid: String, deviceId: String, eventId: String) {
        try {
            val basePath = "users/$ownerUid/devices/$deviceId"
            val frames = db.collection("$basePath/events/$eventId/frames")
                .get().await()

            val allDetected = mutableSetOf<String>()

            for (frameDoc in frames.documents) {
                val b64 = frameDoc.getString("image_base64") ?: continue
                try {
                    val bytes = Base64.decode(b64, Base64.DEFAULT)
                    val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: continue
                    val result = mlKitRepo.detectAll(bitmap, 0)
                    allDetected.addAll(result.detectedCategories.filter { it in monitorCategories })
                } catch (_: Exception) {}
            }

            val hasDetection = allDetected.isNotEmpty()
            db.document("$basePath/events/$eventId")
                .update("validated", hasDetection).await()

            val matched = allDetected.filter { it in monitorCategories }
            if (matched.isNotEmpty()) {
                appContext?.let { ctx ->
                    NotificationHelper.sendDetectionNotification(
                        ctx, _uiState.value.deviceName, eventId, matched.first(),
                        deviceId = _uiState.value.deviceId, ownerUid = dataOwnerUid
                    )
                }
            }
        } catch (_: Exception) {}
    }

    suspend fun refreshStatus() {
            _uiState.value = _uiState.value.copy(isLoading = true)

            // prova prima HTTP diretto (solo se sulla stessa rete)
            val result = deviceRepo.getStatus()
            if (result.isSuccess) {
                lastRemoteUptime = -1
                lastUptimeChangeTime = System.currentTimeMillis()
                _uiState.value = _uiState.value.copy(
                    status = result.getOrNull(),
                    isDeviceOnline = true,
                    isLoading = false,
                    statusLoaded = true,
                    error = null,
                    isRemoteStatus = false
                )
                return
            }

            // fallback: leggi da Firestore (path dell'owner per device condivisi)
            val deviceId = _uiState.value.deviceId
            if (dataOwnerUid.isNotEmpty() && deviceId.isNotEmpty()) {
                try {
                    val basePath = deviceBasePath(deviceId)
                    val doc = db.document("$basePath/status/current")
                        .get().await()

                    if (doc.exists()) {
                        val currentUptime = doc.getLong("uptime_s") ?: 0
                        val timeStr = doc.getString("time") ?: ""

                        // FIX: la primissima volta che leggo lo status remoto
                        // NON lo considero "alive" solo perche' il documento
                        // esiste. Altrimenti un ESP spento da giorni, con
                        // status/current rimasto da prima, verrebbe marcato
                        // online per 90 secondi appena apro la dashboard.
                        //
                        // La logica corretta: "alive" = "l'uptime e' cambiato
                        // in una delle letture FATTE DA QUESTA ISTANZA del
                        // ViewModel negli ultimi 90s". Se l'uptime non cambia
                        // significa che l'ESP non sta piu' scrivendo, quindi
                        // e' offline.
                        //
                        // La prima lettura di questa sessione e' "indecisa":
                        // non so se l'ESP e' vivo o se sto leggendo un valore
                        // stantio. Aspetto almeno un secondo update per dare
                        // verdetto.
                        val isFirstRead = lastRemoteUptime < 0
                        if (isFirstRead) {
                            // Prima lettura: salvo l'uptime ma NON azzero
                            // lastUptimeChangeTime (cosi' se l'ESP non si
                            // aggiorna piu', dopo 90s dall'apertura del VM
                            // passa a offline).
                            lastRemoteUptime = currentUptime
                        } else if (currentUptime != lastRemoteUptime) {
                            // L'uptime e' cambiato da una lettura all'altra:
                            // l'ESP e' vivo e sta aggiornando.
                            lastUptimeChangeTime = System.currentTimeMillis()
                            lastRemoteUptime = currentUptime
                        }
                        // Se currentUptime == lastRemoteUptime, non tocco
                        // nulla: lastUptimeChangeTime resta al valore
                        // precedente e il check "< 90s" gestira' tutto.

                        val isStillAlive =
                                (System.currentTimeMillis() - lastUptimeChangeTime) < 25_000

                        val status = DeviceStatus(
                            device = "SecurityCam-ESP32S3",
                            firmware = "1.0.0",
                            uptime_s = currentUptime,
                            free_heap = doc.getLong("free_heap") ?: 0,
                            camera_ok = doc.getBoolean("camera_ok") ?: false,
                            pir_ok = doc.getBoolean("pir_ok") ?: false,
                            sd_card_ok = doc.getBoolean("sd_card_ok") ?: false,
                            wifi_ok = doc.getBoolean("wifi_ok") ?: false,
                            firebase_ok = doc.getBoolean("firebase_ok") ?: false,
                            total_events = (doc.getLong("total_events") ?: 0).toInt(),
                            failed_uploads = (doc.getLong("failed_uploads") ?: 0).toInt(),
                            state = doc.getString("state") ?: "unknown",
                            ip = _uiState.value.deviceIp,
                            time = timeStr
                        )
                        val finalStatus = if (isStillAlive) status else status.copy(
                            camera_ok = false, pir_ok = false, sd_card_ok = false,
                            wifi_ok = false, firebase_ok = false, state = "offline"
                        )
                        _uiState.value = _uiState.value.copy(
                            status = finalStatus,
                            isDeviceOnline = isStillAlive,
                            isLoading = false,
                            statusLoaded = !isFirstRead,
                            error = if (!isStillAlive) "Dispositivo non aggiornato da tempo" else null,
                            isRemoteStatus = true
                        )
                        return
                    }
                } catch (_: Exception) { }
            }

            _uiState.value = _uiState.value.copy(
                isDeviceOnline = false,
                isLoading = false,
                error = if (_uiState.value.status == null) "Dispositivo non raggiungibile" else null
            )

    }

    override fun onCleared() {
        stopPolling()
    }
}