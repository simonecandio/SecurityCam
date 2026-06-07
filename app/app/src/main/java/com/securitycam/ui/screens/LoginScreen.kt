package com.securitycam.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import com.securitycam.viewmodel.AuthViewModel
import androidx.compose.ui.res.stringResource
import com.securitycam.R

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LoginScreen(viewModel: AuthViewModel) {
    val state by viewModel.uiState.collectAsState()
    var email by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var isRegister by remember { mutableStateOf(false) }
    var showPassword by remember { mutableStateOf(false) }

    Surface(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Icon(
                Icons.Default.CameraAlt,
                contentDescription = null,
                modifier = Modifier.size(72.dp),
                tint = MaterialTheme.colorScheme.primary
            )
            Spacer(modifier = Modifier.height(8.dp))
            Text("SecurityCam", style = MaterialTheme.typography.headlineLarge)
            Text(
                if (isRegister) stringResource(R.string.create_account) else stringResource(R.string.login_subtitle),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(modifier = Modifier.height(32.dp))

            OutlinedTextField(
                value = email,
                onValueChange = { email = it },
                label = { Text(stringResource(R.string.email_hint)) },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email),
                singleLine = true,
                modifier = Modifier
                    .fillMaxWidth(0.80f)
                    .widthIn(max = 400.dp)
            )
            Spacer(modifier = Modifier.height(12.dp))

            OutlinedTextField(
                value = password,
                onValueChange = { password = it },
                label = { Text("Password") },
                singleLine = true,
                visualTransformation = if (showPassword) VisualTransformation.None else PasswordVisualTransformation(),
                trailingIcon = {
                    IconButton(onClick = { showPassword = !showPassword }) {
                        Icon(
                            if (showPassword) Icons.Default.VisibilityOff else Icons.Default.Visibility,
                            contentDescription = stringResource(R.string.show_password)
                        )
                    }
                },
                modifier = Modifier
                    .fillMaxWidth(0.80f)
                    .widthIn(max = 400.dp)
            )
            Spacer(modifier = Modifier.height(24.dp))

            if (state.error != null) {
                Text(state.error!!, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                Spacer(modifier = Modifier.height(8.dp))
            }

            if (state.successMessage != null) {
                Text(state.successMessage!!, color = MaterialTheme.colorScheme.primary, style = MaterialTheme.typography.bodySmall)
                Spacer(modifier = Modifier.height(8.dp))
            }

            Button(
                onClick = {
                    if (isRegister) viewModel.register(email, password)
                    else viewModel.login(email, password)
                },
                enabled = !state.isLoading,
                modifier = Modifier
                    .fillMaxWidth(0.80f)
                    .height(48.dp)
                    .widthIn(max = 400.dp)
            ) {
                if (state.isLoading) {
                    Text(stringResource(R.string.loading))
                } else {
                    Text(if (isRegister) stringResource(R.string.register_button) else stringResource(R.string.login_button))
                }
            }
            Spacer(modifier = Modifier.height(12.dp))

            if (!isRegister) {
                var showResetDialog by remember { mutableStateOf(false) }
                var resetEmail by remember { mutableStateOf("") }

                TextButton(onClick = { resetEmail = email; showResetDialog = true }) {
                    Text(stringResource(R.string.forgot_password))
                }

                if (showResetDialog) {
                    AlertDialog(
                        onDismissRequest = { showResetDialog = false },
                        title = { Text(stringResource(R.string.forgot_password)) },
                        text = {
                            Column {
                                Text(
                                    stringResource(R.string.reset_password_hint),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                                Spacer(Modifier.height(12.dp))
                                OutlinedTextField(
                                    value = resetEmail,
                                    onValueChange = { resetEmail = it },
                                    label = { Text(stringResource(R.string.email_hint)) },
                                    singleLine = true,
                                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email),
                                    modifier = Modifier.fillMaxWidth()
                                )
                            }
                        },
                        confirmButton = {
                            TextButton(
                                onClick = {
                                    viewModel.resetPassword(resetEmail)
                                    showResetDialog = false
                                },
                                enabled = resetEmail.isNotBlank()
                            ) { Text(stringResource(R.string.send)) }
                        },
                        dismissButton = {
                            TextButton(onClick = { showResetDialog = false }) {
                                Text(stringResource(R.string.cancel))
                            }
                        }
                    )
                }
            }

            TextButton(onClick = { isRegister = !isRegister; viewModel.clearError() }) {
                Text(
                    if (isRegister) stringResource(R.string.have_account)
                    else stringResource(R.string.no_account)
                )
            }
        }
    }
}