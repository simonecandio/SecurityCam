package com.securitycam.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.securitycam.R
import com.securitycam.model.DetectionCategories
import com.securitycam.model.Device
import com.securitycam.util.PreferencesManager
import com.securitycam.viewmodel.DevicesViewModel
import androidx.compose.runtime.LaunchedEffect


@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DevicesScreen(
    viewModel: DevicesViewModel,
    onDeviceClick: (Device) -> Unit,
    onLogout: () -> Unit,
    onProfile: () -> Unit = {}
) {
    val state by viewModel.uiState.collectAsState()
    val context = LocalContext.current
    val prefsManager = remember { PreferencesManager(context) }
    LaunchedEffect(Unit) {
        // Cancello la preferenza "ultimo device aperto" ogni volta che
        // l'utente vede la schermata della lista device. Cosi':
        // - Se chiude l'app dalla lista → riapre sulla lista (corretto)
        // - Se clicca un device → saveLastDevice lo riscrive → se chiude
        //   dalla dashboard → riapre sulla dashboard di quel device (corretto)
        prefsManager.clearLastDevice()
        viewModel.loadDevices()
    }

    // Scaffold è uan struttura standard Material3 della schermata (topBar + content).
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.my_devices)) },
                actions = {
                    // pulsante per inserire codice invito
                    IconButton(onClick = { viewModel.showJoinDialog() }) {
                        Icon(Icons.Default.GroupAdd, stringResource(R.string.join_device))
                    }
                    // profilo account (cambio password, logout)
                    IconButton(onClick = onProfile) {
                        Icon(Icons.Default.AccountCircle, "Profilo")
                    }
                }
            )
        },
        floatingActionButton = {
            FloatingActionButton(onClick = { viewModel.showAddDialog() }) {
                Icon(Icons.Default.Add, stringResource(R.string.add_device))
            }
        }
    ) { padding ->
        Column(modifier = Modifier.padding(padding).fillMaxSize()) {
            if (state.isLoading) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(stringResource(R.string.loading))
                }
            } else if (state.devices.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Icon(Icons.Default.CameraAlt, null, Modifier.size(64.dp),
                            tint = MaterialTheme.colorScheme.onSurfaceVariant)
                        Spacer(Modifier.height(16.dp))
                        Text(stringResource(R.string.no_devices), style = MaterialTheme.typography.titleMedium)
                        Text(stringResource(R.string.add_device_hint),
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            } else {
                LazyColumn(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    items(state.devices) { device ->
                        DeviceCard(
                            device = device,
                            /*
                            //Per ogni elemento della lista, Compose crea una Card, e ognuna di loro
                            //cattura l'oggetto di quella specifica iterazione cosi dopo si sanno tutti i dati del device
                             */
                            onClick = {
                                prefsManager.saveLastDevice(device.id, device.name, device.ip)
                                onDeviceClick(device)
                            },
                            onDelete = { viewModel.requestRemoveDevice(device.id) },
                            onShare = { viewModel.shareDevice(device.id) }
                        )
                    }
                }
            }
        }

        // dialog aggiunta dispositivo
        if (state.showAddDialog) {
            AddDeviceDialog(
                ip = state.addIp,
                name = state.addName,
                onIpChange = { viewModel.updateAddIp(it) },
                onNameChange = { viewModel.updateAddName(it) },
                onVerify = { viewModel.verifyDevice() },
                onAdd = { categories -> viewModel.addDevice(categories) },
                onDismiss = { viewModel.hideAddDialog() },
                isVerifying = state.isVerifying,
                isAdding = state.isAdding,
                deviceVerified = state.deviceVerified,
                message = state.addMessage
            )
        }

        // dialog conferma rimozione
        if (state.showRemoveDialog) {
            RemoveDeviceDialog(
                deviceName = state.removeDeviceName ?: "",
                isShared = state.removeDeviceIsShared,
                onRemoveFromApp = { viewModel.removeFromAppOnly(context) },
                onFactoryReset = { viewModel.removeAndFactoryReset(context) },
                onDismiss = { viewModel.cancelRemoveDevice() }
            )
        }

        // dialog codice condivisione (owner mostra il codice)
        if (state.showShareDialog) {
            ShareCodeDialog(
                deviceName = state.shareDeviceName ?: "",
                code = state.shareCode,
                isLoading = state.isGeneratingCode,
                onDismiss = { viewModel.hideShareDialog() }
            )
        }

        // dialog per inserire codice invito (utente invitato)
        if (state.showJoinDialog) {
            JoinDeviceDialog(
                code = state.joinCode,
                onCodeChange = { viewModel.updateJoinCode(it) },
                onAccept = { viewModel.acceptInvite() },
                onDismiss = { viewModel.hideJoinDialog() },
                isJoining = state.isJoining,
                message = state.joinMessage
            )
        }
    }
}

/**
 * Dialog di conferma rimozione con due opzioni.
 * Per i device condivisi, il factory reset non e' disponibile.
 */
@Composable
fun RemoveDeviceDialog(
    deviceName: String,
    isShared: Boolean,
    onRemoveFromApp: () -> Unit,
    onFactoryReset: () -> Unit,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        icon = { Icon(Icons.Default.Warning, null, tint = MaterialTheme.colorScheme.error) },
        title = { Text(stringResource(R.string.remove_device, deviceName)) },
        text = {
            Column {
                Text(stringResource(R.string.remove_device_choose))
                Spacer(Modifier.height(16.dp))

                // opzione 1: rimuovi solo dall'app
                OutlinedCard(
                    modifier = Modifier.fillMaxWidth().clickable { onRemoveFromApp() }
                ) {
                    Row(
                        modifier = Modifier.padding(16.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Icon(Icons.Default.RemoveCircleOutline, null,
                            tint = MaterialTheme.colorScheme.primary)
                        Spacer(Modifier.width(12.dp))
                        Column {
                            Text(stringResource(R.string.remove_app_only),
                                style = MaterialTheme.typography.titleSmall)
                            Text(stringResource(R.string.remove_app_only_desc),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }

                // opzione 2: rimuovi e resetta (solo per device di proprietà)
                if (!isShared) {
                    Spacer(Modifier.height(8.dp))
                    OutlinedCard(
                        modifier = Modifier.fillMaxWidth().clickable { onFactoryReset() }
                    ) {
                        Row(
                            modifier = Modifier.padding(16.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(Icons.Default.DeleteForever, null,
                                tint = MaterialTheme.colorScheme.error)
                            Spacer(Modifier.width(12.dp))
                            Column {
                                Text(stringResource(R.string.remove_and_reset),
                                    style = MaterialTheme.typography.titleSmall,
                                    color = MaterialTheme.colorScheme.error)
                                Text(stringResource(R.string.remove_and_reset_desc),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {},
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) }
        }
    )
}

/**
 * Dialog che mostra il codice invito generato.
 * L'owner lo comunica all'invitato che lo inserisce nel suo app.
 */
@Composable
fun ShareCodeDialog(
    deviceName: String,
    code: String?,
    isLoading: Boolean,
    onDismiss: () -> Unit
) {
    val clipboardManager = LocalClipboardManager.current

    AlertDialog(
        onDismissRequest = onDismiss,
        icon = { Icon(Icons.Default.Share, null, tint = MaterialTheme.colorScheme.primary) },
        title = { Text(stringResource(R.string.share_device)) },
        text = {
            Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.share_device_desc, deviceName),
                    style = MaterialTheme.typography.bodyMedium)
                Spacer(Modifier.height(20.dp))

                if (isLoading) {
                    CircularProgressIndicator()
                } else if (code != null) {
                    // codice grande e copiabile
                    Text(
                        text = code,
                        fontSize = 36.sp,
                        fontWeight = FontWeight.Bold,
                        letterSpacing = 8.sp,
                        textAlign = TextAlign.Center,
                        color = MaterialTheme.colorScheme.primary
                    )
                    Spacer(Modifier.height(12.dp))
                    OutlinedButton(onClick = {
                        clipboardManager.setText(AnnotatedString(code))
                    }) {
                        Icon(Icons.Default.ContentCopy, null, Modifier.size(18.dp))
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.copy_code))
                    }
                    Spacer(Modifier.height(12.dp))
                    Text(stringResource(R.string.share_code_expires),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                } else {
                    Text(stringResource(R.string.share_code_error),
                        color = MaterialTheme.colorScheme.error)
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.done)) }
        },
        dismissButton = {}
    )
}

/**
 * Dialog per inserire un codice invito e unirsi a un device condiviso.
 */
@Composable
fun JoinDeviceDialog(
    code: String,
    onCodeChange: (String) -> Unit,
    onAccept: () -> Unit,
    onDismiss: () -> Unit,
    isJoining: Boolean,
    message: String?
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        icon = { Icon(Icons.Default.GroupAdd, null, tint = MaterialTheme.colorScheme.primary) },
        title = { Text(stringResource(R.string.join_device)) },
        text = {
            Column {
                Text(stringResource(R.string.join_device_desc),
                    style = MaterialTheme.typography.bodyMedium)
                Spacer(Modifier.height(16.dp))
                OutlinedTextField(
                    value = code,
                    onValueChange = onCodeChange,
                    label = { Text(stringResource(R.string.invite_code)) },
                    placeholder = { Text("123456") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                    textStyle = LocalTextStyle.current.copy(
                        fontSize = 24.sp,
                        letterSpacing = 4.sp,
                        textAlign = TextAlign.Center
                    ),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
                )
                if (message != null) {
                    Spacer(Modifier.height(8.dp))
                    Text(message, color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodySmall)
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onAccept, enabled = code.length == 6 && !isJoining) {
                Text(if (isJoining) stringResource(R.string.joining) else stringResource(R.string.join))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) }
        }
    )
}

@Composable
fun DeviceCard(device: Device, onClick: () -> Unit, onDelete: () -> Unit, onShare: () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth().clickable { onClick() },
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Row(
            modifier = Modifier.padding(16.dp).fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(Icons.Default.Videocam, null, Modifier.size(40.dp),
                tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(device.name, style = MaterialTheme.typography.titleMedium)
                    // badge "condiviso" per i device non di proprietà
                    if (device.isShared) {
                        Spacer(Modifier.width(8.dp))
                        AssistChip(
                            onClick = {},
                            label = { Text(stringResource(R.string.shared_badge), style = MaterialTheme.typography.labelSmall) },
                            leadingIcon = { Icon(Icons.Default.People, null, Modifier.size(14.dp)) },
                            modifier = Modifier.height(24.dp)
                        )
                    }
                }
                Text("IP: ${device.ip}", style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (device.monitorCategories.isNotEmpty()) {
                    Row(
                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                        modifier = Modifier.padding(top = 4.dp)
                    ) {
                        device.monitorCategories.forEach { cat ->
                            val (icon, label) = categoryIconAndLabel(cat)
                            AssistChip(
                                onClick = {},
                                label = { Text(label, style = MaterialTheme.typography.labelSmall) },
                                leadingIcon = { Icon(icon, null, Modifier.size(14.dp)) },
                                modifier = Modifier.height(28.dp)
                            )
                        }
                    }
                }
            }
            // pulsante condividi (solo per device di proprietà)
            if (!device.isShared) {
                IconButton(onClick = onShare) {
                    Icon(Icons.Default.Share, stringResource(R.string.share_device),
                        tint = MaterialTheme.colorScheme.primary)
                }
            }
            IconButton(onClick = onDelete) {
                Icon(Icons.Default.Delete, stringResource(R.string.delete),
                    tint = MaterialTheme.colorScheme.error)
            }
        }
    }
}

@Composable
fun AddDeviceDialog(
    ip: String, name: String,
    onIpChange: (String) -> Unit, onNameChange: (String) -> Unit,
    onVerify: () -> Unit, onAdd: (List<String>) -> Unit, onDismiss: () -> Unit,
    isVerifying: Boolean, isAdding: Boolean, deviceVerified: Boolean, message: String?
) {
    var selectedCategories by remember { mutableStateOf(setOf(DetectionCategories.PERSON)) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.add_device)) },
        text = {
            Column {
                Text(
                    "Assicurati di essere collegato alla stessa rete WiFi del dispositivo",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.height(12.dp))
                OutlinedTextField(
                    value = ip, onValueChange = onIpChange,
                    label = { Text(stringResource(R.string.device_ip_hint)) },
                    placeholder = { Text("172.20.10.2") },
                    singleLine = true, modifier = Modifier.fillMaxWidth()
                )
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = name, onValueChange = onNameChange,
                    label = { Text(stringResource(R.string.device_name_hint)) },
                    singleLine = true, modifier = Modifier.fillMaxWidth()
                )
                Spacer(Modifier.height(12.dp))

                Text(stringResource(R.string.select_categories), style = MaterialTheme.typography.labelLarge)
                Spacer(Modifier.height(6.dp))

                CategoryChipRow(
                    selectedCategories = selectedCategories,
                    onToggle = { cat ->
                        selectedCategories = if (cat in selectedCategories) {
                            if (selectedCategories.size > 1) selectedCategories - cat else selectedCategories
                        } else {
                            selectedCategories + cat
                        }
                    }
                )

                Spacer(Modifier.height(8.dp))

                if (!deviceVerified) {
                    Button(onClick = onVerify, enabled = !isVerifying && ip.isNotBlank(),
                        modifier = Modifier.fillMaxWidth()) {
                        Text(if (isVerifying) stringResource(R.string.verifying) else stringResource(R.string.verify_connection))
                    }
                }

                if (message != null) {
                    Spacer(Modifier.height(8.dp))
                    Text(message, color = if (deviceVerified) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodySmall)
                }
            }
        },
        confirmButton = {
            // Il bottone "Aggiungi" e' abilitato SOLO dopo che la verifica
            // ha confermato che il dispositivo e' raggiungibile (= stessa rete).
            // Senza verifica, l'ESP non riceverebbe mai owner_uid+device_id
            // via HTTP e resterebbe non associato all'utente.
            TextButton(
                onClick = { onAdd(selectedCategories.toList()) },
                enabled = deviceVerified && !isAdding
            ) {
                Text(if (isAdding) stringResource(R.string.adding) else stringResource(R.string.add))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) }
        }
    )
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun CategoryChipRow(selectedCategories: Set<String>, onToggle: (String) -> Unit) {
    FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        DetectionCategories.ALL.forEach { cat ->
            val (icon, label) = categoryIconAndLabel(cat)
            val selected = cat in selectedCategories
            FilterChip(
                selected = selected,
                onClick = { onToggle(cat) },
                label = { Text(label) },
                leadingIcon = { Icon(icon, null, Modifier.size(18.dp)) }
            )
        }
    }
}

@Composable
fun categoryIconAndLabel(category: String): Pair<androidx.compose.ui.graphics.vector.ImageVector, String> {
    return when (category) {
        DetectionCategories.PERSON -> Icons.Default.Person to stringResource(R.string.category_person)
        DetectionCategories.DOG -> Icons.Default.Pets to stringResource(R.string.category_dog)
        DetectionCategories.CAT -> Icons.Default.Pets to stringResource(R.string.category_cat)
        else -> Icons.Default.HelpOutline to category
    }
}