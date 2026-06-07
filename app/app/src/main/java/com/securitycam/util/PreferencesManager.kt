package com.securitycam.util

import android.content.Context
import android.content.SharedPreferences

class PreferencesManager(context: Context) {

    private val prefs: SharedPreferences = context.getSharedPreferences(
        "security_cam_prefs", Context.MODE_PRIVATE
    )

    companion object {
        private const val KEY_LAST_DEVICE_ID = "last_device_id"
        private const val KEY_LAST_DEVICE_NAME = "last_device_name"
        private const val KEY_LAST_DEVICE_IP = "last_device_ip"
        private const val KEY_AUTO_ANALYZE = "auto_analyze"
    }

    fun saveLastDevice(id: String, name: String, ip: String, ownerUid: String = "") {
        prefs.edit()
            .putString("last_device_id", id)
            .putString("last_device_name", name)
            .putString("last_device_ip", ip)
            .putString("last_device_owner_uid", ownerUid)
            .apply()
    }

    fun getLastDeviceOwnerUid(): String = prefs.getString("last_device_owner_uid", "") ?: ""

    fun getLastDeviceId(): String = prefs.getString(KEY_LAST_DEVICE_ID, "") ?: ""
    fun getLastDeviceName(): String = prefs.getString(KEY_LAST_DEVICE_NAME, "") ?: ""
    fun getLastDeviceIp(): String = prefs.getString(KEY_LAST_DEVICE_IP, "") ?: ""

    fun setAutoAnalyze(enabled: Boolean) {
        prefs.edit().putBoolean(KEY_AUTO_ANALYZE, enabled).apply()
    }

    fun getAutoAnalyze(): Boolean = prefs.getBoolean(KEY_AUTO_ANALYZE, true)

    fun clearLastDevice() {
        prefs.edit()
            .remove(KEY_LAST_DEVICE_ID)
            .remove(KEY_LAST_DEVICE_NAME)
            .remove(KEY_LAST_DEVICE_IP)
            .apply()
    }
}