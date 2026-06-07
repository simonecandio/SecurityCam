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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.securitycam.R
import com.securitycam.model.DetectionCategories
import com.securitycam.viewmodel.ConfigViewModel

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun ConfigScreen(viewModel: ConfigViewModel, deviceIp: String, deviceId: String = "", onBack: () -> Unit) {
    val state by viewModel.uiState.collectAsState()

    LaunchedEffect(deviceIp) { viewModel.setDevice(deviceIp, deviceId) }

    var showWifiDialog by remember { mutableStateOf(false) }
    var showResetConfirm by remember { mutableStateOf(false) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.config)) },
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
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {

            // categorie monitoraggio
            Text(stringResource(R.string.monitor_categories), style = MaterialTheme.typography.titleSmall)
            Text(stringResource(R.string.select_categories), style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                DetectionCategories.ALL.forEach { cat ->
                    val (icon, label) = categoryIconAndLabel(cat)
                    val selected = cat in state.monitorCategories
                    FilterChip(
                        selected = selected,
                        onClick = { viewModel.toggleCategory(cat) },
                        label = { Text(label) },
                        leadingIcon = { Icon(icon, null, Modifier.size(18.dp)) }
                    )
                }
            }

            HorizontalDivider()

            // parametri ESP
            Text("${stringResource(R.string.cooldown)}: ${state.cooldownMs / 1000}s", style = MaterialTheme.typography.titleSmall)
            Slider(
                value = state.cooldownMs.toFloat(),
                onValueChange = { viewModel.setCooldown(it.toInt()) },
                valueRange = 5000f..60000f,
                steps = 10,
            )

            Text("${stringResource(R.string.frame_count)}: ${state.frameCount}", style = MaterialTheme.typography.titleSmall)
            Slider(
                value = state.frameCount.toFloat(),
                onValueChange = { viewModel.setFrameCount(it.toInt()) },
                valueRange = 1f..5f,
                steps = 3,
            )

            Text("${stringResource(R.string.jpeg_quality)}: ${state.jpegQuality}", style = MaterialTheme.typography.titleSmall)
            Slider(
                value = state.jpegQuality.toFloat(),
                onValueChange = { viewModel.setJpegQuality(it.toInt()) },
                valueRange = 5f..50f,
                steps = 8,
            )

            // pulsanti azione
            Button(
                onClick = { viewModel.saveConfig() },
                enabled = !state.isSaving,
                modifier = Modifier.fillMaxWidth()
            ) {
                Icon(Icons.Default.Save, null)
                Spacer(Modifier.width(8.dp))
                Text(if (state.isSaving) stringResource(R.string.loading) else stringResource(R.string.save))
            }

            OutlinedButton(
                onClick = { viewModel.rebootDevice() },
                enabled = !state.isRebooting,
                modifier = Modifier.fillMaxWidth()
            ) {
                Icon(Icons.Default.RestartAlt, null)
                Spacer(Modifier.width(8.dp))
                Text(if (state.isRebooting) stringResource(R.string.loading) else stringResource(R.string.reboot))
            }

            HorizontalDivider()

            // sezione avanzata
            Text("Avanzate", style = MaterialTheme.typography.titleSmall)

            OutlinedButton(
                onClick = { showWifiDialog = true },
                enabled = state.isDeviceOnline,
                modifier = Modifier.fillMaxWidth()
            ) {
                Icon(Icons.Default.Wifi, null)
                Spacer(Modifier.width(8.dp))
                Text("Cambia WiFi")
            }

            OutlinedButton(
                onClick = { showResetConfirm = true },
                enabled = state.isDeviceOnline,
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error)
            ) {
                Icon(Icons.Default.DeleteForever, null)
                Spacer(Modifier.width(8.dp))
                Text("Factory Reset")
            }

            // messaggio
            state.message?.let { msg ->
                Text(msg, style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.primary)
            }

            Spacer(Modifier.height(16.dp))
        }
    }

    // dialog cambia WiFi
    if (showWifiDialog) {
        var newSsid by remember { mutableStateOf("") }
        var newPass by remember { mutableStateOf("") }
        AlertDialog(
            onDismissRequest = { showWifiDialog = false },
            title = { Text("Cambia WiFi") },
            text = {
                Column {
                    Text("L'ESP si riavviera e si colleghera alla nuova rete.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Spacer(Modifier.height(12.dp))
                    OutlinedTextField(
                        value = newSsid, onValueChange = { newSsid = it },
                        label = { Text("SSID") }, singleLine = true,
                        modifier = Modifier.fillMaxWidth()
                    )
                    Spacer(Modifier.height(8.dp))
                    OutlinedTextField(
                        value = newPass, onValueChange = { newPass = it },
                        label = { Text("Password") }, singleLine = true,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            },
            confirmButton = {
                TextButton(
                    onClick = { viewModel.changeWifi(newSsid, newPass); showWifiDialog = false },
                    enabled = newSsid.isNotBlank()
                ) { Text(stringResource(R.string.save)) }
            },
            dismissButton = {
                TextButton(onClick = { showWifiDialog = false }) { Text(stringResource(R.string.cancel)) }
            }
        )
    }

    // dialog conferma factory reset
    if (showResetConfirm) {
        AlertDialog(
            onDismissRequest = { showResetConfirm = false },
            title = { Text("Factory Reset") },
            text = { Text("L'ESP perdera tutte le impostazioni (WiFi, Firebase, certificati) e tornera in modalita configurazione. Continuare?") },
            confirmButton = {
                TextButton(
                    onClick = { viewModel.factoryReset(); showResetConfirm = false },
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error)
                ) { Text("Reset") }
            },
            dismissButton = {
                TextButton(onClick = { showResetConfirm = false }) { Text(stringResource(R.string.cancel)) }
            }
        )
    }
}