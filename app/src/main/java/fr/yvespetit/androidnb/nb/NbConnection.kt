package fr.yvespetit.androidnb.nb

import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
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

    data class Stats(
        val packets: Long = 0,
        val lost: Long = 0,
        val discarded: Long = 0,
        val underruns: Long = 0,
        val bufferMs: Int = 0,
        val kbps: Double = 0.0,
    )

    val jitter = JitterBuffer()
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var wanted = false
    @Volatile private var socket: NbWebSocket? = null
    private var worker: Thread? = null
    private var player: OpusPlayer? = null
    private var volume = 1f

    fun connect(url: String) {
        disconnect()
        wanted = true
        lastError = null
        worker = Thread({ loop(url) }, "nb-ws").apply { start() }
    }

    fun disconnect() {
        wanted = false
        socket?.close()
        worker?.join(2000)
        worker = null
        stopAudio()
        link = Link.DISCONNECTED
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
        while (wanted) {
            main.post { link = Link.CONNECTING }
            try {
                val ws = NbWebSocket.connect(url)
                socket = ws
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
                if (wanted) main.post { lastError = e.message ?: e.javaClass.simpleName }
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
        var windowStart = System.nanoTime()
        while (wanted) {
            when (val f = ws.receive()) {
                is NbWebSocket.Closed -> {
                    if (wanted) main.post { lastError = "nbd a fermé la connexion" }
                    return
                }
                is NbWebSocket.Binary -> NbProtocol.parseAudio(f.data)?.let {
                    bytes += f.data.size
                    jitter.put(it)
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
                )
                bytes = 0
                windowStart = now
                main.post { stats = s }
            }
        }
    }

    companion object {
        private const val RETRY_MS = 3000L
    }
}
