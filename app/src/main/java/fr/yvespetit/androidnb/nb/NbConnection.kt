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

    // ------------------------------------------------------------ transmit
    // Same logic as Android-DATV: the RF goes on only once the Arduino has
    // confirmed its PTT (arduino/status/ptt), through Tezuka's tx/mute; the
    // gain is Tezuka's tx/gain. nbd only produces the SSB signal (see
    // daemon/src/transmitter.h) and cuts everything if the phone goes quiet.
    enum class Ptt { OFF, STARTING, ON, STOPPING }

    var ptt by mutableStateOf(Ptt.OFF)
        private set
    /** Why the last transmission ended or failed, or what the PTT is doing. */
    var pttMessage by mutableStateOf("")
        private set
    /** Tezuka's state over MQTT (dt/pluto/<call>/...), null while unknown. */
    var callsign by mutableStateOf<String?>(null)
        private set
    var txGainDb by mutableStateOf<Double?>(null)
        private set
    var txMuted by mutableStateOf<Boolean?>(null)
        private set
    var arduinoPtt by mutableStateOf<Boolean?>(null)
        private set
    var paTempC by mutableStateOf<Double?>(null)
        private set
    /** nbd's transmitter. */
    var tx by mutableStateOf<NbProtocol.TxStatus?>(null)
    /** Last automatic TX calibration (bumped on each result). */
    var txCal by mutableStateOf<NbProtocol.TxCalMessage?>(null)
        private set
    /** Set by the UI (saved setting): PTT through the Arduino, with its confirmation. */
    @Volatile var arduinoPresent = true

    @Volatile private var vCallsign: String? = null
    @Volatile private var vStreamMode: String? = null
    @Volatile private var vArduinoPtt: Boolean? = null
    @Volatile private var arduinoReports = 0L
    @Volatile private var vTxState = "off"
    @Volatile private var vTxMessage = ""
    @Volatile private var pttCancel = false
    @Volatile private var savedStreamMode: String? = null
    private var pttThread: Thread? = null
    private val mic = MicStreamer { seq, opus ->
        val ws = socket ?: return@MicStreamer
        runCatching { ws.sendBinary(NbProtocol.micPacket(seq, opus)) }
    }
    val micLevelDb get() = mic.levelDb

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
        pttOff()
        pttThread?.join(3000)
        pttThread = null
        // Leave pluto_stream as it was found (DATV transmissions need it).
        val restore = savedStreamMode
        val call = vCallsign
        if (restore != null && call != null) {
            mqtt?.let { m -> runCatching { m.publish("cmd/pluto/$call/tx/stream/mode", restore) } }
            savedStreamMode = null
        }
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
                m.subscribe("dt/pluto/+/tx/stream/mode")
                m.subscribe("dt/pluto/+/tx/mute")
                m.subscribe("dt/pluto/+/tx/gain")
                m.subscribe("arduino/status/ptt")
                m.subscribe("arduino/status/pa/temp")
                if (askNb) {
                    m.publish("cmd/nb/mode", "nb")
                    askNb = false
                }
                // The board publishes every second (Arduino, Tezuka), so the
                // link is never idle: ping on our own send clock, or mosquitto
                // drops us after 1.5 keepalives without a packet from us. A
                // link silent for a whole keepalive (no PINGRESP) is dead.
                m.setReadTimeout(1000)
                while (wanted) {
                    if (m.sendIdleMs() >= MqttLite.KEEPALIVE_S * 1000L / 3) m.ping()
                    if (m.receiveIdleMs() > MqttLite.KEEPALIVE_S * 1000L) throw java.io.IOException("MQTT: plus de réponse")
                    val msg = m.receive() ?: continue
                    onMqtt(msg.topic, msg.payload.trim())
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

    private fun onMqtt(topic: String, payload: String) {
        if (topic == "state/nb/mode") {
            main.post { boardMode = payload }
            return
        }
        if (topic == "arduino/status/ptt") {
            val on = when (payload.lowercase()) {
                "on" -> true
                "off" -> false
                else -> return
            }
            vArduinoPtt = on
            arduinoReports++
            main.post { arduinoPtt = on }
            return
        }
        if (topic == "arduino/status/pa/temp") {
            payload.toDoubleOrNull()?.let { t -> main.post { paTempC = t } }
            return
        }
        // dt/pluto/<call>/tx/...
        val parts = topic.split('/')
        if (parts.size < 5 || parts[0] != "dt" || parts[1] != "pluto") return
        val call = parts[2]
        if (vCallsign != call) {
            vCallsign = call
            main.post { callsign = call }
        }
        when (parts.drop(3).joinToString("/")) {
            "tx/stream/mode" -> vStreamMode = payload
            "tx/mute" -> payload.toDoubleOrNull()?.let { v -> main.post { txMuted = v != 0.0 } }
            "tx/gain" -> payload.toDoubleOrNull()?.let { v -> main.post { txGainDb = v } }
        }
    }

    private class TxFail(message: String) : Exception(message)

    private fun waitFor(ms: Long, cond: () -> Boolean): Boolean {
        val end = System.nanoTime() + ms * 1_000_000
        while (System.nanoTime() < end) {
            if (cond()) return true
            if (pttCancel) return false
            Thread.sleep(20)
        }
        return cond()
    }

    private fun setPtt(p: Ptt, message: String) = main.post {
        ptt = p
        pttMessage = message
        applyVolume()
    }

    /** Receive audio silenced while transmitting (the waterfall keeps going). */
    var muteRxOnTx = false
        set(value) {
            field = value
            applyVolume()
        }

    private fun applyVolume() {
        player?.volume = if (muteRxOnTx && ptt != Ptt.OFF) 0f else volume
    }

    /** Starts a transmission (PTT pressed). The RF goes on only after every check. */
    fun pttOn() {
        if (pttThread?.isAlive == true) return
        pttCancel = false
        pttThread = Thread({ runPtt() }, "nb-ptt").apply { start() }
    }

    /** Ends the transmission (PTT released); safe to call at any time. */
    fun pttOff() {
        pttCancel = true
    }

    private fun runPtt() {
        setPtt(Ptt.STARTING, "Préparation…")
        var failure = ""
        try {
            val m = mqtt ?: throw TxFail("MQTT de la carte injoignable")
            val call = vCallsign ?: throw TxFail("Indicatif de la carte inconnu (aucun dt/pluto/… reçu)")
            if (socket == null) throw TxFail("nbd n'est pas connecté")
            // 1. pluto_stream must leave the TX DMA to nbd.
            if (vStreamMode != "pass") {
                if (savedStreamMode == null) savedStreamMode = vStreamMode
                setPtt(Ptt.STARTING, "pluto_stream en mode pass…")
                m.publish("cmd/pluto/$call/tx/stream/mode", "pass")
                if (!waitFor(4000) { vStreamMode == "pass" }) throw TxFail("pluto_stream ne libère pas la DMA TX")
            }
            // 2. Microphone and nbd's transmitter (RF still muted).
            mic.start()
            vTxState = "starting"
            socket?.sendText(NbProtocol.txCommand(true))
            if (!waitFor(5000) { vTxState == "on" || vTxState == "error" }) throw TxFail("nbd ne répond pas")
            if (vTxState != "on") throw TxFail("nbd : " + vTxMessage.ifEmpty { "émission refusée" })
            mic.error?.let { throw TxFail("Micro : $it") }
            // 3. Arduino PTT, confirmed (same as Android-DATV).
            if (arduinoPresent) {
                setPtt(Ptt.STARTING, "PTT Arduino…")
                val before = arduinoReports
                m.publish("arduino/ptt", "on")
                if (!waitFor(3000) { arduinoReports > before && vArduinoPtt == true }) {
                    throw TxFail("PTT non confirmé par l’Arduino : sortie RF maintenue muette")
                }
            }
            if (pttCancel) throw TxFail("")
            // 4. RF on.
            m.publish("cmd/pluto/$call/tx/mute", "0")
            setPtt(Ptt.ON, "Émission")
            while (!pttCancel) {
                if (vTxState != "on") throw TxFail("nbd a coupé l’émission : $vTxMessage")
                if (arduinoPresent && vArduinoPtt == false) throw TxFail("PTT coupé par l’Arduino")
                if (socket == null) throw TxFail("Liaison avec nbd perdue")
                mic.error?.let { throw TxFail("Micro : $it") }
                Thread.sleep(50)
            }
        } catch (e: TxFail) {
            failure = e.message ?: ""
        } catch (e: Exception) {
            failure = e.message ?: e.javaClass.simpleName
        }
        setPtt(Ptt.STOPPING, "Arrêt…")
        // RF off first, then the Arduino, then nbd (which mutes again anyway).
        val m = mqtt
        val call = vCallsign
        if (m != null && call != null) runCatching { m.publish("cmd/pluto/$call/tx/mute", "1") }
        if (m != null && arduinoPresent) runCatching { m.publish("arduino/ptt", "off") }
        runCatching { socket?.sendText(NbProtocol.txCommand(false)) }
        mic.stop()
        setPtt(Ptt.OFF, failure)
    }

    /** Tezuka's TX attenuation, -50..0 dB (cmd/pluto/<call>/tx/gain). */
    fun setTxGain(db: Int) {
        val call = vCallsign ?: return
        val m = mqtt ?: return
        Thread { runCatching { m.publish("cmd/pluto/$call/tx/gain", db.coerceIn(-50, 0).toString()) } }.start()
    }

    fun setTune(on: Boolean, hz: Int = 1000) = send(NbProtocol.tuneCommand(on, hz))
    fun setTxCal(hz: Int) = send(NbProtocol.txCalCommand(hz))
    fun setMicGain(db: Int) = send(NbProtocol.micGainCommand(db))
    fun setTxEq(bassDb: Int, trebleDb: Int) = send(NbProtocol.txEqCommand(bassDb, trebleDb))
    fun setCompressor(on: Boolean) = send(NbProtocol.compCommand(on))

    fun setPrefillPackets(n: Int) {
        jitter.prefill = n
    }

    fun setVolume(v: Float) {
        volume = v
        applyVolume()
    }

    fun calibrateTx() = send(NbProtocol.TX_CAL_AUTO)

    fun tune(hz: Long) = send(NbProtocol.freqCommand(hz))
    fun setFilter(lowHz: Int, highHz: Int) = send(NbProtocol.filterCommand(lowHz, highHz))
    fun setAgc(mode: Int) = send(NbProtocol.agcCommand(mode))
    fun setNr(mode: Int) = send(NbProtocol.nrCommand(mode))
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
            it.volume = if (muteRxOnTx && ptt != Ptt.OFF) 0f else volume
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
                        is NbProtocol.ErrorMessage -> {
                            // A refused {"cmd":"tx"} comes back as an error.
                            if (vTxState == "starting") {
                                vTxMessage = m.text
                                vTxState = "error"
                            }
                            main.post { lastError = m.text }
                        }
                        is NbProtocol.TxMessage -> {
                            vTxState = m.tx.state
                            vTxMessage = m.tx.message
                            main.post { tx = m.tx }
                        }
                        is NbProtocol.TxCalMessage -> main.post { txCal = m }
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
