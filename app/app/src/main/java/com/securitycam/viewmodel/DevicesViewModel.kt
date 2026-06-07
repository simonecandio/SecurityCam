package com.securitycam.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.google.firebase.auth.FirebaseAuth
import com.securitycam.model.Device
import com.securitycam.repository.DeviceRepository
import com.securitycam.repository.FirestoreDeviceRepository
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch


data class DevicesUiState(
    val devices: List<Device> = emptyList(),
    val isLoading: Boolean = false,
    val error: String? = null,
    // aggiunta dispositivo
    val isAdding: Boolean = false,
    val addIp: String = "",
    val addName: String = "",
    val addMessage: String? = null,
    val showAddDialog: Boolean = false,
    val isVerifying: Boolean = false,
    val deviceVerified: Boolean = false,
    // dialog conferma rimozione
    val showRemoveDialog: Boolean = false,
    val removeDeviceId: String? = null,
    val removeDeviceName: String? = null,
    val removeDeviceIsShared: Boolean = false,
    val isRemoving: Boolean = false,
    // condivisione (invito)
    val showShareDialog: Boolean = false,
    val shareCode: String? = null,
    val shareDeviceName: String? = null,
    val isGeneratingCode: Boolean = false,
    // accettare un invito
    val showJoinDialog: Boolean = false,
    val joinCode: String = "",
    val joinMessage: String? = null,
    val isJoining: Boolean = false
)

class DevicesViewModel : ViewModel() {

    private val firestoreRepo = FirestoreDeviceRepository()
    private val deviceRepo = DeviceRepository()

    private val _uiState = MutableStateFlow(DevicesUiState())
    val uiState: StateFlow<DevicesUiState> = _uiState.asStateFlow()

    init { loadDevices() }

    fun loadDevices() {
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isLoading = true, error = null)
            try {
                val devices = firestoreRepo.getDevices()
                _uiState.value = _uiState.value.copy(devices = devices, isLoading = false)
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(isLoading = false, error = e.message)
            }
        }
    }

    // === AGGIUNTA DISPOSITIVO ===

    fun showAddDialog() {
        _uiState.value = _uiState.value.copy(
            showAddDialog = true, addIp = "", addName = "",
            addMessage = null, deviceVerified = false
        )
    }

    fun hideAddDialog() {
        _uiState.value = _uiState.value.copy(showAddDialog = false)
    }

    fun updateAddIp(ip: String) {
        _uiState.value = _uiState.value.copy(addIp = ip, deviceVerified = false)
    }

    fun updateAddName(name: String) {
        _uiState.value = _uiState.value.copy(addName = name)
    }

    fun verifyDevice() {
        val ip = _uiState.value.addIp.trim()
        if (ip.isEmpty()) {
            _uiState.value = _uiState.value.copy(addMessage = "Inserisci l'IP")
            return
        }
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isVerifying = true, addMessage = null)
            deviceRepo.updateDeviceIp(ip)
            val reachable = deviceRepo.isDeviceReachable()
            _uiState.value = _uiState.value.copy(
                isVerifying = false,
                deviceVerified = reachable,
                addMessage = if (reachable) "Dispositivo trovato!" else "Dispositivo non raggiungibile"
            )
        }
    }

    fun addDevice(categories: List<String> = listOf("person")) {
        android.util.Log.e("DEVICES", "===== addDevice CHIAMATO =====")
        val ip = _uiState.value.addIp.trim()
        android.util.Log.d("DEVICES", "ip=$ip")
        val ipRegex = Regex("^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$")
        if (!ipRegex.matches(ip)) {
            android.util.Log.e("DEVICES", "IP non valido, esco")
            _uiState.value = _uiState.value.copy(addMessage = "IP non valido")
            return
        }
        // CONTROLLO DUPLICATO IP
        val existing = _uiState.value.devices.find { it.ip == ip && !it.isShared }
        if (existing != null) {
            _uiState.value = _uiState.value.copy(
                addMessage = "Dispositivo già presente: ${existing.name}"
            )
            return
        }
        val name = _uiState.value.addName.trim().ifEmpty { "Camera" }
        val uid = FirebaseAuth.getInstance().currentUser?.uid
        android.util.Log.d("DEVICES", "uid=$uid name=$name")
        if (uid == null) {
            android.util.Log.e("DEVICES", "UID null, esco")
            return
        }
        val deviceId = "cam_${System.currentTimeMillis() / 1000}"
        android.util.Log.d("DEVICES", "deviceId=$deviceId")

        _uiState.value = _uiState.value.copy(isAdding = false, showAddDialog = false)
        val newDevice = Device(id = deviceId, name = name, ip = ip,
            addedAt = System.currentTimeMillis(),
            monitorCategories = categories)
        _uiState.value = _uiState.value.copy(
            devices = listOf(newDevice) + _uiState.value.devices
        )

        // ORDINE IMPORTANTE:
        // 1. PRIMA scrivo su Firestore e ASPETTO che completi
        // 2. SOLO POI mando setOwnerConfig + reboot all'ESP
        //
        // Motivo: l'ESP, al primo boot dopo setOwnerConfig, attiva
        // l'ownership check periodico che fa GET su
        // users/{uid}/devices/{id}. Se il documento Firestore non e'
        // ancora stato creato perche' la scrittura era partita in
        // parallelo (e magari l'app e' stata killata prima di
        // completarla), l'ESP riceve 404 e dopo 3 conferme
        // si auto-deautorizza.
        //
        // Uso GlobalScope per essere sicuro che la sequenza completi
        // anche se l'utente naviga via subito dopo aver premuto "aggiungi"
        // (ho visto nei log che l'app puo' essere killata in mezzo).
        @OptIn(kotlinx.coroutines.DelicateCoroutinesApi::class)
        kotlinx.coroutines.GlobalScope.launch {
            // === STEP 1: scrivo PRIMA su Firestore ===
            android.util.Log.d("DEVICES", "Scrivo su Firestore (sequenziale)")
            var firestoreOk = false
            try {
                firestoreOk = firestoreRepo.addDevice(deviceId, name, ip, categories)
                android.util.Log.d("DEVICES", "Firestore esito: $firestoreOk")
            } catch (e: Exception) {
                android.util.Log.e("DEVICES", "Firestore errore: ${e.message}", e)
            }

            if (!firestoreOk) {
                android.util.Log.e("DEVICES",
                    "Firestore fallito, NON mando config all'ESP per evitare " +
                            "che si auto-deautorizzi al primo boot")
                // rimuovo dalla UI quello che avevo aggiunto ottimisticamente
                try {
                    _uiState.value = _uiState.value.copy(
                        devices = _uiState.value.devices.filter { it.id != deviceId },
                        addMessage = "Errore di rete, riprova"
                    )
                } catch (_: Exception) {}
                return@launch
            }

            // === STEP 2: ora che Firestore ha il documento, mando config all'ESP ===
            // L'ESP al riavvio fara' subito il login Firebase e dopo 30s
            // il primo ownership check: trovera' il documento appena scritto
            // e non si deautorizzera'.
            android.util.Log.d("DEVICES", "===== Invio config a ESP =====")
            try {
                deviceRepo.updateDeviceIp(ip)
                android.util.Log.d("DEVICES", "IP aggiornato, chiamo setOwnerConfig...")
                val result = deviceRepo.setOwnerConfig(uid, deviceId)
                android.util.Log.d("DEVICES", "setOwnerConfig ritornato")
                result.onSuccess {
                    android.util.Log.d("DEVICES", "Config OK, riavvio ESP")
                    deviceRepo.reboot()
                }
                result.onFailure {
                    android.util.Log.e("DEVICES",
                        "setOwnerConfig FALLITA: ${it.message}", it)
                }
            } catch (e: Exception) {
                android.util.Log.e("DEVICES", "Eccezione: ${e.message}", e)
            }
        }
    }

    // === RIMOZIONE DISPOSITIVO ===

    fun requestRemoveDevice(deviceId: String) {
        val device = _uiState.value.devices.find { it.id == deviceId }
        _uiState.value = _uiState.value.copy(
            showRemoveDialog = true,
            removeDeviceId = deviceId,
            removeDeviceName = device?.name ?: deviceId,
            removeDeviceIsShared = device?.isShared ?: false
        )
    }

    fun cancelRemoveDevice() {
        _uiState.value = _uiState.value.copy(
            showRemoveDialog = false,
            removeDeviceId = null,
            removeDeviceName = null
        )
    }

    /** Rimuove solo dall'account, l'ESP resta operativo */
    fun removeFromAppOnly(context: android.content.Context? = null) {
        val deviceId = _uiState.value.removeDeviceId ?: return
        _uiState.value = _uiState.value.copy(
            showRemoveDialog = false, removeDeviceId = null, removeDeviceName = null,
            devices = _uiState.value.devices.filter { it.id != deviceId }
        )
        context?.let {
            val prefs = com.securitycam.util.PreferencesManager(it)
            if (prefs.getLastDeviceId() == deviceId) prefs.clearLastDevice()
        }
        viewModelScope.launch {
            firestoreRepo.removeDeviceWithData(deviceId)
            loadDevices()
        }
    }

    /**
     * Rimuove dall'account E fa factory reset dell'ESP.
     *
     * Tenta DUE strade in cascata per il factory reset:
     *
     * 1. HTTP DIRETTO (veloce, funziona solo se sulla stessa rete dell'ESP):
     *    POST https://{ip}/factory_reset -> l'ESP cancella NVS e
     *    riavvia in SoftAP. Tempo: ~5 secondi.
     *
     * 2. FIRESTORE FALLBACK (funziona da qualsiasi rete, anche dall'altra
     *    parte del mondo, purche' l'ESP sia online):
     *    Scrive {factory_reset: true} su pending_config/current.
     *    L'ESP la legge nel suo prossimo check (ogni 30s) e fa il reset.
     *    Tempo: fino a ~60 secondi nel caso peggiore.
     *
     * Uso GlobalScope invece di viewModelScope perche' il
     * delay(60_000) del fallback Firestore e' troppo lungo. Se l'utente
     * naviga via dalla schermata Devices durante l'attesa, il ViewModel
     * viene distrutto, viewModelScope cancellato, e la cancellazione
     * Firestore si interrompe a meta' lasciando il device visibile su
     * Firebase.
     *
     *
     */
    @OptIn(kotlinx.coroutines.DelicateCoroutinesApi::class)
    fun removeAndFactoryReset(context: android.content.Context? = null) {
        val deviceId = _uiState.value.removeDeviceId ?: return
        val device = _uiState.value.devices.find { it.id == deviceId }
        _uiState.value = _uiState.value.copy(
            showRemoveDialog = false, removeDeviceId = null, removeDeviceName = null,
            devices = _uiState.value.devices.filter { it.id != deviceId },
            isRemoving = true
        )
        context?.let {
            val prefs = com.securitycam.util.PreferencesManager(it)
            if (prefs.getLastDeviceId() == deviceId) prefs.clearLastDevice()
        }

        // Catturo i riferimenti che mi servono PRIMA di lanciare la coroutine
        // su GlobalScope: dentro non posso accedere a "this" del ViewModel
        // in modo sicuro perche' potrebbe essere distrutto.
        val deviceRepoRef = deviceRepo
        val firestoreRepoRef = firestoreRepo

        kotlinx.coroutines.GlobalScope.launch {
            // === STEP 1: provo PRIMA la via HTTP diretta ===
            // Se sono sulla stessa rete dell'ESP funziona ed e' veloce.
            var httpSuccess = false
            if (device != null && device.ip.isNotEmpty()) {
                try {
                    android.util.Log.d("REMOVE", "Tentativo HTTP a ${device.ip}")
                    deviceRepoRef.updateDeviceIp(device.ip)
                    val result = deviceRepoRef.factoryReset()
                    result.onSuccess {
                        httpSuccess = true
                        android.util.Log.d("REMOVE", "Factory reset HTTP accettato")
                    }
                    result.onFailure { e ->
                        android.util.Log.w("REMOVE", "HTTP fallito: ${e.message}")
                    }
                } catch (e: Exception) {
                    android.util.Log.w("REMOVE", "HTTP eccezione: ${e.message}")
                }
            }

            // === STEP 2: se HTTP fallito, fallback su Firestore ===
            // Scrivo {factory_reset:true} su pending_config/current.
            // L'ESP lo legge nel prossimo check_remote_config (max ~30s).
            if (!httpSuccess) {
                android.util.Log.d("REMOVE", "Fallback Firestore pending_config")
                val written = firestoreRepoRef.requestRemoteFactoryReset(deviceId)
                if (written) {
                    android.util.Log.d("REMOVE",
                        "Comando factory reset scritto su Firestore")
                } else {
                    android.util.Log.e("REMOVE",
                        "Errore scrittura factory reset cloud")
                }
            }

            // === STEP 3: aspetto che l'ESP completi reset + reboot ===
            // - Via HTTP: ~5s reset + ~3s reboot = 8s di margine
            // - Via Firestore: fino a 30s di check + 5s reset + 3s reboot
            //   + qualche secondo di slack = 60s di margine
            val waitMs = if (httpSuccess) 8_000L else 60_000L
            android.util.Log.d("REMOVE",
                "Attendo ${waitMs / 1000}s per completamento reset")
            delay(waitMs)

            // === STEP 4: cancello da Firestore (l'ESP non scrivera' piu') ===
            android.util.Log.d("REMOVE", "Cancellazione Firestore in corso")
            val ok = firestoreRepoRef.removeDeviceWithData(deviceId)
            android.util.Log.d("REMOVE", "Cancellazione Firestore esito: $ok")

            // === STEP 5: doppio passaggio di pulizia per residui ===
            delay(2000)
            firestoreRepoRef.removeDeviceWithData(deviceId)

            android.util.Log.d("REMOVE", "Operazione completata")

            try {
                _uiState.value = _uiState.value.copy(isRemoving = false)
                loadDevices()
            } catch (_: Exception) {
                // VM distrutto, ignoro
            }
        }
    }

    // === CONDIVISIONE (INVITI) ===

    /** Genera un codice invito a 6 cifre per condividere il device */
    fun shareDevice(deviceId: String) {
        val device = _uiState.value.devices.find { it.id == deviceId } ?: return
        _uiState.value = _uiState.value.copy(
            showShareDialog = true,
            shareCode = null,
            shareDeviceName = device.name,
            isGeneratingCode = true
        )
        viewModelScope.launch {
            val code = firestoreRepo.createInvite(device)
            _uiState.value = _uiState.value.copy(
                shareCode = code,
                isGeneratingCode = false
            )
        }
    }

    fun hideShareDialog() {
        _uiState.value = _uiState.value.copy(
            showShareDialog = false,
            shareCode = null,
            shareDeviceName = null
        )
    }

    /** Mostra il dialog per inserire un codice invito */
    fun showJoinDialog() {
        _uiState.value = _uiState.value.copy(
            showJoinDialog = true,
            joinCode = "",
            joinMessage = null
        )
    }

    fun hideJoinDialog() {
        _uiState.value = _uiState.value.copy(
            showJoinDialog = false,
            joinCode = "",
            joinMessage = null
        )
    }

    fun updateJoinCode(code: String) {
        // solo cifre, max 6 caratteri
        val filtered = code.filter { it.isDigit() }.take(6)
        _uiState.value = _uiState.value.copy(joinCode = filtered)
    }

    /** Accetta un invito con il codice a 6 cifre */
    fun acceptInvite() {
        val code = _uiState.value.joinCode
        if (code.length != 6) {
            _uiState.value = _uiState.value.copy(joinMessage = "Inserisci un codice a 6 cifre")
            return
        }
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isJoining = true, joinMessage = null)
            val result = firestoreRepo.acceptInvite(code)
            result.onSuccess { deviceName ->
                _uiState.value = _uiState.value.copy(
                    isJoining = false,
                    showJoinDialog = false,
                    joinMessage = null
                )
                loadDevices()
            }
            result.onFailure { error ->
                _uiState.value = _uiState.value.copy(
                    isJoining = false,
                    joinMessage = error.message
                )
            }
        }
    }
}