package com.securitycam.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.google.firebase.auth.FirebaseAuth
import com.securitycam.repository.FirestoreDeviceRepository
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await

data class AuthUiState(
    val isLoggedIn: Boolean = false,
    val isLoading: Boolean = false,
    val error: String? = null,
    val successMessage: String? = null,
    val userEmail: String = ""
)

class AuthViewModel : ViewModel() {

    private val auth = FirebaseAuth.getInstance()
    private val deviceRepo = FirestoreDeviceRepository()

    private val _uiState = MutableStateFlow(AuthUiState())
    val uiState: StateFlow<AuthUiState> = _uiState.asStateFlow()

    init {
        val user = auth.currentUser
        if (user != null) {
            _uiState.value = AuthUiState(isLoggedIn = true, userEmail = user.email ?: "")
        }
    }

    fun login(email: String, password: String) {
        val trimmedEmail = email.trim()
        if (trimmedEmail.isBlank() || password.isBlank()) {
            _uiState.value = _uiState.value.copy(error = "Compila tutti i campi")
            return
        }
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isLoading = true, error = null, successMessage = null)
            try {
                val result = auth.signInWithEmailAndPassword(trimmedEmail, password).await()
                deviceRepo.ensureUserDocument()
                _uiState.value = AuthUiState(isLoggedIn = true, userEmail = result.user?.email ?: "")
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(isLoading = false, error = parseError(e))
            }
        }
    }

    fun register(email: String, password: String) {
        val trimmedEmail = email.trim()
        if (trimmedEmail.isBlank() || password.isBlank()) {
            _uiState.value = _uiState.value.copy(error = "Compila tutti i campi")
            return
        }
        if (password.length < 6) {
            _uiState.value = _uiState.value.copy(error = "Password: minimo 6 caratteri")
            return
        }
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isLoading = true, error = null, successMessage = null)
            try {
                val result = auth.createUserWithEmailAndPassword(trimmedEmail, password).await()
                deviceRepo.ensureUserDocument()
                _uiState.value = AuthUiState(isLoggedIn = true, userEmail = result.user?.email ?: "")
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(isLoading = false, error = parseError(e))
            }
        }
    }

    fun resetPassword(email: String) {
        val trimmedEmail = email.trim()
        if (trimmedEmail.isBlank()) {
            _uiState.value = _uiState.value.copy(error = "Inserisci l'email")
            return
        }
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isLoading = true, error = null, successMessage = null)
            try {
                auth.sendPasswordResetEmail(trimmedEmail).await()
                _uiState.value = _uiState.value.copy(
                    isLoading = false,
                    successMessage = "Email di reset inviata a $trimmedEmail"
                )
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(isLoading = false, error = parseError(e))
            }
        }
    }

    fun changePassword(newPassword: String) {
        if (newPassword.length < 6) {
            _uiState.value = _uiState.value.copy(error = "Password: minimo 6 caratteri")
            return
        }
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isLoading = true, error = null, successMessage = null)
            try {
                auth.currentUser?.updatePassword(newPassword)?.await()
                _uiState.value = _uiState.value.copy(
                    isLoading = false,
                    successMessage = "Password aggiornata"
                )
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(
                    isLoading = false,
                    error = if (e.message?.contains("CREDENTIAL_TOO_OLD") == true
                        || e.message?.contains("requires-recent-login") == true)
                        "Devi rifare il login prima di cambiare password"
                    else parseError(e)
                )
            }
        }
    }

    fun logout() {
        auth.signOut()
        _uiState.value = AuthUiState()
    }

    fun clearError() { _uiState.value = _uiState.value.copy(error = null, successMessage = null) }

    fun deleteAccount(password: String) {
        val user = auth.currentUser
        if (user == null || user.email == null) {
            _uiState.value = _uiState.value.copy(error = "Utente non autenticato")
            return
        }
        if (password.isBlank()) {
            _uiState.value = _uiState.value.copy(error = "Inserisci la password")
            return
        }
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isLoading = true, error = null, successMessage = null)
            try {
                // Step 1: re-autenticazione (Firebase la richiede per operazioni sensibili)
                val credential = com.google.firebase.auth.EmailAuthProvider
                    .getCredential(user.email!!, password)
                user.reauthenticate(credential).await()

                // Step 2: cancello il documento utente da Firestore
                try {
                    deviceRepo.ensureUserDocument() // assicuro che esista prima di cancellare
                    com.google.firebase.firestore.FirebaseFirestore.getInstance()
                        .document("users/${user.uid}")
                        .delete()
                        .await()
                } catch (_: Exception) {
                    // best-effort: se fallisce, l'account viene comunque cancellato
                }

                // Step 3: cancello l'account Firebase Auth
                user.delete().await()

                // Step 4: reset stato
                _uiState.value = AuthUiState()
            } catch (e: Exception) {
                _uiState.value = _uiState.value.copy(
                    isLoading = false,
                    error = when {
                        e.message?.contains("INVALID_LOGIN_CREDENTIALS") == true -> "Password errata"
                        e.message?.contains("WRONG_PASSWORD") == true -> "Password errata"
                        e.message?.contains("requires-recent-login") == true -> "Sessione scaduta, rifai il login"
                        else -> parseError(e)
                    }
                )
            }
        }
    }

    private fun parseError(e: Exception): String = when {
        e.message?.contains("INVALID_LOGIN_CREDENTIALS") == true -> "Email o password errati"
        e.message?.contains("credential") == true -> "Email o password errati"  // ← aggiungi questo
        e.message?.contains("EMAIL_EXISTS") == true -> "Email già registrata"
        e.message?.contains("WEAK_PASSWORD") == true -> "Password troppo debole"
        e.message?.contains("INVALID_EMAIL") == true -> "Email non valida"
        e.message?.contains("badly formatted") == true -> "Email non valida"
        e.message?.contains("network") == true -> "Errore di rete"
        else -> e.message ?: "Errore sconosciuto"
    }
}