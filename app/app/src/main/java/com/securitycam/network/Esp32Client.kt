package com.securitycam.network

import com.securitycam.model.ConfigResponse
import com.securitycam.model.DeviceConfig
import com.securitycam.model.DeviceStatus
import com.securitycam.model.OwnerConfig
import okhttp3.Credentials
import okhttp3.Interceptor
import okhttp3.OkHttpClient
import okhttp3.ResponseBody
import retrofit2.Response
import retrofit2.Retrofit
import retrofit2.converter.gson.GsonConverterFactory
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager
/* Esp32ApiService.kt
* Client di rete per la comunicazione diretta con l'ESP32 via LAN.
*
* Qui si implementa il canale di comunicazione locale tra l'app
* e l'ESP32, usato quando entrambi sono sulla stessa rete WiFi.
* È uno dei tre canali di rete dell'app (gli altri due sono il
* Firestore SDK per il cloud e OkHttp raw per lo streaming MJPEG).
*/
interface Esp32ApiService {
    @GET("/status")
    suspend fun getStatus(): Response<DeviceStatus>

    @POST("/config")
    suspend fun setConfig(@Body config: DeviceConfig): Response<ConfigResponse>

    @POST("/config")
    suspend fun setOwnerConfig(@Body config: OwnerConfig): Response<ConfigResponse>

    @POST("/reboot")
    suspend fun reboot(): Response<ConfigResponse>

    @GET("/capture")
    suspend fun captureFrame(): Response<ResponseBody>

    @POST("/factory_reset")
    suspend fun factoryReset(): Response<ConfigResponse>

    @POST("/wifi_config")
    suspend fun setWifiConfig(@Body config: Map<String, String>): Response<ConfigResponse>
}

// Interceptor che aggiunge l'header Basic Auth a ogni richiesta
class BasicAuthInterceptor(
    private val username: String,
    private val password: String
) : Interceptor {
    override fun intercept(chain: Interceptor.Chain): okhttp3.Response {
        val request = chain.request().newBuilder()
            .header("Authorization", Credentials.basic(username, password))
            .build()
        return chain.proceed(request)
    }
}
// Singleton factory: costruisce i client di rete verso l'ESP32
object Esp32Client {
    private const val AUTH_USER = "YOUR_AUTH_USER"
    private const val AUTH_PASS = "YOUR_AUTH_PASS"

    /**
     * TrustManager che accetta tutti i certificati (inclusi self-signed).
     * Necessario perché l'ESP32 genera un certificato self-signed al primo boot.
     * In produzione si userebbe certificate pinning con il certificato specifico del dispositivo.
     */
    private val trustAllManager = object : X509TrustManager {
        override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
        override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
        override fun getAcceptedIssuers(): Array<X509Certificate> = arrayOf()
    }

    private fun getUnsafeSslContext(): SSLContext {
        val sslContext = SSLContext.getInstance("TLS")
        sslContext.init(null, arrayOf<TrustManager>(trustAllManager), SecureRandom())
        return sslContext
    }

    private fun getBaseClientBuilder(): OkHttpClient.Builder {
        return OkHttpClient.Builder()
            .addInterceptor(BasicAuthInterceptor(AUTH_USER, AUTH_PASS))
            .sslSocketFactory(getUnsafeSslContext().socketFactory, trustAllManager)
            .hostnameVerifier { _, _ -> true }
    }

    fun create(deviceIp: String): Esp32ApiService {
        val client = getBaseClientBuilder()
            .connectTimeout(10, TimeUnit.SECONDS)
            .readTimeout(10, TimeUnit.SECONDS)
            .writeTimeout(10, TimeUnit.SECONDS)
            .build()

        return Retrofit.Builder()
            .baseUrl("https://$deviceIp/")
            .client(client)
            .addConverterFactory(GsonConverterFactory.create())
            .build()
            .create(Esp32ApiService::class.java)
    }

    // Client OkHttp "raw" per lo streaming MJPEG:
    // readTimeout = 0 (infinito) perché lo stream resta aperto finché l'utente guarda
    fun createStreamClient(): OkHttpClient {
        return getBaseClientBuilder()
            .connectTimeout(5, TimeUnit.SECONDS)
            .readTimeout(0, TimeUnit.SECONDS)
            .build()
    }
}