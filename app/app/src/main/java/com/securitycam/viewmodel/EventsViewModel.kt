package com.securitycam.viewmodel

import android.content.Context
import android.graphics.Bitmap
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.securitycam.model.DetectionCategories
import com.securitycam.model.DetectionResult
import com.securitycam.model.EventFrame
import com.securitycam.model.SecurityEvent
import com.securitycam.repository.EventRepository
import com.securitycam.repository.FirestoreDeviceRepository
import com.securitycam.repository.MlKitRepository
import com.securitycam.util.NotificationHelper
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await

data class EventsUiState(
    val events: List<SecurityEvent> = emptyList(),
    val isLoading: Boolean = false,
    val isLoadingMore: Boolean = false,
    val hasMoreEvents: Boolean = true,
    val error: String? = null,
    val selectedEvent: SecurityEvent? = null,
    val selectedFrames: List<EventFrame> = emptyList(),
    val decodedBitmaps: Map<Int, Bitmap> = emptyMap(),
    val detectionResults: Map<Int, DetectionResult> = emptyMap(),
    val isSharedDevice: Boolean = false
)

class EventsViewModel : ViewModel() {

    private val eventRepo = EventRepository()
    private val mlKitRepo = MlKitRepository()
    private val firestoreRepo = FirestoreDeviceRepository()

    private val _uiState = MutableStateFlow(EventsUiState())
    val uiState: StateFlow<EventsUiState> = _uiState.asStateFlow()

    private var currentDeviceId = ""
    private var currentDeviceName = ""
    private var monitorCategories: List<String> = listOf("person")
    private var appContext: Context? = null
    private val analyzedEventIds = mutableSetOf<String>()
    private var isShared = false
    private var dataOwnerUid: String = ""

    private val PAGE_SIZE = 20

    fun init(context: Context, deviceId: String, deviceName: String, ownerUid: String = "") {
        appContext = context.applicationContext
        currentDeviceId = deviceId
        currentDeviceName = deviceName
        isShared = ownerUid.isNotEmpty()
        dataOwnerUid = ownerUid.ifEmpty {
            com.google.firebase.auth.FirebaseAuth.getInstance().currentUser?.uid ?: ""
        }

        eventRepo.setDataOwner(ownerUid)

        _uiState.value = _uiState.value.copy(isSharedDevice = isShared)
        loadEvents()
        loadMonitorCategories()
        startRealTimeListener()
    }

    private fun loadMonitorCategories() {
        viewModelScope.launch {
            try {
                val devices = firestoreRepo.getDevices()
                val device = devices.find { it.id == currentDeviceId }
                monitorCategories = device?.monitorCategories ?: listOf("person")
            } catch (_: Exception) {}
        }
    }

    fun loadEvents() {
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isLoading = true, error = null)
            try {
                val events = eventRepo.getRecentEvents(currentDeviceId, PAGE_SIZE)
                _uiState.value = _uiState.value.copy(
                    events = events,
                    isLoading = false,
                    hasMoreEvents = events.size >= PAGE_SIZE
                )
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(
                    isLoading = false, error = "Errore: ${e.message}"
                )
            }
        }
    }

    /** Carica la pagina successiva di eventi piu' vecchi */
    fun loadMoreEvents() {
        if (_uiState.value.isLoadingMore || !_uiState.value.hasMoreEvents) return

        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isLoadingMore = true)
            try {
                val moreEvents = eventRepo.loadMoreEvents(currentDeviceId, PAGE_SIZE)
                _uiState.value = _uiState.value.copy(
                    events = _uiState.value.events + moreEvents,
                    isLoadingMore = false,
                    hasMoreEvents = moreEvents.size >= PAGE_SIZE
                )
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(isLoadingMore = false)
            }
        }
    }

    private fun startRealTimeListener() {
        eventRepo.startListening(currentDeviceId) { newEvent ->
            val currentEvents = _uiState.value.events.toMutableList()
            if (currentEvents.none { it.id == newEvent.id }) {
                currentEvents.add(0, newEvent)
                _uiState.value = _uiState.value.copy(events = currentEvents)
            }

            if (!analyzedEventIds.contains(newEvent.id) && !newEvent.validated) {
                analyzedEventIds.add(newEvent.id)
                viewModelScope.launch {
                    autoAnalyzeEvent(newEvent)
                }
            }
        }
    }

    private suspend fun autoAnalyzeEvent(event: SecurityEvent) {
        try {
            val frames = eventRepo.getEventFrames(currentDeviceId, event.id)
            val allDetectedCategories = mutableSetOf<String>()
            var bestLabel = ""
            var maxConf = 0f

            for (frame in frames) {
                val bitmap = eventRepo.decodeFrame(frame.image_base64) ?: continue
                val result = mlKitRepo.detectAll(bitmap, frame.frame_index)
                allDetectedCategories.addAll(result.detectedCategories.filter { it in monitorCategories })
                if (result.confidence > maxConf) {
                    maxConf = result.confidence
                    bestLabel = result.detectedLabel
                }
            }

            val matchedCategories = allDetectedCategories.filter { it in monitorCategories }
            val shouldNotify = matchedCategories.isNotEmpty()
            val shouldValidate = allDetectedCategories.isNotEmpty()

            eventRepo.updateValidation(currentDeviceId, event.id, shouldValidate)

            val updated = _uiState.value.events.map {
                if (it.id == event.id) it.copy(validated = shouldValidate) else it
            }
            _uiState.value = _uiState.value.copy(events = updated)

            if (shouldNotify) {
                appContext?.let { ctx ->
                    val categoryName = matchedCategories.first()
                    NotificationHelper.sendDetectionNotification(
                        ctx, currentDeviceName, event.id, categoryName,
                        deviceId = currentDeviceId, ownerUid = dataOwnerUid
                    )
                }
            }
        } catch (_: Exception) {}
    }

    fun selectEvent(event: SecurityEvent) {
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(
                selectedEvent = event, isLoading = true,
                decodedBitmaps = emptyMap(), detectionResults = emptyMap()
            )
            try {
                val frames = eventRepo.getEventFrames(currentDeviceId, event.id)
                val bitmaps = mutableMapOf<Int, Bitmap>()
                for (frame in frames) {
                    val bitmap = eventRepo.decodeFrame(frame.image_base64)
                    if (bitmap != null) bitmaps[frame.frame_index] = bitmap
                }
                _uiState.value = _uiState.value.copy(
                    selectedFrames = frames, decodedBitmaps = bitmaps, isLoading = false
                )
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(isLoading = false, error = e.message)
            }
        }
    }

    /**
     * Ritorna l'uid che identifica l'owner reale dei dati Firestore.
     * - Se dataOwnerUid e' stato settato in init() lo uso
     * - Altrimenti faccio fallback al uid corrente (caso "device mio")
     * - Se anche quello e' vuoto ritorno "" (situazione anomala)
     *
     * Questa funzione e' difensiva: previene path malformati come
     * "users//devices/..." (doppio slash) che Firestore rifiuta
     * silenziosamente, facendo fallire tutte le operazioni di delete.
     */
    private fun effectiveOwnerUid(): String {
        if (dataOwnerUid.isNotEmpty()) return dataOwnerUid
        return com.google.firebase.auth.FirebaseAuth.getInstance().currentUser?.uid ?: ""
    }

    fun deleteEvent(event: SecurityEvent) {
        if (isShared) return
        val ownerUid = effectiveOwnerUid()
        android.util.Log.d("EVT_DEL",
            "deleteEvent id=${event.id} ownerUid='$ownerUid' deviceId='$currentDeviceId'")
        if (ownerUid.isEmpty() || currentDeviceId.isEmpty()) {
            android.util.Log.e("EVT_DEL", "ownerUid o deviceId vuoto, abort")
            return
        }

        _uiState.value = _uiState.value.copy(
            events = _uiState.value.events.filter { it.id != event.id }
        )
        viewModelScope.launch {
            try {
                val db = com.google.firebase.firestore.FirebaseFirestore.getInstance()
                val basePath = "users/$ownerUid/devices/$currentDeviceId"
                android.util.Log.d("EVT_DEL", "basePath=$basePath")
                val frames = eventRepo.getEventFrames(currentDeviceId, event.id)
                android.util.Log.d("EVT_DEL", "frames trovati: ${frames.size}")

                // Uso WriteBatch (vedi commento in deleteSelectedEvents)
                val batch = db.batch()
                for (frame in frames) {
                    batch.delete(db.document(
                        "$basePath/events/${event.id}/frames/${frame.id}"))
                }
                batch.delete(db.document("$basePath/events/${event.id}"))

                android.util.Log.d("EVT_DEL", "Batch commit (1 evento + ${frames.size} frame)")
                val ok = kotlinx.coroutines.withTimeoutOrNull(20_000) {
                    batch.commit().await()
                    true
                }
                if (ok == null) {
                    android.util.Log.e("EVT_DEL", "Batch TIMEOUT 20s")
                } else {
                    android.util.Log.d("EVT_DEL", "delete OK per ${event.id}")
                }
            } catch (e: Exception) {
                android.util.Log.e("EVT_DEL", "delete FAILED: ${e.message}", e)
            }
        }
    }

    fun deleteAllEvents() {
        if (isShared) return
        val ownerUid = effectiveOwnerUid()
        android.util.Log.d("EVT_DEL",
            "deleteAllEvents ownerUid='$ownerUid' deviceId='$currentDeviceId'")
        if (ownerUid.isEmpty() || currentDeviceId.isEmpty()) {
            android.util.Log.e("EVT_DEL", "ownerUid o deviceId vuoto, abort")
            return
        }

        val eventsToDelete = _uiState.value.events.toList()
        _uiState.value = _uiState.value.copy(events = emptyList())
        eventRepo.stopListening()

        viewModelScope.launch {
            try {
                val db = com.google.firebase.firestore.FirebaseFirestore.getInstance()
                val basePath = "users/$ownerUid/devices/$currentDeviceId"
                android.util.Log.d("EVT_DEL",
                    "deleteAll basePath=$basePath, ${eventsToDelete.size} eventi")

                // Raccolgo TUTTI i ref da cancellare, poi splitto in batch
                // da max 400 (margine sotto il limite Firestore di 500).
                // Pattern identico a removeDeviceWithData del FirestoreRepo.
                val allRefs = mutableListOf<com.google.firebase.firestore.DocumentReference>()
                for (event in eventsToDelete) {
                    val frames = eventRepo.getEventFrames(currentDeviceId, event.id)
                    for (frame in frames) {
                        allRefs.add(db.document(
                            "$basePath/events/${event.id}/frames/${frame.id}"))
                    }
                    allRefs.add(db.document("$basePath/events/${event.id}"))
                }
                android.util.Log.d("EVT_DEL",
                    "Totale ${allRefs.size} ref, splitto in batch da 400")

                val chunks = allRefs.chunked(400)
                for ((idx, chunk) in chunks.withIndex()) {
                    val batch = db.batch()
                    for (ref in chunk) batch.delete(ref)
                    val ok = kotlinx.coroutines.withTimeoutOrNull(20_000) {
                        batch.commit().await()
                        true
                    }
                    if (ok == null) {
                        android.util.Log.e("EVT_DEL",
                            "Batch ${idx + 1}/${chunks.size} TIMEOUT 20s")
                    } else {
                        android.util.Log.d("EVT_DEL",
                            "Batch ${idx + 1}/${chunks.size} OK (${chunk.size} ops)")
                    }
                }
                android.util.Log.d("EVT_DEL", "deleteAll COMPLETATO")
            } catch (e: Exception) {
                android.util.Log.e("EVT_DEL", "deleteAll FAILED: ${e.message}", e)
            }
            startRealTimeListener()
        }
    }

    fun deleteSelectedEvents(eventIds: Set<String>) {
        if (isShared) return
        val ownerUid = effectiveOwnerUid()
        android.util.Log.d("EVT_DEL",
            "deleteSelected ${eventIds.size} eventi ownerUid='$ownerUid'")
        if (ownerUid.isEmpty() || currentDeviceId.isEmpty()) {
            android.util.Log.e("EVT_DEL", "ownerUid o deviceId vuoto, abort")
            return
        }

        val eventsToDelete = _uiState.value.events.filter { it.id in eventIds }
        android.util.Log.d("EVT_DEL", "eventsToDelete.size=${eventsToDelete.size}")
        _uiState.value = _uiState.value.copy(
            events = _uiState.value.events.filter { it.id !in eventIds }
        )
        eventRepo.stopListening()
        android.util.Log.d("EVT_DEL", "stopListening fatto, lancio coroutine")

        viewModelScope.launch {
            android.util.Log.d("EVT_DEL", "DENTRO viewModelScope.launch")
            try {
                val db = com.google.firebase.firestore.FirebaseFirestore.getInstance()
                val basePath = "users/$ownerUid/devices/$currentDeviceId"
                android.util.Log.d("EVT_DEL", "deleteSelected basePath=$basePath")

                // STRATEGIA: uso un WriteBatch invece di N delete singole.
                // Le delete singole con .await() sull'emulatore Android e/o
                // su connessioni instabili (hotspot, NAT multi-hop) restano
                // appese indefinitamente perche' il Firestore SDK le mette
                // in pending writes e non risolve mai la await(). Il
                // WriteBatch invece e' atomico: una sola chiamata HTTP, una
                // sola await(), gestione retry interna piu' robusta.
                //
                // Limite Firestore: max 500 operazioni per batch. Per
                // deleteSelectedEvents tipicamente sono <= 50 frame (max
                // 5 frame per evento * 10 eventi selezionati), ben dentro
                // il limite.
                val batch = db.batch()
                var totalOps = 0
                for ((idx, event) in eventsToDelete.withIndex()) {
                    android.util.Log.d("EVT_DEL",
                        "Evento ${idx + 1}/${eventsToDelete.size}: ${event.id}")
                    val frames = eventRepo.getEventFrames(currentDeviceId, event.id)
                    android.util.Log.d("EVT_DEL", "  frames recuperati: ${frames.size}")
                    for (frame in frames) {
                        batch.delete(db.document(
                            "$basePath/events/${event.id}/frames/${frame.id}"))
                        totalOps++
                    }
                    batch.delete(db.document("$basePath/events/${event.id}"))
                    totalOps++
                }
                android.util.Log.d("EVT_DEL",
                    "Batch pronto con $totalOps operazioni, commit...")

                val ok = kotlinx.coroutines.withTimeoutOrNull(20_000) {
                    batch.commit().await()
                    true
                }
                if (ok == null) {
                    android.util.Log.e("EVT_DEL",
                        "Batch commit TIMEOUT dopo 20s (la cache Firestore " +
                                "potrebbe essere sincronizzata in background piu' tardi)")
                } else {
                    android.util.Log.d("EVT_DEL", "Batch commit OK")
                }
            } catch (e: Exception) {
                android.util.Log.e("EVT_DEL", "deleteSelected FAILED: ${e.message}", e)
            }
            startRealTimeListener()
        }
    }

    fun clearSelection() {
        _uiState.value = _uiState.value.copy(
            selectedEvent = null, selectedFrames = emptyList(),
            decodedBitmaps = emptyMap(), detectionResults = emptyMap()
        )
    }

    override fun onCleared() {
        eventRepo.stopListening()
    }
}