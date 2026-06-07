@file:OptIn(ExperimentalMaterial3Api::class)
package com.securitycam.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.securitycam.R
import com.securitycam.ui.theme.Green400
import com.securitycam.ui.theme.Red400
import com.securitycam.viewmodel.DashboardViewModel
import androidx.compose.ui.platform.LocalContext

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DashboardScreen(
    viewModel: DashboardViewModel,
    deviceName: String,
    deviceIp: String,
    deviceId: String = "",
    ownerUid: String = "",
    isTablet: Boolean = false,
    onStream: () -> Unit,
    onEvents: () -> Unit,
    onConfig: () -> Unit,
    onBack: () -> Unit
) {
    val state by viewModel.uiState.collectAsState()
    val context = LocalContext.current

    LaunchedEffect(deviceIp) {
        viewModel.setDevice(deviceName, deviceIp, deviceId, ownerUid, context)
        viewModel.startPolling()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(deviceName)
                        if (state.isSharedDevice) {
                            Text(
                                stringResource(R.string.shared_badge),
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                },
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(Icons.Default.ArrowBack, stringResource(R.string.back)) }
                }
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // stato connessione
            if (!state.statusLoaded) {
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surfaceVariant
                    )
                ) {
                    Row(
                        modifier = Modifier.padding(16.dp).fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                        Spacer(Modifier.width(12.dp))
                        Text("Verifica stato dispositivo...", style = MaterialTheme.typography.titleMedium)
                    }
                }
            } else {
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = if (state.isDeviceOnline)
                            MaterialTheme.colorScheme.primaryContainer
                        else MaterialTheme.colorScheme.errorContainer
                    )
                ) {
                    Row(
                        modifier = Modifier.padding(16.dp).fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Icon(
                            if (state.isDeviceOnline) Icons.Default.Wifi else Icons.Default.WifiOff,
                            null, tint = if (state.isDeviceOnline) Green400 else Red400
                        )
                        Spacer(Modifier.width(12.dp))
                        Column {
                            Text(
                                if (state.isDeviceOnline) stringResource(R.string.online) else stringResource(R.string.offline),
                                style = MaterialTheme.typography.titleMedium
                            )
                            Text("IP: $deviceIp", style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }

            // stato periferiche e statistiche
            // Mostro SOLO quando il device e' online: se e' offline
            // i dati sarebbero stantii (dall'ultima sessione) e
            // confonderebbero l'utente (uptime, heap, ecc. vecchi).
            if (state.status != null && state.isDeviceOnline) {
                val s = state.status!!

                if (isTablet) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(16.dp)
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(stringResource(R.string.peripherals_status), style = MaterialTheme.typography.titleMedium)
                            Spacer(Modifier.height(8.dp))
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                StatusChip("CAM", s.camera_ok, Modifier.weight(1f))
                                StatusChip("PIR", s.pir_ok, Modifier.weight(1f))
                                StatusChip("SD", s.sd_card_ok, Modifier.weight(1f))
                                StatusChip("WiFi", s.wifi_ok, Modifier.weight(1f))
                            }
                        }
                        Column(modifier = Modifier.weight(1f)) {
                            Text(stringResource(R.string.statistics), style = MaterialTheme.typography.titleMedium)
                            Spacer(Modifier.height(8.dp))
                            Card {
                                Column(modifier = Modifier.padding(16.dp).fillMaxWidth()) {
                                    StatRow("Rilevamenti totali", "${s.total_events}")
                                    StatRow(stringResource(R.string.failed_uploads), "${s.failed_uploads}")
                                    StatRow(stringResource(R.string.uptime), "${s.uptime_s / 60}m ${s.uptime_s % 60}s")
                                    StatRow(stringResource(R.string.state), s.state)
                                    StatRow(stringResource(R.string.firmware), s.firmware)
                                }
                            }
                        }
                    }
                } else {
                    Text(stringResource(R.string.peripherals_status), style = MaterialTheme.typography.titleMedium)

                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        StatusChip("CAM", s.camera_ok, Modifier.weight(1f))
                        StatusChip("PIR", s.pir_ok, Modifier.weight(1f))
                        StatusChip("SD", s.sd_card_ok, Modifier.weight(1f))
                        StatusChip("WiFi", s.wifi_ok, Modifier.weight(1f))
                    }

                    Card {
                        Column(modifier = Modifier.padding(16.dp).fillMaxWidth()) {
                            Text(stringResource(R.string.statistics), style = MaterialTheme.typography.titleMedium)
                            Spacer(Modifier.height(8.dp))
                            StatRow("Rilevamenti totali", "${s.total_events}")
                            StatRow(stringResource(R.string.failed_uploads), "${s.failed_uploads}")
                            StatRow(stringResource(R.string.uptime), "${s.uptime_s / 60}m ${s.uptime_s % 60}s")
                            StatRow(stringResource(R.string.state), s.state)
                            StatRow(stringResource(R.string.firmware), s.firmware)
                        }
                    }
                }
            }

            // pulsanti azione
            Text(stringResource(R.string.actions), style = MaterialTheme.typography.titleMedium)

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ActionButton(stringResource(R.string.stream), Icons.Default.Videocam, onStream, Modifier.weight(1f),
                    enabled = state.isDeviceOnline && !state.isRemoteStatus)
                ActionButton(stringResource(R.string.events), Icons.Default.History, onEvents, Modifier.weight(1f))
                ActionButton(stringResource(R.string.config), Icons.Default.Settings, onConfig, Modifier.weight(1f),
                    enabled = !state.isSharedDevice)
            }

            if (state.isRemoteStatus) {
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surfaceVariant
                    )
                ) {
                    Text(
                        stringResource(R.string.cloud_message),
                        modifier = Modifier.padding(12.dp),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

@Composable
fun StatusChip(label: String, ok: Boolean, modifier: Modifier = Modifier) {
    Card(
        modifier = modifier,
        colors = CardDefaults.cardColors(
            containerColor = if (ok) Color(0xFF1B5E20).copy(alpha = 0.15f)
            else Color(0xFFB71C1C).copy(alpha = 0.15f)
        )
    ) {
        Column(
            modifier = Modifier.padding(12.dp).fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Icon(
                if (ok) Icons.Default.CheckCircle else Icons.Default.Error,
                null, tint = if (ok) Green400 else Red400, modifier = Modifier.size(24.dp)
            )
            Spacer(Modifier.height(4.dp))
            Text(label, style = MaterialTheme.typography.labelSmall)
        }
    }
}

@Composable
fun StatRow(label: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
        Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable
fun ActionButton(label: String, icon: ImageVector, onClick: () -> Unit,
                 modifier: Modifier = Modifier, enabled: Boolean = true) {
    OutlinedCard(
        modifier = modifier,
        onClick = onClick,
        enabled = enabled
    ) {
        Column(
            modifier = Modifier.padding(16.dp).fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Icon(icon, null, modifier = Modifier.size(32.dp),
                tint = if (enabled) MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(4.dp))
            Text(label, style = MaterialTheme.typography.labelMedium)
        }
    }
}