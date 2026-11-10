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
private val waterfallRates = listOf(0, 4, 8, 12)  // lines per second, 0 = off

@Composable
private fun MainScreen(c: NbConnection) {
    // The board address is remembered between launches.
    val prefs = LocalContext.current.getSharedPreferences("nb", android.content.Context.MODE_PRIVATE)
    var host by rememberSaveable { mutableStateOf(prefs.getString("host", "192.168.1.125")!!) }
    var port by rememberSaveable { mutableStateOf(prefs.getString("port", "8073")!!) }
    var dialText by rememberSaveable { mutableStateOf("10489,900") }
    var dial by rememberSaveable { mutableLongStateOf(10_489_900_000L) }
    var step by rememberSaveable { mutableLongStateOf(100L) }
    var bufferPackets by rememberSaveable { mutableIntStateOf(6) }
    var volume by rememberSaveable { mutableFloatStateOf(1f) }
    var span by rememberSaveable { mutableIntStateOf(spans[0].first) }
    var waterfallRate by rememberSaveable { mutableIntStateOf(8) }
    var viewCenter by rememberSaveable { mutableLongStateOf(0L) }  // 0: band centre
    var bandWidthPx by remember { mutableIntStateOf(0) }

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
    LaunchedEffect(connected) { if (connected) c.setNr(prefs.getInt("nr", 0)) }
    val minHz = hello?.minFreqHz ?: Qo100.DOWNLINK_LOW_HZ
    val maxHz = hello?.maxFreqHz ?: Qo100.DOWNLINK_HIGH_HZ

    fun tuneTo(hz: Long) {
        dial = hz.coerceIn(minHz, maxHz)
        dialText = java.math.BigDecimal.valueOf(dial).movePointLeft(6).stripTrailingZeros().toPlainString().replace('.', ',')
        c.tune(dial)
    }

    // On (re)connection, nbd keeps its own frequency: push ours.
    LaunchedEffect(connected, hello) {
        if (connected && hello != null) {
            c.setPrefillPackets(bufferPackets)
            tuneTo(dial)
        }
    }

    Column(
        modifier = Modifier.padding(16.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("QO-100 NB", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.weight(1f))
            LinkBadge(c.link)
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = host, onValueChange = { host = it.trim() }, label = { Text("Carte") },
                singleLine = true, modifier = Modifier.weight(1f),
            )
            OutlinedTextField(
                value = port, onValueChange = { port = it.filter(Char::isDigit) }, label = { Text("Port") },
                singleLine = true, modifier = Modifier.width(96.dp),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            )
        }
        if (c.link == NbConnection.Link.DISCONNECTED) {
            Button(onClick = { listen() }, modifier = Modifier.fillMaxWidth()) { Text("Écouter") }
        } else {
            OutlinedButton(onClick = { c.disconnect() }, modifier = Modifier.fillMaxWidth()) { Text("Arrêter") }
        }
        // Board mode: NB (nbd, this app) or DATV (zynqrx, started from the Dashboard)
        if (c.link != NbConnection.Link.DISCONNECTED) {
            when {
                !c.mqttUp -> Text(
                    "MQTT de la carte injoignable (port 1883) : la carte ne peut pas être passée en NB",
                    color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall,
                )
                c.boardMode == "datv" -> Text(
                    "Réception DATV en cours (Webtioune) : la bande étroite reprendra à son arrêt.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
        permissionError?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
        c.lastError?.takeIf { c.boardMode != "datv" }?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }

        // Band: spectrum and waterfall, tap or drag to tune
        val filterLow = status?.filterLowHz ?: 200
        val filterHigh = status?.filterHighHz ?: 2800
        // Zoomed views follow the dial when the passband nears an edge.
        LaunchedEffect(dial, span, filterLow, filterHigh, hello) {
            val centre = hello?.centerHz ?: return@LaunchedEffect
            val mid = dial + (filterLow + filterHigh) / 2
            viewCenter = when {
                span >= spans[0].first -> centre
                viewCenter == 0L || kotlin.math.abs(mid - viewCenter) > span * 0.3 -> mid
                else -> viewCenter
            }
        }
        LaunchedEffect(connected, hello, span, viewCenter, waterfallRate, bandWidthPx) {
            // Bins follow the screen width (one per pixel, 1024 at most, to spare 4G data).
            if (connected && hello != null && hello.spectrumSource != "off" && (bandWidthPx > 0 || waterfallRate == 0)) {
                val bins = if (bandWidthPx > 0) bandWidthPx.coerceIn(128, 1024) else 768
                c.setSpectrum(viewCenter, span, bins, waterfallRate.toDouble())
            }
        }
        if (hello != null && hello.spectrumSource != "off" && waterfallRate > 0) {
            BandView(
                waterfall = c.waterfall, version = c.waterfallVersion, dialHz = dial,
                filterLowHz = filterLow, filterHighHz = filterHigh, onTune = { tuneTo(it) },
                modifier = Modifier.onSizeChanged { bandWidthPx = it.width },
            )
        }
        ChipRow("Vue", spans, spans.firstOrNull { it.first == span }, { it.second }) { span = it.first }

        // Frequency
        Text(
            Qo100.formatMHz(dial),
            fontFamily = FontFamily.Monospace, fontSize = 34.sp,
            modifier = Modifier.align(Alignment.CenterHorizontally),
        )
        Text(
            "Émission " + Qo100.formatMHz(Qo100.uplinkFor(dial)) +
                if (Qo100.isTransmitAllowed(Qo100.uplinkFor(dial))) "" else "  (hors zone d'émission)",
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.align(Alignment.CenterHorizontally),
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            OutlinedButton(onClick = { tuneTo(dial - step) }, modifier = Modifier.weight(1f)) { Text("−") }
            OutlinedButton(onClick = { tuneTo(dial + step) }, modifier = Modifier.weight(1f)) { Text("+") }
        }
        ChipRow("Pas", steps, step, { if (it >= 1000) "${it / 1000} kHz" else "$it Hz" }) { step = it }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = dialText, onValueChange = { dialText = it }, label = { Text("Aller à (MHz)") },
                singleLine = true, modifier = Modifier.weight(1f),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            )
            Button(onClick = { Qo100.parseMHz(dialText)?.let { tuneTo(it) } }) { Text("OK") }
        }

        // Transmit (nbd started with --tx)
        if (hello?.tx == true) TxPanel(c, dial, connected)

        // Beacon lock and signal
        BeaconPanel(status)
        SMeter(status?.signalDb)

        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Volume", modifier = Modifier.width(72.dp))
            Slider(value = volume, onValueChange = {
                volume = it
                c.setVolume(it)
            })
        }

        var advanced by rememberSaveable { mutableStateOf(false) }
        TextButton(onClick = { advanced = !advanced }) {
            Text(if (advanced) "Réglages avancés ▾" else "Réglages avancés ▸")
        }
        if (advanced) {
            ChipRow("Chute", waterfallRates, waterfallRate, { if (it == 0) "Off" else "$it/s" }) { waterfallRate = it }
            ChipRow("Filtre", filters, filters.firstOrNull { it.low == status?.filterLowHz && it.high == status?.filterHighHz }, { it.label }) {
                c.setFilter(it.low, it.high)
            }
            ChipRow("AGC", agcModes, agcModes.firstOrNull { it.second == status?.agc }, { it.first }) { c.setAgc(it.second) }
            ChipRow("Bruit", nrModes, nrModes.firstOrNull { it.second == status?.nr }, { it.first }) {
                prefs.edit().putInt("nr", it.second).apply()
                c.setNr(it.second)
            }
            ChipRow("Débit", bitrates, status?.bitrate, { "${it / 1000}k" }) { c.setBitrate(it) }
            ChipRow("Tampon", buffers, bufferPackets, { "${it * 20} ms" }) {
                bufferPackets = it
                c.setPrefillPackets(it)
            }
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

@Composable
private fun TxPanel(c: NbConnection, dial: Long, connected: Boolean) {
    val context = LocalContext.current
    val prefs = context.getSharedPreferences("nb", android.content.Context.MODE_PRIVATE)
    var arduino by rememberSaveable { mutableStateOf(prefs.getBoolean("arduino", true)) }
    var calText by rememberSaveable { mutableStateOf(prefs.getInt("tx_cal", 0).toString()) }
    var gain by remember { mutableFloatStateOf(-30f) }
    var gainTouched by remember { mutableStateOf(false) }
    var tune by remember { mutableStateOf(false) }
    var micError by remember { mutableStateOf<String?>(null) }
    c.arduinoPresent = arduino
    // Follow the board's gain until the user moves the slider.
    LaunchedEffect(c.txGainDb) { if (!gainTouched) c.txGainDb?.let { gain = it.toFloat() } }
    // nbd forgets the correction when it restarts: send it on each connection.
    var muteTx by rememberSaveable { mutableStateOf(prefs.getBoolean("mute_rx_tx", false)) }
    LaunchedEffect(muteTx) { c.muteRxOnTx = muteTx }
    LaunchedEffect(c.txCal) {
        c.txCal?.let {
            prefs.edit().putInt("tx_cal", it.hz).apply()
            calText = it.hz.toString()
        }
    }
    var comp by rememberSaveable { mutableStateOf(prefs.getBoolean("comp", false)) }
    var eqBass by rememberSaveable { mutableStateOf(prefs.getInt("eq_bass", 0).toFloat()) }
    var eqTreble by rememberSaveable { mutableStateOf(prefs.getInt("eq_treble", 0).toFloat()) }
    var micGain by rememberSaveable { mutableStateOf(prefs.getInt("mic_gain_db", 0).toFloat()) }
    LaunchedEffect(connected) {
        if (connected) {
            c.setTxCal(prefs.getInt("tx_cal", 0))
            c.setMicGain(prefs.getInt("mic_gain_db", 0))
            c.setCompressor(prefs.getBoolean("comp", false))
            c.setTxEq(prefs.getInt("eq_bass", 0), prefs.getInt("eq_treble", 0))
        }
    }

    val uplink = Qo100.uplinkFor(dial)
    val allowed = Qo100.isTransmitAllowed(uplink)
    val active = c.ptt != NbConnection.Ptt.OFF
    val micGranted = context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
    val micLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        micError = if (granted) null else "Micro refusé : autorise le micro pour QO-100 NB dans les réglages."
    }

    Column(
        modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp))
            .background(if (c.ptt == NbConnection.Ptt.ON) Color(0x55C62828) else Color(0xFF1E1E1E)).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Émission", style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f))
            Text(Qo100.formatMHz(uplink), fontFamily = FontFamily.Monospace)
        }
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
                    NbConnection.Ptt.ON -> "EN ÉMISSION · couper"
                    NbConnection.Ptt.STOPPING -> "Arrêt…"
                },
                fontSize = 18.sp,
            )
        }
        val line = when {
            !allowed -> "Fréquence hors de la zone d'émission (balises et bords du transpondeur)"
            c.pttMessage.isNotEmpty() -> c.pttMessage
            else -> "Prêt"
        }
        Text(line, style = MaterialTheme.typography.bodySmall,
            color = if (c.ptt == NbConnection.Ptt.OFF && c.pttMessage.isNotEmpty()) MaterialTheme.colorScheme.error
            else MaterialTheme.colorScheme.onSurface)
        micError?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }

        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(String.format(Locale.FRANCE, "Gain %.0f dB", gain), modifier = Modifier.width(96.dp),
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
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(String.format(Locale.FRANCE, "Micro %+.0f dB", micGain), modifier = Modifier.width(96.dp),
                style = MaterialTheme.typography.bodySmall)
            Slider(
                value = micGain, valueRange = -10f..30f, steps = 39,
                onValueChange = { micGain = Math.round(it).toFloat() },
                onValueChangeFinished = {
                    prefs.edit().putInt("mic_gain_db", micGain.toInt()).apply()
                    c.setMicGain(micGain.toInt())
                },
                enabled = connected,
            )
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(String.format(Locale.FRANCE, "Graves %+.0f dB", eqBass), modifier = Modifier.width(96.dp),
                style = MaterialTheme.typography.bodySmall)
            Slider(
                value = eqBass, valueRange = -12f..12f, steps = 23,
                onValueChange = { eqBass = Math.round(it).toFloat() },
                onValueChangeFinished = {
                    prefs.edit().putInt("eq_bass", eqBass.toInt()).apply()
                    c.setTxEq(eqBass.toInt(), eqTreble.toInt())
                },
                enabled = connected,
            )
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(String.format(Locale.FRANCE, "Aigus %+.0f dB", eqTreble), modifier = Modifier.width(96.dp),
                style = MaterialTheme.typography.bodySmall)
            Slider(
                value = eqTreble, valueRange = -12f..12f, steps = 23,
                onValueChange = { eqTreble = Math.round(it).toFloat() },
                onValueChangeFinished = {
                    prefs.edit().putInt("eq_treble", eqTreble.toInt()).apply()
                    c.setTxEq(eqBass.toInt(), eqTreble.toInt())
                },
                enabled = connected,
            )
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(selected = tune, onClick = {
                tune = !tune
                c.setTune(tune)
            }, label = { Text("Porteuse 1 kHz") })
            FilterChip(selected = comp, onClick = {
                comp = !comp
                prefs.edit().putBoolean("comp", comp).apply()
                c.setCompressor(comp)
            }, label = { Text("Compresseur") })
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(selected = muteTx, onClick = {
                muteTx = !muteTx
                prefs.edit().putBoolean("mute_rx_tx", muteTx).apply()
            }, label = { Text("Muet en TX") })
            Text("Arduino", style = MaterialTheme.typography.bodySmall)
            Switch(checked = arduino, enabled = !active, onCheckedChange = {
                arduino = it
                prefs.edit().putBoolean("arduino", it).apply()
            })
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = calText, onValueChange = { calText = it.filter { ch -> ch.isDigit() || ch == '-' } },
                label = { Text("Correction TX (Hz)") }, singleLine = true, modifier = Modifier.weight(1f),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            )
            Button(onClick = {
                calText.toIntOrNull()?.coerceIn(-50000, 50000)?.let {
                    prefs.edit().putInt("tx_cal", it).apply()
                    c.setTxCal(it)
                }
            }) { Text("OK") }
            Button(
                onClick = { c.calibrateTx() },
                enabled = c.ptt == NbConnection.Ptt.ON && c.tx?.tune == true,
            ) { Text("Calibrer") }
        }
        c.txCal?.let {
            Text(
                String.format(Locale.FRANCE, "Calibré : porteuse reçue à %+.0f Hz, correction %+d Hz", it.measuredHz, it.hz),
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
        Text(
            "Full duplex : avec le haut-parleur, ta propre voix revient du satellite dans le micro. Casque conseillé.",
            style = MaterialTheme.typography.bodySmall, color = Color(0xFF9E9E9E),
        )
    }
}
