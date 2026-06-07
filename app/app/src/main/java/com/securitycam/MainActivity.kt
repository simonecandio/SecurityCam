package com.securitycam

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.material3.windowsizeclass.ExperimentalMaterial3WindowSizeClassApi
import androidx.compose.material3.windowsizeclass.WindowWidthSizeClass
import androidx.compose.material3.windowsizeclass.calculateWindowSizeClass
import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import androidx.work.*
import com.securitycam.ui.screens.*
import com.securitycam.ui.theme.SecurityCamTheme
import com.securitycam.util.NotificationHelper
import com.securitycam.util.PreferencesManager
import com.securitycam.viewmodel.*
import java.util.concurrent.TimeUnit

class MainActivity : ComponentActivity() {

    private val notifPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* non critico se rifiutato */ }

    @OptIn(ExperimentalMaterial3WindowSizeClassApi::class)
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        NotificationHelper.createChannel(this)

        val workRequest = PeriodicWorkRequestBuilder<com.securitycam.util.EventCheckWorker>(
            15, TimeUnit.MINUTES
        )
            .setConstraints(
                Constraints.Builder()
                    .setRequiredNetworkType(NetworkType.CONNECTED)
                    .build()
            )
            .build()

        WorkManager.getInstance(this).enqueueUniquePeriodicWork(
            "event_check",
            ExistingPeriodicWorkPolicy.KEEP,
            workRequest
        )

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED
            ) {
                notifPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }

        // Salvo i dati dalla notifica prima di setContent
        val notifNavigateTo = intent?.getStringExtra("navigate_to")
        val notifDeviceId = intent?.getStringExtra("device_id") ?: ""
        val notifDeviceName = intent?.getStringExtra("device_name") ?: ""
        val notifOwnerUid = intent?.getStringExtra("owner_uid") ?: ""

        setContent {
            SecurityCamTheme {
                val windowSizeClass = calculateWindowSizeClass(this)
                val isTablet = windowSizeClass.widthSizeClass != WindowWidthSizeClass.Compact
                SecurityCamApp(
                    isTablet = isTablet,
                    notifNavigateTo = notifNavigateTo,
                    notifDeviceId = notifDeviceId,
                    notifDeviceName = notifDeviceName,
                    notifOwnerUid = notifOwnerUid
                )
            }
        }
    }
}

@Composable
fun SecurityCamApp(
    isTablet: Boolean = false,
    notifNavigateTo: String? = null,
    notifDeviceId: String = "",
    notifDeviceName: String = "",
    notifOwnerUid: String = ""
) {
    val navController = rememberNavController()
    // AuthViewModel vive a livello app: condiviso tra login, profilo, logout
    val authViewModel: AuthViewModel = viewModel()
    val authState by authViewModel.uiState.collectAsState()
    // Flag per evitare che le navigazioni automatiche partano più di una volta

    var hasAutoNavigated by remember { mutableStateOf(false) }
    var hasNotifNavigated by remember { mutableStateOf(false) }

    // Se l'app è aperta da notifica e l'utente è loggato, vado agli eventi del device
    LaunchedEffect(notifNavigateTo) {
        if (notifNavigateTo == "events" && !hasNotifNavigated && authState.isLoggedIn) {
            hasNotifNavigated = true
            if (notifDeviceId.isNotEmpty()) {
                val ownerParam = notifOwnerUid.ifEmpty { "_" }
                navController.navigate("events/$notifDeviceId/$notifDeviceName/$ownerParam")
            }
        }
    }

    NavHost(
        navController = navController,
        startDestination = if (authState.isLoggedIn) "devices" else "login"
    ) {
        composable("login") {
            LoginScreen(viewModel = authViewModel)
            LaunchedEffect(authState.isLoggedIn) {
                if (authState.isLoggedIn) {
                    navController.navigate("devices") {
                        popUpTo("login") { inclusive = true }
                    }
                }
            }
        }

        composable("devices") {
            val devicesViewModel: DevicesViewModel = viewModel()
            val context = LocalContext.current
            val prefsManager = remember { PreferencesManager(context) }

            LaunchedEffect(Unit) {
                if (!hasAutoNavigated && !hasNotifNavigated) {
                    hasAutoNavigated = true
                    val lastId = prefsManager.getLastDeviceId()
                    val lastName = prefsManager.getLastDeviceName()
                    val lastIp = prefsManager.getLastDeviceIp()
                    val lastOwner = prefsManager.getLastDeviceOwnerUid()
                    if (lastId.isNotEmpty()) {
                        val ownerParam = lastOwner.ifEmpty { "_" }
                        navController.navigate("dashboard/$lastId/$lastName/$lastIp/$ownerParam")
                    }
                }
            }

            DevicesScreen(
                viewModel = devicesViewModel,
                // Salvo il device come "ultimo aperto" per la prossima volta
                onDeviceClick = { device ->
                    prefsManager.saveLastDevice(device.id, device.name, device.ip, device.ownerUid)
                    val ownerParam = device.ownerUid.ifEmpty { "_" }
                    navController.navigate("dashboard/${device.id}/${device.name}/${device.ip}/$ownerParam")
                },
                onLogout = {
                    authViewModel.logout()
                    navController.navigate("login") {
                        popUpTo("devices") { inclusive = true }
                    }
                },
                onProfile = {
                    navController.navigate("profile")
                }
            )
        }

        composable("profile") {
            ProfileScreen(
                viewModel = authViewModel,
                onBack = { navController.popBackStack() },
                onLogout = {
                    authViewModel.logout()
                    navController.navigate("login") {
                        // popUpTo(0): ripulisco completamente il back stack
                        popUpTo(0) { inclusive = true }
                    }
                }
            )
        }

        composable(
            "dashboard/{deviceId}/{deviceName}/{deviceIp}/{ownerUid}",
            arguments = listOf(
                navArgument("deviceId") { type = NavType.StringType },
                navArgument("deviceName") { type = NavType.StringType },
                navArgument("deviceIp") { type = NavType.StringType },
                navArgument("ownerUid") { type = NavType.StringType; defaultValue = "_" }
            )
        ) { backStackEntry ->
            val deviceId = backStackEntry.arguments?.getString("deviceId") ?: ""
            val deviceName = backStackEntry.arguments?.getString("deviceName") ?: ""
            val deviceIp = backStackEntry.arguments?.getString("deviceIp") ?: ""
            val ownerUid = (backStackEntry.arguments?.getString("ownerUid") ?: "_").let {
                if (it == "_") "" else it
            }
            val dashboardViewModel: DashboardViewModel = viewModel()

            DashboardScreen(
                viewModel = dashboardViewModel,
                deviceName = deviceName,
                deviceIp = deviceIp,
                deviceId = deviceId,
                ownerUid = ownerUid,
                isTablet = isTablet,
                onStream = { navController.navigate("stream/$deviceIp") },
                onEvents = {
                    val ownerParam = ownerUid.ifEmpty { "_" }
                    navController.navigate("events/$deviceId/$deviceName/$ownerParam")
                },
                onConfig = { navController.navigate("config/$deviceIp/$deviceId") },
                onBack = { navController.popBackStack() }
            )
        }

        composable(
            "stream/{deviceIp}",
            arguments = listOf(navArgument("deviceIp") { type = NavType.StringType })
        ) { backStackEntry ->
            val deviceIp = backStackEntry.arguments?.getString("deviceIp") ?: ""
            StreamScreen(deviceIp = deviceIp, onBack = { navController.popBackStack() })
        }

        composable(
            "events/{deviceId}/{deviceName}/{ownerUid}",
            arguments = listOf(
                navArgument("deviceId") { type = NavType.StringType },
                navArgument("deviceName") { type = NavType.StringType },
                navArgument("ownerUid") { type = NavType.StringType; defaultValue = "_" }
            )
        ) { backStackEntry ->
            val deviceId = backStackEntry.arguments?.getString("deviceId") ?: ""
            val deviceName = backStackEntry.arguments?.getString("deviceName") ?: ""
            val ownerUid = (backStackEntry.arguments?.getString("ownerUid") ?: "_").let {
                if (it == "_") "" else it
            }
            val eventsViewModel: EventsViewModel = viewModel()

            EventsScreen(
                viewModel = eventsViewModel,
                deviceId = deviceId,
                deviceName = deviceName,
                ownerUid = ownerUid,
                onBack = { navController.popBackStack() }
            )
        }

        composable(
            "config/{deviceIp}/{deviceId}",
            arguments = listOf(
                navArgument("deviceIp") { type = NavType.StringType },
                navArgument("deviceId") { type = NavType.StringType }
            )
        ) { backStackEntry ->
            val deviceIp = backStackEntry.arguments?.getString("deviceIp") ?: ""
            val deviceId = backStackEntry.arguments?.getString("deviceId") ?: ""
            val configViewModel: ConfigViewModel = viewModel()

            ConfigScreen(
                viewModel = configViewModel,
                deviceIp = deviceIp,
                deviceId = deviceId,
                onBack = { navController.popBackStack() }
            )
        }
    }
}