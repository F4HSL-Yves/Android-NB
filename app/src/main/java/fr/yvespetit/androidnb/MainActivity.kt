package fr.yvespetit.androidnb

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.heightIn
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.runtime.snapshotFlow
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeoutOrNull
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import fr.yvespetit.androidnb.nb.NbConnection
import fr.yvespetit.androidnb.nb.NbProtocol
import java.util.Locale

class MainActivity : ComponentActivity() {
    private val connection = NbConnection()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                // Edge to edge (Android 15+): keep the content clear of the status bar.
                Surface(modifier = Modifier.fillMaxSize()) {
                    Box(modifier = Modifier.safeDrawingPadding()) { MainScreen(connection) }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Listening session: keep the screen on (background playback comes with a service later).
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    override fun onStop() {
        // Like Android-DATV: no transmission from an app in the background
        // (Android also cuts the microphone there).
        connection.pttOff()
        super.onStop()
    }

    override fun onDestroy() {
        if (isFinishing) connection.disconnect()
        super.onDestroy()
    }
}

private data class FilterPreset(val label: String, val low: Int, val high: Int)

private val filters = listOf(
    FilterPreset("1,8 k", 400, 2200),
    FilterPreset("2,4 k", 300, 2700),
    FilterPreset("2,7 k", 200, 2900),
    FilterPreset("3,2 k", 100, 3300),
)
private val agcModes = listOf("Off" to 0, "Lent" to 2, "Moyen" to 3, "Rapide" to 4)
private val nrModes = listOf("Off" to 0, "NR1" to 1, "NR2" to 2)
private val bitrates = listOf(8000, 12000, 16000, 24000)
private val buffers = listOf(4, 6, 10, 15)  // packets of 20 ms
private val steps = listOf(10L, 100L, 1000L, 10000L)
private val spans = listOf(520_000 to "Bande", 100_000 to "100 k", 25_000 to "25 k", 6_000 to "6 k")
private fun formatSpan(hz: Int) =
    if (hz >= 10_000) "${hz / 1000} k" else String.format(Locale.FRANCE, "%.1f k", hz / 1000.0)

private const val MIN_SPAN_HZ = 3_000  // narrowest pinch zoom
private val waterfallRates = listOf(0, 4, 8, 12)  // lines per second, 0 = off

private val ritRange = -3000f..3000f

@Composable
private fun MainScreen(c: NbConnection) {
    // The board address is remembered between launches.
    val prefs = LocalContext.current.getSharedPreferences("nb", android.content.Context.MODE_PRIVATE)
    var host by rememberSaveable { mutableStateOf(prefs.getString("host", "192.168.1.125")!!) }
    var port by rememberSaveable { mutableStateOf(prefs.getString("port", "8073")!!) }
    var dial by rememberSaveable { mutableLongStateOf(10_489_900_000L) }
    var step by rememberSaveable { mutableLongStateOf(100L) }
    var rit by rememberSaveable { mutableIntStateOf(0) }
    var bufferPackets by rememberSaveable { mutableIntStateOf(6) }
    var span by rememberSaveable { mutableIntStateOf(spans[0].first) }
    var waterfallRate by rememberSaveable { mutableIntStateOf(8) }
    var viewCenter by rememberSaveable { mutableLongStateOf(0L) }  // 0: band centre
    var bandWidthPx by remember { mutableIntStateOf(0) }
    var tab by rememberSaveable { mutableIntStateOf(0) }

    // Android 17 blocks sockets to the local network (the board) until the
    // user grants "Local network".
    val context = LocalContext.current
    var permissionError by rememberSaveable { mutableStateOf<String?>(null) }
    fun localNetworkGranted() = Build.VERSION.SDK_INT < 37 ||
        context.checkSelfPermission(Manifest.permission.ACCESS_LOCAL_NETWORK) == PackageManager.PERMISSION_GRANTED
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) {
            permissionError = null
            prefs.edit().putString("host", host).putString("port", port).apply()
            c.connect(host, port.toIntOrNull() ?: 8073)
        } else {
            permissionError = "Android refuse l'accès au réseau local : autorise « Réseau local » pour QO-100 NB " +
                "dans les réglages de l'appli, sinon la carte reste injoignable en Wi-Fi."
        }
    }
    fun listen() {
        if (localNetworkGranted()) {
            permissionError = null
            prefs.edit().putString("host", host).putString("port", port).apply()
            c.connect(host, port.toIntOrNull() ?: 8073)
        } else {
            permissionLauncher.launch(Manifest.permission.ACCESS_LOCAL_NETWORK)
        }
    }

    val status = c.status
    val hello = c.hello
    val connected = c.link == NbConnection.Link.CONNECTED
    val minHz = hello?.minFreqHz ?: Qo100.DOWNLINK_LOW_HZ
    val maxHz = hello?.maxFreqHz ?: Qo100.DOWNLINK_HIGH_HZ

    fun tuneTo(hz: Long) {
        dial = hz.coerceIn(minHz, maxHz)
        c.tune(dial)
    }
    fun setRit(hz: Int) {
        rit = hz
        c.setRit(hz)
    }

    // Transmit side settings that live across both tabs.
    var arduino by rememberSaveable { mutableStateOf(prefs.getBoolean("arduino", true)) }
    c.arduinoPresent = arduino
    var muteTx by rememberSaveable { mutableStateOf(prefs.getBoolean("mute_rx_tx", false)) }
    LaunchedEffect(muteTx) { c.muteRxOnTx = muteTx }
    var tune by remember { mutableStateOf(false) }
    var calibrating by remember { mutableStateOf(false) }
    var calibMessage by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(c.txCal) { c.txCal?.let { prefs.edit().putInt("tx_cal", it.hz).apply() } }

    // On (re)connection, nbd keeps its own settings: push ours.
    LaunchedEffect(connected, hello) {
        if (connected && hello != null) {
            c.setPrefillPackets(bufferPackets)
            c.setNr(prefs.getInt("nr", 0))
            tuneTo(dial)
            c.setRit(rit)
            if (hello.tx) {
                c.setTxCal(prefs.getInt("tx_cal", 0))
                c.setMicGain(prefs.getInt("mic_gain_db", 0))
                c.setCompressor(prefs.getBoolean("comp", false))
                c.setTxEq(prefs.getInt("eq_bass", 0), prefs.getInt("eq_treble", 0))
            }
        }
    }

    // One-button TX calibration: tune carrier on the air, measure it, stop.
    LaunchedEffect(calibrating) {
        if (!calibrating) return@LaunchedEffect
        val before = c.txCal
        calibMessage = "Calibration : émission de la porteuse…"
        tune = true
        c.setTune(true)
        if (c.ptt == NbConnection.Ptt.OFF) c.pttOn()
        val result = withTimeoutOrNull(20_000) {
            snapshotFlow { c.ptt == NbConnection.Ptt.ON && c.tx?.tune == true }.first { it }
            delay(1500)
            calibMessage = "Calibration : mesure…"
            c.calibrateTx()
            snapshotFlow { c.txCal }.first { it != null && it !== before }
        }
        c.pttOff()
        tune = false
        c.setTune(false)
        calibMessage = if (result == null) "Calibration échouée : porteuse non reçue (voir le gain TX et la balise)." else null
        calibrating = false
    }

    // Band view follows the receive passband when it nears an edge.
    val filterLow = status?.filterLowHz ?: 200
    val filterHigh = status?.filterHighHz ?: 2800
    LaunchedEffect(dial, rit, span, filterLow, filterHigh, hello) {
        val centre = hello?.centerHz ?: return@LaunchedEffect
        val mid = dial + rit + (filterLow + filterHigh) / 2
        viewCenter = when {
            span >= spans[0].first -> centre
            viewCenter == 0L || kotlin.math.abs(mid - viewCenter) > span * 0.3 -> mid
            else -> viewCenter
        }
    }
    LaunchedEffect(connected, hello, span, viewCenter, waterfallRate, bandWidthPx) {
        delay(120)  // a pinch changes the span every frame: send once it settles
        // Bins follow the screen width (one per pixel, 1024 at most, to spare 4G data).
        if (connected && hello != null && hello.spectrumSource != "off" && (bandWidthPx > 0 || waterfallRate == 0)) {
            val bins = if (bandWidthPx > 0) bandWidthPx.coerceIn(128, 1024) else 768
            c.setSpectrum(viewCenter, span, bins, waterfallRate.toDouble())
        }
    }

    Column(modifier = Modifier.fillMaxSize()) {
        Box(modifier = Modifier.weight(1f).fillMaxWidth()) {
            if (tab == 0) {
                HomeTab(
                    c = c, dial = dial, step = step, rit = rit, span = span, viewCenter = viewCenter,
                    waterfallRate = waterfallRate, filterLow = filterLow, filterHigh = filterHigh,
                    permissionError = permissionError, muteTx = muteTx, onMuteTx = {
                        muteTx = it
                        prefs.edit().putBoolean("mute_rx_tx", it).apply()
                    },
                    onListen = { listen() }, onTune = { tuneTo(it) }, onStep = { step = it }, onRit = { setRit(it) },
                    onSpan = { span = it }, onBandWidth = { bandWidthPx = it },
                )
            } else {
                SettingsTab(
                    txTab = tab == 2,
                    c = c, prefs = prefs, host = host, port = port,
                    onHost = { host = it }, onPort = { port = it }, onListen = { listen() },
                    permissionError = permissionError,
                    waterfallRate = waterfallRate, onWaterfallRate = { waterfallRate = it },
                    bufferPackets = bufferPackets, onBuffer = {
                        bufferPackets = it
                        c.setPrefillPackets(it)
                    },
                    tune = tune, onTune = {
                        tune = it
                        c.setTune(it)
                    },
                    arduino = arduino, onArduino = {
                        arduino = it
                        prefs.edit().putBoolean("arduino", it).apply()
                    },
                    calibrating = calibrating, calibMessage = calibMessage,
                    onCalibrate = { calibrating = true },
                    canCalibrate = connected && c.mqttUp && Qo100.isTransmitAllowed(Qo100.uplinkFor(dial)),
                )
            }
        }
        NavigationBar(windowInsets = WindowInsets(0)) {
            NavigationBarItem(selected = tab == 0, onClick = { tab = 0 }, icon = { Text("📡") }, label = { Text("Accueil") })
            NavigationBarItem(selected = tab == 1, onClick = { tab = 1 }, icon = { Text("🎧") }, label = { Text("Réglages\nréception", style = MaterialTheme.typography.labelSmall, textAlign = TextAlign.Center, lineHeight = 13.sp) })
            if (hello?.tx == true || tab == 2) {
                NavigationBarItem(selected = tab == 2, onClick = { tab = 2 }, icon = { Text("🎙") }, label = { Text("Réglages\némission", style = MaterialTheme.typography.labelSmall, textAlign = TextAlign.Center, lineHeight = 13.sp) })
            }
        }
    }
}

@Composable
private fun HomeTab(
    c: NbConnection, dial: Long, step: Long, rit: Int, span: Int, viewCenter: Long, waterfallRate: Int,
    filterLow: Int, filterHigh: Int, permissionError: String?, muteTx: Boolean, onMuteTx: (Boolean) -> Unit,
    onListen: () -> Unit, onTune: (Long) -> Unit, onStep: (Long) -> Unit, onRit: (Int) -> Unit,
    onSpan: (Int) -> Unit, onBandWidth: (Int) -> Unit,
) {
    val status = c.status
    val hello = c.hello
    val connected = c.link == NbConnection.Link.CONNECTED
    var goTo by remember { mutableStateOf(false) }
    Column(
        modifier = Modifier.fillMaxSize().padding(horizontal = 12.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("QO-100 NB", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            BeaconBadge(status)
            LinkBadge(c.link)
        }
        if (c.link == NbConnection.Link.DISCONNECTED) {
            Button(onClick = onListen, modifier = Modifier.fillMaxWidth()) { Text("Écouter") }
        }
        val problem = when {
            permissionError != null -> permissionError
            c.link != NbConnection.Link.DISCONNECTED && !c.mqttUp -> "MQTT de la carte injoignable (port 1883)"
            c.link != NbConnection.Link.DISCONNECTED && c.boardMode == "datv" -> "Réception DATV en cours : la bande étroite reprendra à son arrêt."
            else -> c.lastError
        }
        problem?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall, maxLines = 2) }

        // Band: spectrum and waterfall; tap or drag to tune, pinch to zoom.
        if (hello != null && hello.spectrumSource != "off" && waterfallRate > 0) {
            BandView(
                waterfall = c.waterfall, version = c.waterfallVersion, dialHz = dial,
                filterLowHz = filterLow, filterHighHz = filterHigh, ritHz = rit, onTune = onTune,
                onZoom = { f -> onSpan(kotlin.math.round(span / f).toInt().coerceIn(MIN_SPAN_HZ, spans[0].first)) },
                viewLeftHz = (if (viewCenter != 0L) viewCenter else hello.centerHz) - span / 2,
                viewSpanHz = span,
                modifier = Modifier.weight(1f).heightIn(min = 120.dp).onSizeChanged { onBandWidth(it.width) },
            )
            ChipRow(if (spans.any { it.first == span }) "Vue" else "Vue ${formatSpan(span)}", spans, spans.firstOrNull { it.first == span }, { it.second }) { onSpan(it.first) }
        } else {
            Spacer(modifier = Modifier.weight(1f))
        }

        // Frequency: − / + by the step, tap the figure to type one.
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            OutlinedButton(onClick = { onTune(dial - step) }) { Text("−") }
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier.weight(1f).clickable { goTo = true },
            ) {
                Text(Qo100.formatMHz(dial), fontFamily = FontFamily.Monospace, fontSize = 28.sp, maxLines = 1)
                val up = Qo100.uplinkFor(dial)
                Text(
                    "TX " + Qo100.formatMHz(up) + if (Qo100.isTransmitAllowed(up)) "" else " (hors zone)",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            OutlinedButton(onClick = { onTune(dial + step) }) { Text("+") }
        }
        ChipRow("Pas", steps, step, { if (it >= 1000) "${it / 1000} kHz" else "$it Hz" }, onStep)

        // RIT: listen beside the dial, the uplink stays put.
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                String.format(Locale.FRANCE, "RIT %+d", rit), modifier = Modifier.width(72.dp),
                style = MaterialTheme.typography.bodyMedium,
                color = if (rit != 0) Color(0xFFFFD54F) else MaterialTheme.colorScheme.onSurface,
            )
            Slider(
                value = rit.toFloat(), valueRange = ritRange,
                onValueChange = { onRit((Math.round(it / 10f) * 10)) },
                modifier = Modifier.weight(1f),
            )
            TextButton(onClick = { onRit(0) }, enabled = rit != 0) { Text("0") }
        }
        SMeter(status?.signalDb)

        if (hello?.tx == true) TxControls(c, dial, connected, muteTx, onMuteTx)
    }
    if (goTo) GoToDialog(dial, onDismiss = { goTo = false }, onGo = {
        onTune(it)
        goTo = false
    })
}

@Composable
private fun GoToDialog(dial: Long, onDismiss: () -> Unit, onGo: (Long) -> Unit) {
    var text by remember {
        mutableStateOf(java.math.BigDecimal.valueOf(dial).movePointLeft(6).stripTrailingZeros().toPlainString().replace('.', ','))
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Aller à") },
        text = {
            OutlinedTextField(
                value = text, onValueChange = { text = it }, label = { Text("Fréquence (MHz)") }, singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            )
        },
        confirmButton = { TextButton(onClick = { Qo100.parseMHz(text)?.let(onGo) }) { Text("OK") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Annuler") } },
    )
}

/** PTT and RF gain: the transmit controls used during a QSO. */
@Composable
private fun TxControls(c: NbConnection, dial: Long, connected: Boolean, muteTx: Boolean, onMuteTx: (Boolean) -> Unit) {
    val context = LocalContext.current
    var gain by remember { mutableFloatStateOf(-30f) }
    var gainTouched by remember { mutableStateOf(false) }
    var micError by remember { mutableStateOf<String?>(null) }
    // Follow the board's gain until the user moves the slider.
    LaunchedEffect(c.txGainDb) { if (!gainTouched) c.txGainDb?.let { gain = it.toFloat() } }
    val allowed = Qo100.isTransmitAllowed(Qo100.uplinkFor(dial))
    val active = c.ptt != NbConnection.Ptt.OFF
    val micGranted = context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
    val micLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        micError = if (granted) null else "Micro refusé : autorise le micro pour QO-100 NB dans les réglages."
    }
    val line = when {
        micError != null -> micError
        !allowed -> "Fréquence hors de la zone d'émission"
        c.pttMessage.isNotEmpty() -> c.pttMessage
        else -> null
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        FilterChip(selected = muteTx, onClick = { onMuteTx(!muteTx) }, label = { Text("Muet en TX") })
        Text(line ?: "", style = MaterialTheme.typography.bodySmall, maxLines = 2, modifier = Modifier.weight(1f),
            color = if (c.ptt == NbConnection.Ptt.OFF) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface)
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(String.format(Locale.FRANCE, "Gain TX %.0f dB", gain), modifier = Modifier.width(104.dp),
            style = MaterialTheme.typography.bodySmall)
        Slider(
            value = gain, valueRange = -50f..0f, steps = 49,
            onValueChange = {
                gain = Math.round(it).toFloat()
                gainTouched = true
            },
            onValueChangeFinished = { c.setTxGain(gain.toInt()) },
            enabled = c.mqttUp && c.callsign != null,
        )
    }
    // PTT last: right above the tab bar, under the thumb.
    Button(
        onClick = {
            when {
                active -> c.pttOff()
                !micGranted -> micLauncher.launch(Manifest.permission.RECORD_AUDIO)
                else -> c.pttOn()
            }
        },
        enabled = active || (connected && c.mqttUp && allowed),
        colors = ButtonDefaults.buttonColors(
            containerColor = if (active) Color(0xFFC62828) else Color(0xFF2E7D32),
            contentColor = Color.White,
        ),
        modifier = Modifier.fillMaxWidth().height(56.dp),
    ) {
        Text(
            when (c.ptt) {
                NbConnection.Ptt.OFF -> "PTT"
                NbConnection.Ptt.STARTING -> "PTT… (annuler)"
                NbConnection.Ptt.ON -> if (c.tx?.tune == true) "PORTEUSE · couper" else "EN ÉMISSION · couper"
                NbConnection.Ptt.STOPPING -> "Arrêt…"
            },
            fontSize = 18.sp,
        )
    }
}

@Composable
private fun SettingsTab(
    txTab: Boolean,
    c: NbConnection, prefs: android.content.SharedPreferences,
    host: String, port: String, onHost: (String) -> Unit, onPort: (String) -> Unit, onListen: () -> Unit,
    permissionError: String?,
    waterfallRate: Int, onWaterfallRate: (Int) -> Unit, bufferPackets: Int, onBuffer: (Int) -> Unit,
    tune: Boolean, onTune: (Boolean) -> Unit,
    arduino: Boolean, onArduino: (Boolean) -> Unit,
    calibrating: Boolean, calibMessage: String?, onCalibrate: () -> Unit, canCalibrate: Boolean,
) {
    val status = c.status
    val hello = c.hello
    val connected = c.link == NbConnection.Link.CONNECTED
    val context = LocalContext.current
    Column(
        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
      if (!txTab) {
        Section("Connexion")
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = host, onValueChange = { onHost(it.trim()) }, label = { Text("Carte") },
                singleLine = true, modifier = Modifier.weight(1f),
            )
            OutlinedTextField(
                value = port, onValueChange = { onPort(it.filter(Char::isDigit)) }, label = { Text("Port") },
                singleLine = true, modifier = Modifier.width(96.dp),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            )
        }
        if (c.link == NbConnection.Link.DISCONNECTED) {
            Button(onClick = onListen, modifier = Modifier.fillMaxWidth()) { Text("Écouter") }
        } else {
            OutlinedButton(onClick = { c.disconnect() }, modifier = Modifier.fillMaxWidth()) { Text("Arrêter") }
        }
        permissionError?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
      }

        if (txTab && hello?.tx != true) Text("Cette carte n'émet pas (nbd lancé sans --tx).", style = MaterialTheme.typography.bodySmall)
        if (txTab && hello?.tx == true) {
            Section("Émission")
            var micGain by rememberSaveable { mutableStateOf(prefs.getInt("mic_gain_db", 0).toFloat()) }
            var eqBass by rememberSaveable { mutableStateOf(prefs.getInt("eq_bass", 0).toFloat()) }
            var eqTreble by rememberSaveable { mutableStateOf(prefs.getInt("eq_treble", 0).toFloat()) }
            var comp by rememberSaveable { mutableStateOf(prefs.getBoolean("comp", false)) }
            DbSlider("Micro", micGain, -10f..30f, connected) {
                micGain = it
                prefs.edit().putInt("mic_gain_db", it.toInt()).apply()
                c.setMicGain(it.toInt())
            }
            DbSlider("Graves", eqBass, -12f..12f, connected) {
                eqBass = it
                prefs.edit().putInt("eq_bass", it.toInt()).apply()
                c.setTxEq(it.toInt(), eqTreble.toInt())
            }
            DbSlider("Aigus", eqTreble, -12f..12f, connected) {
                eqTreble = it
                prefs.edit().putInt("eq_treble", it.toInt()).apply()
                c.setTxEq(eqBass.toInt(), it.toInt())
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(selected = comp, onClick = {
                    comp = !comp
                    prefs.edit().putBoolean("comp", comp).apply()
                    c.setCompressor(comp)
                }, label = { Text("Compresseur") })
                FilterChip(selected = tune, onClick = { onTune(!tune) }, label = { Text("Porteuse 1 kHz") })
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("PTT par l'Arduino", style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                Switch(checked = arduino, enabled = c.ptt == NbConnection.Ptt.OFF, onCheckedChange = onArduino)
            }

            Section("Correction TX")
            var calText by rememberSaveable { mutableStateOf(prefs.getInt("tx_cal", 0).toString()) }
            LaunchedEffect(c.txCal) { c.txCal?.let { calText = it.hz.toString() } }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = calText, onValueChange = { calText = it.filter { ch -> ch.isDigit() || ch == '-' } },
                    label = { Text("Correction (Hz)") }, singleLine = true, modifier = Modifier.weight(1f),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                Button(onClick = {
                    calText.toIntOrNull()?.coerceIn(-50000, 50000)?.let {
                        prefs.edit().putInt("tx_cal", it).apply()
                        c.setTxCal(it)
                    }
                }) { Text("OK") }
            }
            val micGranted = context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
            Button(
                onClick = onCalibrate, enabled = !calibrating && canCalibrate && micGranted,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(if (calibrating) "Calibration en cours…" else "Calibrer (émet la porteuse quelques secondes)") }
            calibMessage?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
            c.txCal?.let {
                Text(
                    String.format(Locale.FRANCE, "Dernière mesure : porteuse reçue à %+.0f Hz, correction %+d Hz", it.measuredHz, it.hz),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            val tx = c.tx
            val board = buildList {
                c.callsign?.let { add(it) }
                c.txMuted?.let { add(if (it) "RF coupée" else "RF active") }
                c.arduinoPtt?.let { add("Arduino PTT " + if (it) "on" else "off") }
                c.paTempC?.let { add(String.format(Locale.FRANCE, "PA %.1f °C", it)) }
            }
            if (board.isNotEmpty()) Text(board.joinToString(" • "), style = MaterialTheme.typography.bodySmall)
            if (tx != null && tx.state == "on") {
                Text(
                    String.format(
                        Locale.FRANCE, "Niveau micro %.0f dB • ALC %.1f dB • sortie %.1f dB • %.0f s",
                        tx.micDb, tx.alcDb, tx.outDb, tx.onS,
                    ) + if (tx.underruns > 0) " • trous ${tx.underruns}" else "",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

      if (!txTab) {
        Section("Réception")
        ChipRow("Filtre", filters, filters.firstOrNull { it.low == status?.filterLowHz && it.high == status?.filterHighHz }, { it.label }) {
            c.setFilter(it.low, it.high)
        }
        ChipRow("AGC", agcModes, agcModes.firstOrNull { it.second == status?.agc }, { it.first }) { c.setAgc(it.second) }
        ChipRow("Bruit", nrModes, nrModes.firstOrNull { it.second == status?.nr }, { it.first }) {
            prefs.edit().putInt("nr", it.second).apply()
            c.setNr(it.second)
        }
        ChipRow("Chute", waterfallRates, waterfallRate, { if (it == 0) "Off" else "$it/s" }, onWaterfallRate)
        ChipRow("Débit", bitrates, status?.bitrate, { "${it / 1000}k" }) { c.setBitrate(it) }
        ChipRow("Tampon", buffers, bufferPackets, { "${it * 20} ms" }, onBuffer)

        Section("État")
        BeaconPanel(status)
        val s = c.stats
        Text(
            String.format(
                Locale.FRANCE, "Audio %.1f kbit/s • tampon %d ms • perdus %d • jetés %d • coupures %d",
                s.kbps, s.bufferMs, s.lost, s.discarded, s.underruns,
            ) + if (s.waterfallKbps > 0) String.format(Locale.FRANCE, " • chute %.0f kbit/s", s.waterfallKbps) else "",
            style = MaterialTheme.typography.bodySmall,
        )
        hello?.let {
            Text(
                "nbd ${it.source} • chute ${it.spectrumSource} • ${Qo100.formatMHz(it.minFreqHz)} – ${Qo100.formatMHz(it.maxFreqHz)}" +
                    (status?.let { st -> " • ${st.clients} client(s)" } ?: ""),
                style = MaterialTheme.typography.bodySmall,
            )
        }
      }
    }
}

@Composable
private fun Section(title: String) {
    Text(title, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(top = 6.dp))
}

/** Slider in whole dB; onDone runs when the finger lifts. */
@Composable
private fun DbSlider(label: String, value: Float, range: ClosedFloatingPointRange<Float>, enabled: Boolean, onDone: (Float) -> Unit) {
    var v by remember(value) { mutableFloatStateOf(value) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(String.format(Locale.FRANCE, "%s %+.0f dB", label, v), modifier = Modifier.width(112.dp),
            style = MaterialTheme.typography.bodySmall)
        Slider(
            value = v, valueRange = range, steps = (range.endInclusive - range.start).toInt() - 1,
            onValueChange = { v = Math.round(it).toFloat() },
            onValueChangeFinished = { onDone(v) },
            enabled = enabled,
        )
    }
}

@Composable
private fun BeaconBadge(status: NbProtocol.Status?) {
    val (text, color) = when (status?.beacon) {
        NbProtocol.BeaconState.LOCKED -> "balise ✓" to Color(0xFF2E7D32)
        NbProtocol.BeaconState.HOLDOVER -> "balise ~" to Color(0xFFF9A825)
        NbProtocol.BeaconState.SEARCHING -> "balise ?" to Color(0xFFC62828)
        else -> return
    }
    Text(
        text, color = Color.White, style = MaterialTheme.typography.labelMedium,
        modifier = Modifier.clip(RoundedCornerShape(8.dp)).background(color).padding(horizontal = 8.dp, vertical = 4.dp),
    )
}

@Composable
private fun LinkBadge(link: NbConnection.Link) {
    val (text, color) = when (link) {
        NbConnection.Link.CONNECTED -> "connecté" to Color(0xFF2E7D32)
        NbConnection.Link.CONNECTING -> "connexion…" to Color(0xFFF9A825)
        NbConnection.Link.DISCONNECTED -> "arrêté" to Color(0xFF616161)
    }
    Text(
        text, color = Color.White, style = MaterialTheme.typography.labelMedium,
        modifier = Modifier.clip(RoundedCornerShape(8.dp)).background(color).padding(horizontal = 8.dp, vertical = 4.dp),
    )
}

@Composable
private fun BeaconPanel(status: NbProtocol.Status?) {
    val (label, color) = when (status?.beacon) {
        NbProtocol.BeaconState.LOCKED -> "Balise verrouillée" to Color(0xFF2E7D32)
        NbProtocol.BeaconState.HOLDOVER -> "Balise perdue, correction maintenue" to Color(0xFFF9A825)
        NbProtocol.BeaconState.SEARCHING -> "Recherche de la balise" to Color(0xFFC62828)
        else -> "Balise : pas d'information" to Color(0xFF616161)
    }
    Column(
        modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp)).background(color.copy(alpha = 0.25f)).padding(12.dp),
    ) {
        Text(label, style = MaterialTheme.typography.titleSmall)
        if (status != null) {
            Text(
                String.format(
                    Locale.FRANCE, "Correction LNB %+.1f Hz • dérive %+.2f Hz/s • SNR %.0f dB",
                    status.correctionHz, status.driftHzPerS, status.snrDb,
                ),
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
private fun SMeter(db: Double?) {
    // WDSP S-meter in dB: S9 = -73 dBm on HF conventions; here only relative, 60 dB span.
    val fraction = if (db == null) 0f else ((db + 120.0) / 60.0).toFloat().coerceIn(0f, 1f)
    Column {
        Text(if (db == null) "Signal —" else String.format(Locale.FRANCE, "Signal %.1f dB", db), style = MaterialTheme.typography.bodySmall)
        Box(modifier = Modifier.fillMaxWidth().height(10.dp).clip(RoundedCornerShape(5.dp)).background(Color(0xFF303030))) {
            Box(modifier = Modifier.fillMaxWidth(fraction).fillMaxHeight().background(Color(0xFF42A5F5)))
        }
    }
}

@Composable
private fun <T> ChipRow(title: String, items: List<T>, selected: T?, label: (T) -> String, onPick: (T) -> Unit) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        modifier = Modifier.horizontalScroll(rememberScrollState()),
    ) {
        Text(title, modifier = Modifier.width(64.dp), style = MaterialTheme.typography.bodyMedium)
        items.forEach { item ->
            FilterChip(selected = item == selected, onClick = { onPick(item) }, label = { Text(label(item)) })
        }
    }
}
