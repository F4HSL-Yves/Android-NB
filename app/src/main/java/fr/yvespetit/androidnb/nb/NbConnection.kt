package fr.yvespetit.androidnb.nb

import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/**
 * Link to nbd: one WebSocket for status, commands and audio, reconnected
 * automatically while the user wants it. State is exposed as Compose state,
 * always updated on the main thread.
 */
class NbConnection {
    enum class Link { DISCONNECTED, CONNECTING, CONNECTED }

    var link by mutableStateOf(Link.DISCONNECTED)
        private set
    var lastError by mutableStateOf<String?>(null)
        private set
    var hello by mutableStateOf<NbProtocol.Hello?>(null)
        private set
    var status by mutableStateOf<NbProtocol.Status?>(null)
        private set
    var stats by mutableStateOf(Stats())
        private set

    /** Board mode from state/nb/mode ("nb" or "datv"), null while unknown. */
    var boardMode by mutableStateOf<String?>(null)
        private set
    var mqttUp by mutableStateOf(false)
        private set

    /** Waterfall rows; only touched on the main thread. */
    val waterfall = Waterfall()
    /** Bumped on every new waterfall row, so Compose redraws. */
    var waterfallVersion by mutableIntStateOf(0)
        private set

    data class Stats(
        val packets: Long = 0,
        val lost: Long = 0,
        val discarded: Long = 0,
        val underruns: Long = 0,
        val bufferMs: Int = 0,
        val kbps: Double = 0.0,
        val waterfallKbps: Double = 0.0,
    )

    val jitter = JitterBuffer()
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var wanted = false
    @Volatile private var socket: NbWebSocket? = null
    private var worker: Thread? = null
    private var player: OpusPlayer? = null
    private var volume = 1f
    @Volatile private var mqtt: MqttLite? = null
    private var mqttWorker: Thread? = null

    /**
     * Listens to nbd on [host]:[port]. The board is first put in NB mode
     * over its MQTT broker (nbctl.sh pauses zynqrx and starts nbd), then the
     * WebSocket connects, retrying while nbd starts. A DATV start from any
     * client takes the radio (nbctl switches to datv) and its stop gives it
     * back (nbctl returns to nb); the app follows state/nb/mode.
     */
    fun connect(host: String, port: Int) {
        disconnect()
        wanted = true
        lastError = null
        mqttWorker = Thread({ mqttLoop(host, requestNb = true) }, "nb-mqtt").apply { start() }
        worker = Thread({ loop("ws://$host:$port/") }, "nb-ws").apply { start() }
    }

    fun disconnect() {
        wanted = false
        socket?.close()
        mqtt?.close()
        worker?.join(2000)
        mqttWorker?.join(2000)
        worker = null
        mqttWorker = null
        stopAudio()
        link = Link.DISCONNECTED
        mqttUp = false
    }

    private fun mqttLoop(host: String, requestNb: Boolean) {
        var askNb = requestNb
        while (wanted) {
            try {
                val m = MqttLite.connect(host, clientId = "android-nb-" + (System.nanoTime() % 100000))
                mqtt = m
                main.post { mqttUp = true }
                m.subscribe("state/nb/mode")
                if (askNb) {
                    m.publish("cmd/nb/mode", "nb")
                    askNb = false
                }
                m.setReadTimeout(MqttLite.KEEPALIVE_S * 1000 / 2)
                while (wanted) {
                    val msg = m.receive()
                    if (msg == null) {
                        m.ping()
                        continue
                    }
                    val payload = msg.payload.trim()
                    if (msg.topic == "state/nb/mode") main.post { boardMode = payload }
                }
            } catch (_: Exception) {
                // Board unreachable or broker restarting: retry below.
            } finally {
                mqtt?.close()
                mqtt = null
                main.post { mqttUp = false }
            }
            if (!wanted) break
            try {
                Thread.sleep(RETRY_MS)
            } catch (_: InterruptedException) {
                break
            }
        }
    }

    fun setPrefillPackets(n: Int) {
        jitter.prefill = n
    }

    fun setVolume(v: Float) {
        volume = v
        player?.volume = v
    }

    fun tune(hz: Long) = send(NbProtocol.freqCommand(hz))
    fun setFilter(lowHz: Int, highHz: Int) = send(NbProtocol.filterCommand(lowHz, highHz))
    fun setAgc(mode: Int) = send(NbProtocol.agcCommand(mode))
    fun setBitrate(bps: Int) = send(NbProtocol.bitrateCommand(bps))
    fun setSpectrum(centerHz: Long, spanHz: Int, bins: Int, rate: Double) =
        send(NbProtocol.spectrumCommand(centerHz, spanHz, bins, rate))

    private fun send(text: String) {
        val ws = socket ?: return
        Thread {
            runCatching { ws.sendText(text) }
        }.start()
    }

    private fun startAudio() {
        jitter.clear()
        player = OpusPlayer(jitter).also {
            it.volume = volume
            it.start()
        }
    }

    private fun stopAudio() {
        player?.stop()
        player = null
    }

    private fun loop(url: String) {
        var failures = 0
        while (wanted) {
            main.post { link = Link.CONNECTING }
            try {
                val ws = NbWebSocket.connect(url)
                socket = ws
                failures = 0
                if (!wanted) {
                    ws.close()
                    break
                }
                main.post {
                    link = Link.CONNECTED
                    lastError = null
                    startAudio()
                }
                receive(ws)
            } catch (e: Exception) {
                // nbd takes a few seconds to come up after a switch to NB (or
                // at boot): only report it once it keeps failing (~10 s).
                failures++
                if (wanted && failures >= 6) main.post { lastError = e.message ?: e.javaClass.simpleName }
            } finally {
                socket?.close()
                socket = null
                main.post { if (link == Link.CONNECTED) stopAudio() }
            }
            if (!wanted) break
            main.post { link = Link.CONNECTING }
            try {
                Thread.sleep(RETRY_MS)
            } catch (_: InterruptedException) {
                break
            }
        }
        main.post { link = Link.DISCONNECTED }
    }

    private fun receive(ws: NbWebSocket) {
        var bytes = 0L
        var spectrumBytes = 0L
        var windowStart = System.nanoTime()
        while (wanted) {
            when (val f = ws.receive()) {
                is NbWebSocket.Closed -> {
                    if (wanted) main.post { lastError = "nbd a fermé la connexion" }
                    return
                }
                is NbWebSocket.Binary -> when (f.data.firstOrNull()) {
                    NbProtocol.AUDIO_OPUS -> NbProtocol.parseAudio(f.data)?.let {
                        bytes += f.data.size
                        jitter.put(it)
                    }
                    NbProtocol.SPECTRUM_LINE -> NbProtocol.parseSpectrum(f.data)?.let { line ->
                        spectrumBytes += f.data.size
                        main.post {
                            hello?.let {
                                waterfall.add(line, it.centerHz)
                                waterfallVersion++
                            }
                        }
                    }
                    else -> {}
                }
                is NbWebSocket.Text -> {
                    when (val m = NbProtocol.parse(f.text)) {
                        is NbProtocol.HelloMessage -> main.post { hello = m.hello }
                        is NbProtocol.StatusMessage -> main.post { status = m.status }
                        is NbProtocol.ErrorMessage -> main.post { lastError = m.text }
                        is NbProtocol.Unknown -> {}
                    }
                }
            }
            val now = System.nanoTime()
            if (now - windowStart >= 1_000_000_000L) {
                val s = Stats(
                    packets = jitter.received,
                    lost = jitter.lost,
                    discarded = jitter.discarded,
                    underruns = jitter.underruns,
                    bufferMs = jitter.size * 20,
                    kbps = bytes * 8.0 / ((now - windowStart) / 1e9) / 1000.0,
                    waterfallKbps = spectrumBytes * 8.0 / ((now - windowStart) / 1e9) / 1000.0,
                )
                bytes = 0
                spectrumBytes = 0
                windowStart = now
                main.post { stats = s }
            }
        }
    }

    companion object {
        private const val RETRY_MS = 1500L
    }
}
