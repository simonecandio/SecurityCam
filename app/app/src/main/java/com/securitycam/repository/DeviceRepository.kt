package com.securitycam.repository

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import com.securitycam.model.*
import com.securitycam.network.Esp32Client
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/*
* DeviceRepository.kt
* Repository per la comunicazione diretta con l'ESP32 in rete locale.
*/
class DeviceRepository {

    private var apiService = Esp32Client.create("192.168.1.1")
    private var currentIp = ""


    fun updateDeviceIp(ip: String) {
        if (ip != currentIp && ip.isNotEmpty()) {
            currentIp = ip
            apiService = Esp32Client.create(ip)
        }
    }

    suspend fun isDeviceReachable(): Boolean = withContext(Dispatchers.IO) {
        try {
            val response = apiService.getStatus()
            response.isSuccessful
        } catch (e: Exception) {
            false
        }
    }

    suspend fun getStatus(): Result<DeviceStatus> = withContext(Dispatchers.IO) {
        try {
            val response = apiService.getStatus()
            if (response.isSuccessful && response.body() != null) {
                Result.success(response.body()!!)
            } else {
                Result.failure(Exception("HTTP ${response.code()}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun setConfig(config: DeviceConfig): Result<ConfigResponse> =
        withContext(Dispatchers.IO) {
            try {
                val response = apiService.setConfig(config)
                if (response.isSuccessful && response.body() != null) {
                    Result.success(response.body()!!)
                } else {
                    Result.failure(Exception("HTTP ${response.code()}"))
                }
            } catch (e: Exception) {
                Result.failure(e)
            }
        }

    /**
     * Manda owner_uid e device_id all'ESP32 per associarlo all'utente.
     * L'ESP salverà in NVS e al riavvio scriverà nel path Firestore corretto.
     */
    suspend fun setOwnerConfig(ownerUid: String, deviceId: String): Result<ConfigResponse> =
        withContext(Dispatchers.IO) {
            try {
                val config = OwnerConfig(owner_uid = ownerUid, device_id = deviceId)
                val response = apiService.setOwnerConfig(config)
                if (response.isSuccessful && response.body() != null) {
                    Result.success(response.body()!!)
                } else {
                    Result.failure(Exception("HTTP ${response.code()}"))
                }
            } catch (e: Exception) {
                Result.failure(e)
            }
        }

    suspend fun reboot(): Result<ConfigResponse> = withContext(Dispatchers.IO) {
        try {
            val response = apiService.reboot()
            if (response.isSuccessful && response.body() != null) {
                Result.success(response.body()!!)
            } else {
                Result.failure(Exception("HTTP ${response.code()}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun captureFrame(): Result<Bitmap> = withContext(Dispatchers.IO) {
        try {
            val response = apiService.captureFrame()
            if (response.isSuccessful && response.body() != null) {
                val bytes = response.body()!!.bytes()
                val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                if (bitmap != null) Result.success(bitmap)
                else Result.failure(Exception("Decodifica fallita"))
            } else {
                Result.failure(Exception("HTTP ${response.code()}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun factoryReset(): Result<ConfigResponse> = withContext(Dispatchers.IO) {
        try {
            val response = apiService.factoryReset()
            if (response.isSuccessful && response.body() != null) {
                Result.success(response.body()!!)
            } else {
                Result.failure(Exception("HTTP ${response.code()}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun setWifiConfig(ssid: String, password: String): Result<ConfigResponse> =
        withContext(Dispatchers.IO) {
            try {
                val config = mapOf("ssid" to ssid, "wifi_pass" to password)
                val response = apiService.setWifiConfig(config)
                if (response.isSuccessful && response.body() != null) {
                    Result.success(response.body()!!)
                } else {
                    Result.failure(Exception("HTTP ${response.code()}"))
                }
            } catch (e: Exception) {
                Result.failure(e)
            }
        }

}
