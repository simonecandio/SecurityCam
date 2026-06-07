package com.securitycam.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.securitycam.model.DeviceConfig
import com.securitycam.repository.DeviceRepository
import com.securitycam.repository.FirestoreDeviceRepository
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await

data class ConfigUiState(
    val cooldownMs: Int = 10000,
    val frameCount: Int = 3,
    val jpegQuality: Int = 12,
    val isSaving: Boolean = false,
    val isRebooting: Boolean = false,
    val message: String? = null,
    val isDeviceOnline: Boolean = false,
    val monitorCategories: Set<String> = setOf("person")
)

class ConfigViewModel : ViewModel() {

    private val deviceRepo = DeviceRepository()
    private val firestoreRepo = FirestoreDeviceRepository()

    private val _uiState = MutableStateFlow(ConfigUiState())
    val uiState: StateFlow<ConfigUiState> = _uiState.asStateFlow()

    private var deviceId = ""

    fun setDevice(ip: String, deviceId: String) {
        this.deviceId = deviceId
        deviceRepo.updateDeviceIp(ip)
        checkConnection()
        loadCategories()
    }

    fun checkConnection() {
        viewModelScope.launch {
            val reachable = deviceRepo.isDeviceReachable()
            _uiState.value = _uiState.value.copy(isDeviceOnline = reachable)
        }
    }

    private fun loadCategories() {
        viewModelScope.launch {
            try {
                val devices = firestoreRepo.getDevices()
                val device = devices.find { it.id == deviceId }
                if (device != null) {
                    _uiState.value = _uiState.value.copy(
                        monitorCategories = device.monitorCategories.toSet()
                    )
                }
            } catch (_: Exception) {}
        }
    }

    fun toggleCategory(category: String) {
        val current = _uiState.value.monitorCategories
        val updated = if (category in current) {
            if (current.size > 1) current - category else current // almeno una categoria
        } else {
            current + category
        }
        _uiState.value = _uiState.value.copy(monitorCategories = updated)

        // salva subito su Firestore
        viewModelScope.launch {
            firestoreRepo.updateMonitorCategories(deviceId, updated.toList())
        }
    }

    fun setCooldown(value: Int) { _uiState.value = _uiState.value.copy(cooldownMs = value.coerceIn(5000, 60000)) }
    fun setFrameCount(value: Int) { _uiState.value = _uiState.value.copy(frameCount = value.coerceIn(1, 5)) }
    fun setJpegQuality(value: Int) { _uiState.value = _uiState.value.copy(jpegQuality = value.coerceIn(5, 50)) }

    fun saveConfig() {
        viewModelScope.launch {
            //Provo a mandare la nuova config via http, se non riesco callback con firebase
            _uiState.value = _uiState.value.copy(isSaving = true, message = null)
            val config = DeviceConfig(_uiState.value.cooldownMs, _uiState.value.frameCount, _uiState.value.jpegQuality)

            val result = deviceRepo.setConfig(config)
            if (result.isSuccess) {
                _uiState.value = _uiState.value.copy(isSaving = false, message = "Salvato! Riavviare per applicare.")
                return@launch
            }
            //Uso firebase
            try {
                val uid = com.google.firebase.auth.FirebaseAuth.getInstance().currentUser?.uid ?: throw Exception("Non loggato")
                val data = hashMapOf(
                    "cooldown_ms" to config.cooldown_ms,
                    "frame_count" to config.frame_count,
                    "jpeg_quality" to config.jpeg_quality,
                    "timestamp" to System.currentTimeMillis()
                )
                com.google.firebase.firestore.FirebaseFirestore.getInstance()
                    .document("users/$uid/devices/$deviceId/pending_config/current")
                    .set(data)
                    .await()
                _uiState.value = _uiState.value.copy(isSaving = false, message = "Config salvata su cloud. Verrà applicata al prossimo check.")
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(isSaving = false, message = "Errore: ${e.message}")
            }
        }
    }

    fun rebootDevice() {
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isRebooting = true)
            deviceRepo.reboot()
            _uiState.value = _uiState.value.copy(isRebooting = false, message = "Riavvio in corso...", isDeviceOnline = false)
        }
    }

    fun clearMessage() { _uiState.value = _uiState.value.copy(message = null) }

    fun changeWifi(ssid: String, password: String) {
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(message = "Invio nuova configurazione WiFi...")
            val result = deviceRepo.setWifiConfig(ssid, password)
            if (result.isSuccess) {
                _uiState.value = _uiState.value.copy(message = "WiFi aggiornato! L'ESP si sta riavviando...", isDeviceOnline = false)
            } else {
                _uiState.value = _uiState.value.copy(message = "Errore: dispositivo non raggiungibile")
            }
        }
    }

    fun factoryReset() {
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(message = "Factory reset in corso...")
            val result = deviceRepo.factoryReset()
            if (result.isSuccess) {
                _uiState.value = _uiState.value.copy(message = "Reset completato! L'ESP torna in modalità configurazione.", isDeviceOnline = false)
            } else {
                _uiState.value = _uiState.value.copy(message = "Errore: dispositivo non raggiungibile")
            }
        }
    }
}