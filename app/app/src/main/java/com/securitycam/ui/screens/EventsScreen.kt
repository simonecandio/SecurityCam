package com.securitycam.ui.screens

import android.content.ContentValues
import android.graphics.Bitmap
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.widget.Toast
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.securitycam.R
import com.securitycam.model.DetectionCategories
import com.securitycam.model.SecurityEvent
import com.securitycam.ui.theme.Green400
import com.securitycam.ui.theme.Red400
import com.securitycam.viewmodel.EventsViewModel
import java.text.SimpleDateFormat
import java.util.*

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EventsScreen(
    viewModel: EventsViewModel,
    deviceId: String,
    deviceName: String,
    ownerUid: String = "",
    onBack: () -> Unit
) {
    val state by viewModel.uiState.collectAsState()
    val context = LocalContext.current

    LaunchedEffect(deviceId) {
        viewModel.init(context, deviceId, deviceName, ownerUid)
    }

    if (state.selectedEvent != null) {
        EventDetailScreen(viewModel, onBack = { viewModel.clearSelection() })
        return
    }
    var selectionMode by remember { mutableStateOf(false) }
    var selectedIds by remember { mutableStateOf(setOf<String>()) }
    var showDeleteAllConfirm by remember { mutableStateOf(false) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("${stringResource(R.string.events)} - $deviceName") },
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(Icons.Default.ArrowBack, stringResource(R.string.back)) }
                },
                actions = {
                    if (selectionMode) {
                        Text(
                            "${selectedIds.size} ${stringResource(R.string.selected_count)}",
                            style = MaterialTheme.typography.bodyMedium
                        )
                        IconButton(onClick = {
                            viewModel.deleteSelectedEvents(selectedIds)
                            selectedIds = emptySet()
                            selectionMode = false
                        }) {
                            Icon(Icons.Default.Delete, stringResource(R.string.delete_selected), tint = MaterialTheme.colorScheme.error)
                        }
                        IconButton(onClick = {
                            selectionMode = false
                            selectedIds = emptySet()
                        }) {
                            Icon(Icons.Default.Close, stringResource(R.string.cancel))
                        }
                    } else {
                        // selezione e delete solo per device di proprietà
                        if (!state.isSharedDevice) {
                            IconButton(onClick = { selectionMode = true }) {
                                Icon(Icons.Default.Checklist, stringResource(R.string.select))
                            }
                            IconButton(onClick = { showDeleteAllConfirm = true }) {
                                Icon(Icons.Default.DeleteSweep, stringResource(R.string.delete_all_btn))
                            }
                        }
                        IconButton(onClick = { viewModel.loadEvents() }) {
                            Icon(Icons.Default.Refresh, stringResource(R.string.refresh))
                        }
                    }
                }
            )
        }
    ) { padding ->
        if (state.isLoading && state.events.isEmpty()) {
            Box(Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(stringResource(R.string.loading_events))
                    Spacer(Modifier.height(8.dp))
                    LinearProgressIndicator()
                }
            }
        } else if (state.events.isEmpty()) {
            Box(Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Icon(Icons.Default.EventBusy, null, Modifier.size(64.dp),
                        tint = MaterialTheme.colorScheme.onSurfaceVariant)
                    Spacer(Modifier.height(8.dp))
                    Text(stringResource(R.string.no_events))
                    Text(stringResource(R.string.events_hint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        } else {
            LazyColumn(
                modifier = Modifier.padding(padding).padding(horizontal = 16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
                contentPadding = PaddingValues(vertical = 8.dp)
            ) {
                items(state.events) { event ->
                    if (selectionMode) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Checkbox(
                                checked = event.id in selectedIds,
                                onCheckedChange = { checked ->
                                    selectedIds = if (checked) selectedIds + event.id
                                    else selectedIds - event.id
                                }
                            )
                            Box(modifier = Modifier.weight(1f)) {
                                EventCard(event = event,
                                    onClick = {
                                        selectedIds = if (event.id in selectedIds) selectedIds - event.id
                                        else selectedIds + event.id
                                    },
                                    onDelete = {},
                                    showDelete = false
                                )
                            }
                        }
                    } else {
                        EventCard(
                            event = event,
                            onClick = { viewModel.selectEvent(event) },
                            onDelete = { viewModel.deleteEvent(event) },
                            showDelete = !state.isSharedDevice
                        )
                    }
                }

                // pulsante "Carica altri" in fondo alla lista
                if (state.hasMoreEvents) {
                    item {
                        Box(
                            modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
                            contentAlignment = Alignment.Center
                        ) {
                            if (state.isLoadingMore) {
                                CircularProgressIndicator(modifier = Modifier.size(32.dp))
                            } else {
                                OutlinedButton(onClick = { viewModel.loadMoreEvents() }) {
                                    Icon(Icons.Default.ExpandMore, null, Modifier.size(18.dp))
                                    Spacer(Modifier.width(8.dp))
                                    Text(stringResource(R.string.load_more))
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (showDeleteAllConfirm) {
        AlertDialog(
            onDismissRequest = { showDeleteAllConfirm = false },
            title = { Text(stringResource(R.string.delete_all)) },
            text = { Text(stringResource(R.string.delete_all_msg)) },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.deleteAllEvents()
                    showDeleteAllConfirm = false
                }) { Text(stringResource(R.string.delete_all_btn), color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = {
                TextButton(onClick = { showDeleteAllConfirm = false }) { Text(stringResource(R.string.cancel)) }
            }
        )
    }
}

@Composable
fun EventCard(event: SecurityEvent, onClick: () -> Unit, onDelete: () -> Unit = {},
              showDelete: Boolean = true) {
    var showDeleteConfirm by remember { mutableStateOf(false) }
    val dateFormat = remember { SimpleDateFormat("dd/MM/yyyy HH:mm:ss", Locale.ITALY) }
    val timeStr = if (event.timestamp > 1704067200000) {
        dateFormat.format(Date(event.timestamp))
    } else {
        "Uptime: ${event.timestamp / 1000}s"
    }

    Card(
        modifier = Modifier.fillMaxWidth().clickable { onClick() },
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Row(modifier = Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(
                when {
                    event.validated -> Icons.Default.Verified
                    else -> Icons.Default.HelpOutline
                },
                null,
                tint = when {
                    event.validated -> Green400
                    else -> MaterialTheme.colorScheme.onSurfaceVariant
                },
                modifier = Modifier.size(40.dp)
            )
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text("${stringResource(R.string.events)} #${event.event_id}", style = MaterialTheme.typography.titleSmall)
                Text(timeStr, style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text("${event.frame_count} ${stringResource(R.string.frames)}", style = MaterialTheme.typography.bodySmall)
            }
            if (event.validated) {
                AssistChip(
                    onClick = {},
                    label = { Text(stringResource(R.string.validated), style = MaterialTheme.typography.labelSmall) },
                    leadingIcon = { Icon(Icons.Default.Visibility, null, Modifier.size(16.dp)) }
                )
            }
            if (showDelete) {
                IconButton(onClick = { showDeleteConfirm = true }) {
                    Icon(Icons.Default.Delete, stringResource(R.string.delete), tint = MaterialTheme.colorScheme.error)
                }
            }
        }
    }

    if (showDeleteConfirm) {
        AlertDialog(
            onDismissRequest = { showDeleteConfirm = false },
            title = { Text("${stringResource(R.string.delete)} ${stringResource(R.string.events)} #${event.event_id}?") },
            text = { Text(stringResource(R.string.delete_event_msg)) },
            confirmButton = {
                TextButton(onClick = { onDelete(); showDeleteConfirm = false }) { Text(stringResource(R.string.delete)) }
            },
            dismissButton = {
                TextButton(onClick = { showDeleteConfirm = false }) { Text(stringResource(R.string.cancel)) }
            }
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun EventDetailScreen(viewModel: EventsViewModel, onBack: () -> Unit) {
    val state by viewModel.uiState.collectAsState()
    val event = state.selectedEvent ?: return

    val allDetectedCategories = state.detectionResults.values
        .flatMap { it.detectedCategories }
        .toSet()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("${stringResource(R.string.events)} #${event.event_id}") },
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(Icons.Default.ArrowBack, stringResource(R.string.back)) }
                }
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier.padding(padding).fillMaxSize().padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            if (state.decodedBitmaps.isNotEmpty()) {
                Text("${stringResource(R.string.photos)} (${state.decodedBitmaps.size})", style = MaterialTheme.typography.titleMedium)
                LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    items(state.decodedBitmaps.entries.toList()) { (index, bitmap) ->
                        val context = LocalContext.current
                        Card {
                            Column {
                                Box {
                                    Image(
                                        bitmap = bitmap.asImageBitmap(),
                                        contentDescription = "Frame $index",
                                        modifier = Modifier.width(280.dp).aspectRatio(4f / 3f),
                                        contentScale = ContentScale.Fit
                                    )
                                    // Bottone salva nell'angolo in basso a destra
                                    IconButton(
                                        onClick = {
                                            val eventId = state.selectedEvent?.event_id ?: 0
                                            val saved = saveBitmapToGallery(
                                                context, bitmap,
                                                "SecurityCam_evt${eventId}_frame$index"
                                            )
                                            Toast.makeText(
                                                context,
                                                if (saved) "Foto salvata nella galleria"
                                                else "Errore nel salvataggio",
                                                Toast.LENGTH_SHORT
                                            ).show()
                                        },
                                        modifier = Modifier.align(Alignment.BottomEnd)
                                    ) {
                                        Icon(
                                            Icons.Default.Download, "Salva foto",
                                            tint = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.8f)
                                        )
                                    }
                                }
                                state.detectionResults[index]?.let { result ->
                                    if (result.detectedCategories.isNotEmpty()) {
                                        FlowRow(
                                            modifier = Modifier.padding(8.dp),
                                            horizontalArrangement = Arrangement.spacedBy(4.dp),
                                            verticalArrangement = Arrangement.spacedBy(4.dp)
                                        ) {
                                            result.detectedCategories.forEach { cat ->
                                                val (icon, label) = categoryIconAndLabel(cat)
                                                AssistChip(
                                                    onClick = {},
                                                    label = { Text("${label} ${(result.confidence * 100).toInt()}%",
                                                        style = MaterialTheme.typography.labelSmall) },
                                                    leadingIcon = { Icon(icon, null, Modifier.size(14.dp)) },
                                                    modifier = Modifier.height(28.dp)
                                                )
                                            }
                                        }
                                    } else if (result.confidence == 0f) {
                                        Text(
                                            stringResource(R.string.no_person),
                                            modifier = Modifier.padding(8.dp),
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            style = MaterialTheme.typography.bodySmall
                                        )
                                    }
                                }
                            }
                        }
                    }
                }
            } else if (state.isLoading) {
                Box(Modifier.fillMaxWidth().height(200.dp), contentAlignment = Alignment.Center) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(stringResource(R.string.loading_photos))
                        Spacer(Modifier.height(8.dp))
                        LinearProgressIndicator()
                    }
                }
            } else {
                // Frame non trovati: l'ESP potrebbe non aver completato
                // l'upload (rete caduta durante il caricamento) oppure i
                // frame sono stati cancellati da un'operazione precedente.
                Box(Modifier.fillMaxWidth().height(200.dp), contentAlignment = Alignment.Center) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Icon(
                            Icons.Default.BrokenImage, null,
                            modifier = Modifier.size(48.dp),
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                        Spacer(Modifier.height(8.dp))
                        Text(
                            "Frame non disponibili",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                        Text(
                            "L'upload potrebbe essere fallito",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }

            if (allDetectedCategories.isNotEmpty()) {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    allDetectedCategories.forEach { cat ->
                        val (icon, label) = categoryIconAndLabel(cat)
                        AssistChip(
                            onClick = {},
                            label = { Text(label) },
                            leadingIcon = { Icon(icon, null, Modifier.size(18.dp), tint = Green400) }
                        )
                    }
                }
            }

            Card {
                Column(modifier = Modifier.padding(16.dp).fillMaxWidth()) {
                    Text(stringResource(R.string.details), style = MaterialTheme.typography.titleMedium)
                    Spacer(Modifier.height(8.dp))
                    StatRow(stringResource(R.string.event_id), "${event.event_id}")
                    StatRow(stringResource(R.string.frames), "${event.frame_count}")
                    StatRow("PIR", "${event.pir_value}")
                    StatRow(stringResource(R.string.validated), if (event.validated) stringResource(R.string.detection_confirmed) else stringResource(R.string.no_detection))
                }
            }
        }
    }
}

/**
 * Su Android 10+ usa MediaStore (ContentProvider).
 */
private fun saveBitmapToGallery(
    context: android.content.Context,
    bitmap: Bitmap,
    filename: String
): Boolean {
    return try {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            // Android 10+ (API 29+): MediaStore
            val contentValues = ContentValues().apply {
                put(MediaStore.Images.Media.DISPLAY_NAME, "$filename.jpg")
                put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
                put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/SecurityCam")
            }
            val uri = context.contentResolver.insert(
                MediaStore.Images.Media.EXTERNAL_CONTENT_URI, contentValues
            ) ?: return false

            context.contentResolver.openOutputStream(uri)?.use { stream ->
                bitmap.compress(Bitmap.CompressFormat.JPEG, 90, stream)
            }
            true
        } else {
            // Android <10: salvataggio tradizionale
            @Suppress("DEPRECATION")
            val dir = java.io.File(
                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES),
                "SecurityCam"
            )
            if (!dir.exists()) dir.mkdirs()
            val file = java.io.File(dir, "$filename.jpg")
            java.io.FileOutputStream(file).use { stream ->
                bitmap.compress(Bitmap.CompressFormat.JPEG, 90, stream)
            }
            // Notifica la galleria che c'e' un nuovo file
            val mediaScanIntent = android.content.Intent(
                android.content.Intent.ACTION_MEDIA_SCANNER_SCAN_FILE,
                android.net.Uri.fromFile(file)
            )
            context.sendBroadcast(mediaScanIntent)
            true
        }
    } catch (e: Exception) {
        android.util.Log.e("EventsScreen", "Errore salvataggio: ${e.message}")
        false
    }
}