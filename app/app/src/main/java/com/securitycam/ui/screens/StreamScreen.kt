package com.securitycam.ui.screens

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import com.securitycam.R
import com.securitycam.network.Esp32Client
import com.securitycam.repository.MlKitRepository
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import okhttp3.Request
import java.io.ByteArrayOutputStream

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun StreamScreen(deviceIp: String, onBack: () -> Unit) {
    var currentFrame by remember { mutableStateOf<Bitmap?>(null) }
    var isConnected by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var retryTrigger by remember { mutableIntStateOf(0) }

    // Detection con MlKitRepository (unico punto di logica ML nell'app)
    var detectionEnabled by remember { mutableStateOf(false) }
    var detections by remember { mutableStateOf<List<MlKitRepository.BoxDetection>>(emptyList()) }
    var viewSize by remember { mutableStateOf(IntSize.Zero) }

    val mlKitRepo = remember { MlKitRepository() }

    // Detection throttled a 1 volta al secondo
    var lastDetectionTime by remember { mutableLongStateOf(0L) }

    LaunchedEffect(currentFrame, detectionEnabled) {
        if (!detectionEnabled) {
            detections = emptyList()
            return@LaunchedEffect
        }
        currentFrame?.let { bitmap ->
            val now = System.currentTimeMillis()
            if (now - lastDetectionTime < 1000) return@LaunchedEffect
            lastDetectionTime = now

            withContext(Dispatchers.Default) {
                detections = mlKitRepo.detectWithBoxes(bitmap)
            }
        }
    }

    // Timestamp dell'ultimo frame ricevuto (per watchdog)
    var lastFrameTime by remember { mutableLongStateOf(0L) }

    // Watchdog: se non arrivano frame per 8 secondi, segnala offline
    LaunchedEffect(isConnected) {
        if (!isConnected) return@LaunchedEffect
        while (isActive) {
            kotlinx.coroutines.delay(3000)
            if (lastFrameTime > 0 && System.currentTimeMillis() - lastFrameTime > 8000) {
                isConnected = false
                error = "Connessione persa (nessun frame da ${(System.currentTimeMillis() - lastFrameTime) / 1000}s)"
                break
            }
        }
    }

    // Stream MJPEG
    LaunchedEffect(deviceIp, retryTrigger) {
        error = null
        isConnected = false
        lastFrameTime = 0L
        withContext(Dispatchers.IO) {
            try {
                // Timeout di 10s sulla lettura: se l'ESP va offline,
                // la read() ritorna errore dopo 10s invece di bloccarsi
                val client = Esp32Client.createStreamClient().newBuilder()
                    .readTimeout(10, java.util.concurrent.TimeUnit.SECONDS)
                    .build()
                val request = Request.Builder()
                    .url("https://$deviceIp/stream")
                    .build()
                val response = client.newCall(request).execute()

                if (!response.isSuccessful) {
                    error = "HTTP ${response.code}"
                    return@withContext
                }

                isConnected = true
                lastFrameTime = System.currentTimeMillis()
                val inputStream = response.body?.byteStream() ?: return@withContext
                val buffer = ByteArrayOutputStream()
                var inFrame = false
                val readBuf = ByteArray(4096)

                while (isActive) {
                    val bytesRead = inputStream.read(readBuf)
                    if (bytesRead <= 0) break

                    for (i in 0 until bytesRead) {
                        val b = readBuf[i]
                        if (!inFrame) {
                            if (buffer.size() > 0 && buffer.toByteArray().last() == 0xFF.toByte() && b == 0xD8.toByte()) {
                                buffer.reset()
                                buffer.write(0xFF)
                                buffer.write(0xD8)
                                inFrame = true
                            } else {
                                buffer.write(b.toInt())
                                if (buffer.size() > 100) buffer.reset()
                            }
                        } else {
                            buffer.write(b.toInt())
                            if (buffer.size() > 2) {
                                val bytes = buffer.toByteArray()
                                if (bytes[bytes.size - 2] == 0xFF.toByte() && bytes[bytes.size - 1] == 0xD9.toByte()) {
                                    val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                                    if (bitmap != null) {
                                        currentFrame = bitmap
                                        lastFrameTime = System.currentTimeMillis()
                                    }
                                    buffer.reset()
                                    inFrame = false
                                }
                            }
                            if (buffer.size() > 200000) {
                                buffer.reset()
                                inFrame = false
                            }
                        }
                    }
                }
                response.close()
            } catch (e: Exception) {
                error = e.message ?: "Connessione persa"
                isConnected = false
            }
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.stream_title)) },
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(Icons.Default.ArrowBack, stringResource(R.string.back)) }
                },
                actions = {
                    IconButton(onClick = {
                        detectionEnabled = !detectionEnabled
                        if (!detectionEnabled) detections = emptyList()
                    }) {
                        Icon(
                            if (detectionEnabled) Icons.Default.Visibility else Icons.Default.VisibilityOff,
                            if (detectionEnabled) "Nascondi" else "Detection"
                        )
                    }
                    IconButton(onClick = { retryTrigger++ }) {
                        Icon(Icons.Default.Refresh, stringResource(R.string.refresh))
                    }
                }
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier.padding(padding).fillMaxSize(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            if (currentFrame != null) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .aspectRatio(4f / 3f)
                        .onSizeChanged { viewSize = it }
                ) {
                    Image(
                        bitmap = currentFrame!!.asImageBitmap(),
                        contentDescription = stringResource(R.string.stream_title),
                        modifier = Modifier.fillMaxSize(),
                        contentScale = ContentScale.Fit
                    )

                    if (detectionEnabled && detections.isNotEmpty() && viewSize != IntSize.Zero) {
                        val bitmapW = currentFrame!!.width.toFloat()
                        val bitmapH = currentFrame!!.height.toFloat()
                        val viewW = viewSize.width.toFloat()
                        val viewH = viewSize.height.toFloat()
                        val scale = minOf(viewW / bitmapW, viewH / bitmapH)
                        val offsetX = (viewW - bitmapW * scale) / 2f
                        val offsetY = (viewH - bitmapH * scale) / 2f

                        Canvas(modifier = Modifier.fillMaxSize()) {
                            for (det in detections) {
                                val color = when (det.label) {
                                    "Person" -> Color.Green
                                    "Cat" -> Color.Cyan
                                    "Dog" -> Color.Yellow
                                    "Animal" -> Color(0xFFFFA500) // arancione
                                    else -> Color.White
                                }
                                val left = offsetX + det.box.left * scale
                                val top = offsetY + det.box.top * scale
                                val right = offsetX + det.box.right * scale
                                val bottom = offsetY + det.box.bottom * scale

                                drawRect(
                                    color = color,
                                    topLeft = Offset(left, top),
                                    size = Size(right - left, bottom - top),
                                    style = Stroke(width = 3f)
                                )

                                val labelText = "${det.label} ${(det.confidence * 100).toInt()}%"
                                val paint = android.graphics.Paint().apply {
                                    this.color = android.graphics.Color.BLACK
                                    alpha = 180
                                    style = android.graphics.Paint.Style.FILL
                                }
                                val textPaint = android.graphics.Paint().apply {
                                    this.color = when (det.label) {
                                        "Person" -> android.graphics.Color.GREEN
                                        "Cat" -> android.graphics.Color.CYAN
                                        "Dog" -> android.graphics.Color.YELLOW
                                        "Animal" -> android.graphics.Color.rgb(255, 165, 0)
                                        else -> android.graphics.Color.WHITE
                                    }
                                    textSize = 32f
                                    isFakeBoldText = true
                                    isAntiAlias = true
                                }
                                val textWidth = textPaint.measureText(labelText)
                                drawContext.canvas.nativeCanvas.apply {
                                    drawRect(left, top - 38f, left + textWidth + 12f, top, paint)
                                    drawText(labelText, left + 6f, top - 8f, textPaint)
                                }
                            }
                        }
                    }

                    // Overlay "connessione persa" sopra il frame congelato
                    if (!isConnected && error != null) {
                        Box(
                            modifier = Modifier
                                .fillMaxSize()
                                .padding(16.dp),
                            contentAlignment = Alignment.Center
                        ) {
                            Card(
                                colors = CardDefaults.cardColors(
                                    containerColor = MaterialTheme.colorScheme.errorContainer.copy(alpha = 0.9f)
                                )
                            ) {
                                Column(
                                    modifier = Modifier.padding(24.dp),
                                    horizontalAlignment = Alignment.CenterHorizontally
                                ) {
                                    Icon(
                                        Icons.Default.VisibilityOff, null,
                                        tint = MaterialTheme.colorScheme.error,
                                        modifier = Modifier.size(36.dp)
                                    )
                                    Spacer(Modifier.height(8.dp))
                                    Text(
                                        "Connessione persa",
                                        style = MaterialTheme.typography.titleMedium,
                                        color = MaterialTheme.colorScheme.onErrorContainer
                                    )
                                    Spacer(Modifier.height(12.dp))
                                    Button(onClick = { retryTrigger++ }) {
                                        Text("Riconnetti")
                                    }
                                }
                            }
                        }
                    }
                }

                Spacer(Modifier.height(8.dp))

                Row(
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        if (isConnected) stringResource(R.string.online) else stringResource(R.string.offline),
                        color = if (isConnected) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodySmall
                    )

                    if (detectionEnabled && detections.isNotEmpty()) {
                        Text(
                            "${detections.size} rilevament${if (detections.size == 1) "o" else "i"}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.primary
                        )
                    } else if (detectionEnabled) {
                        Text(
                            "Detection attiva",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }

            } else if (error != null) {
                Text("${stringResource(R.string.stream_error)}: $error", color = MaterialTheme.colorScheme.error)
                Spacer(Modifier.height(16.dp))
                Button(onClick = { retryTrigger++ }) { Text(stringResource(R.string.refresh)) }
            } else {
                Text(stringResource(R.string.stream_connecting))
                Spacer(Modifier.height(8.dp))
                LinearProgressIndicator()
            }
        }
    }
}