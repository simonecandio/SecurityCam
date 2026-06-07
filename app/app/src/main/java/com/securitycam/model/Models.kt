package com.securitycam.model

/*
*Models.kt
* Data class e oggetti che rappresentano il dominio dell'applicazione.
* È il dizionario dell'app — definisce la forma di tutti i dati.
* Quando l'app riceve un JSON da Firebase o dall'ESP, lo converte in una di queste data class.
*/
data class Device(
    val id: String = "",
    val name: String = "",
    val ip: String = "",
    val addedAt: Long = 0,
    val lastSeen: Long = 0,
    val monitorCategories: List<String> = listOf("person"),
    // se non vuoto, indica che il device e' condiviso da un altro utente.
    // eventi e status vanno letti da users/{ownerUid}/devices/{id}/
    // invece che da users/{mioUid}/devices/{id}/
    val ownerUid: String = ""
) {
    /** true se il device e' di proprieta' di un altro utente (condiviso con me) */
    val isShared: Boolean get() = ownerUid.isNotEmpty()
}

data class DeviceStatus(
    val device: String = "",
    val firmware: String = "",
    val uptime_s: Long = 0,
    val free_heap: Long = 0,
    val camera_ok: Boolean = false,
    val pir_ok: Boolean = false,
    val sd_card_ok: Boolean = false,
    val wifi_ok: Boolean = false,
    val firebase_ok: Boolean = false,
    val total_events: Int = 0,
    val failed_uploads: Int = 0,
    val state: String = "unknown",
    val ip: String = "",
    val time: String = ""
)

data class SecurityEvent(
    val id: String = "",
    val event_id: Long = 0,
    val timestamp: Long = 0,
    val frame_count: Int = 0,
    val pir_value: Int = 0,
    val validated: Boolean = false,
    val frames: List<EventFrame> = emptyList()
)

data class EventFrame(
    val id: String = "",
    val image_base64: String = "",
    val jpeg_size: Int = 0,
    val frame_index: Int = 0
)

data class DeviceConfig(
    val cooldown_ms: Int = 10000,
    val frame_count: Int = 3,
    val jpeg_quality: Int = 12
)

data class ConfigResponse(
    val ok: Boolean = false,
    val msg: String = "",
    val error: String = ""
)

data class OwnerConfig(
    val owner_uid: String,
    val device_id: String
)

data class DetectionResult(
    val frameIndex: Int = 0,
    val personDetected: Boolean = false,
    val confidence: Float = 0f,
    val detectedCategories: List<String> = emptyList(),
    val detectedLabel: String = ""
)

object DetectionCategories {
    const val PERSON = "person"
    const val DOG = "dog"
    const val CAT = "cat"

    val ALL = listOf(PERSON, DOG, CAT)

    val LABEL_MAP = mapOf(
        "Person" to PERSON,
        "Human body" to PERSON,
        "Man" to PERSON,
        "Woman" to PERSON,
        "Dog" to DOG,
        "Cat" to CAT,
        "Small to medium-sized cats" to CAT,
    )
    fun labelToCategory(mlKitLabel: String): String? {
        return LABEL_MAP[mlKitLabel] ?: LABEL_MAP.entries.firstOrNull {
            mlKitLabel.contains(it.key, ignoreCase = true)
        }?.value
    }
}