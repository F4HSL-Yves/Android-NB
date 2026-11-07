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
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
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
                Surface(modifier = Modifier.fillMaxSize()) { MainScreen(connection) }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Listening session: keep the screen on (background playback comes with a service later).
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
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
private val bitrates = listOf(8000, 12000, 16000, 24000)
private val buffers = listOf(4, 6, 10, 15)  // packets of 20 ms
private val steps = listOf(10L, 100L, 1000L, 10000L)

@Composable
private fun MainScreen(c: NbConnection) {
    var host by rememberSaveable { mutableStateOf("192.168.2.1") }
    var port by rememberSaveable { mutableStateOf("8073") }
    var dialText by rememberSaveable { mutableStateOf("10489,900") }
    var dial by rememberSaveable { mutableLongStateOf(10_489_900_000L) }
    var step by rememberSaveable { mutableLongStateOf(100L) }
    var bufferPackets by rememberSaveable { mutableIntStateOf(6) }
    var volume by rememberSaveable { mutableFloatStateOf(1f) }

    // Android 17 blocks sockets to the local network (the board) until the
    // user grants "Local network".
    val context = LocalContext.current
    var permissionError by rememberSaveable { mutableStateOf<String?>(null) }
    fun localNetworkGranted() = Build.VERSION.SDK_INT < 37 ||
        context.checkSelfPermission(Manifest.permission.ACCESS_LOCAL_NETWORK) == PackageManager.PERMISSION_GRANTED
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) {
            permissionError = null
            c.connect("ws://$host:$port/")
        } else {
            permissionError = "Android refuse l'accès au réseau local : autorise « Réseau local » pour QO-100 NB " +
                "dans les réglages de l'appli, sinon la carte reste injoignable en Wi-Fi."
        }
    }
    fun listen() {
        if (localNetworkGranted()) {
            permissionError = null
            c.connect("ws://$host:$port/")
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
        permissionError?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
        c.lastError?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }

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

        // Beacon lock and signal
        BeaconPanel(status)
        SMeter(status?.signalDb)

        // Settings
        ChipRow("Filtre", filters, filters.firstOrNull { it.low == status?.filterLowHz && it.high == status?.filterHighHz }, { it.label }) {
            c.setFilter(it.low, it.high)
        }
        ChipRow("AGC", agcModes, agcModes.firstOrNull { it.second == status?.agc }, { it.first }) { c.setAgc(it.second) }
        ChipRow("Débit", bitrates, status?.bitrate, { "${it / 1000}k" }) { c.setBitrate(it) }
        ChipRow("Tampon", buffers, bufferPackets, { "${it * 20} ms" }) {
            bufferPackets = it
            c.setPrefillPackets(it)
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Volume", modifier = Modifier.width(72.dp))
            Slider(value = volume, onValueChange = {
                volume = it
                c.setVolume(it)
            })
        }

        val s = c.stats
        Text(
            String.format(
                Locale.FRANCE, "Audio %.1f kbit/s • tampon %d ms • perdus %d • jetés %d • coupures %d",
                s.kbps, s.bufferMs, s.lost, s.discarded, s.underruns,
            ),
            style = MaterialTheme.typography.bodySmall,
        )
        hello?.let {
            Text(
                "nbd ${it.source} • ${Qo100.formatMHz(it.minFreqHz)} – ${Qo100.formatMHz(it.maxFreqHz)}" +
                    (status?.let { st -> " • ${st.clients} client(s)" } ?: ""),
                style = MaterialTheme.typography.bodySmall,
            )
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
