package fr.yvespetit.androidnb.nb

/**
 * Messages exchanged with the nbd daemon over its WebSocket
 * (daemon/src/protocol.h is the reference).
 *
 * nbd sends flat JSON objects; the few fields needed here are read with a
 * small extractor so the class stays plain Kotlin, testable on the JVM.
 */
object NbProtocol {
    const val VERSION = 1
    const val AUDIO_OPUS: Byte = 0x01
    const val SPECTRUM_LINE: Byte = 0x02
    const val MIC_OPUS: Byte = 0x03

    data class Hello(
        val version: Int,
        val centerHz: Long,
        val rate: Double,
        val minFreqHz: Long,
        val maxFreqHz: Long,
        val source: String,
        /** Where nbd takes the waterfall from: "maia", "local" or "off". */
        val spectrumSource: String = "off",
        /** nbd runs with --tx: the app may transmit. */
        val tx: Boolean = false,
    )

    /** Transmitter state, sent by nbd on each change and every 200 ms while on. */
    data class TxStatus(
        val state: String,
        val message: String,
        val uplinkHz: Long,
        val loHz: Long,
        val calHz: Double,
        val tune: Boolean,
        val micDb: Double,
        val alcDb: Double,
        val outDb: Double,
        val underruns: Long,
        val onS: Double,
    )

    data class Status(
        val freqHz: Long,
        val beacon: BeaconState,
        val correctionHz: Double,
        val driftHzPerS: Double,
        val snrDb: Double,
        val signalDb: Double,
        val filterLowHz: Int,
        val filterHighHz: Int,
        val agc: Int,
        val bitrate: Int,
        val clients: Int,
        val dropped: Long,
        val uptimeS: Double,
    )

    enum class BeaconState { SEARCHING, LOCKED, HOLDOVER, UNKNOWN }

    sealed interface Message
    data class HelloMessage(val hello: Hello) : Message
    data class StatusMessage(val status: Status) : Message
    data class ErrorMessage(val text: String) : Message
    data class TxMessage(val tx: TxStatus) : Message
    data class Unknown(val raw: String) : Message

    class AudioPacket(val seq: Int, val opus: ByteArray)

    /**
     * One waterfall row. The view is in true downlink frequencies (nbd has
     * removed the LNB drift); bin k covers
     * [left + k * span / bins, left + (k + 1) * span / bins).
     */
    class SpectrumLine(
        val seq: Int,
        val centerOffsetHz: Int,
        val spanHz: Int,
        val refDb: Float,
        val stepDb: Float,
        val values: ByteArray,
    ) {
        val bins get() = values.size
        /** Power of bin k in dBFS. */
        fun db(k: Int): Float = refDb + (values[k].toInt() and 0xFF) * stepDb
        fun hasData(k: Int): Boolean = values[k].toInt() != 0
    }

    fun parse(text: String): Message = when (string(text, "type")) {
        "hello" -> HelloMessage(
            Hello(
                version = number(text, "version")?.toInt() ?: 0,
                centerHz = number(text, "center")?.toLong() ?: 0L,
                rate = number(text, "rate") ?: 0.0,
                minFreqHz = number(text, "min_freq")?.toLong() ?: 0L,
                maxFreqHz = number(text, "max_freq")?.toLong() ?: 0L,
                source = string(text, "source") ?: "",
                spectrumSource = text.indexOf("\"spectrum\"").let { i ->
                    if (i < 0) "off" else string(text.substring(i + 1), "source") ?: "off"
                },
                tx = text.contains("\"tx\":true"),
            ),
        )
        "tx" -> TxMessage(
            TxStatus(
                state = string(text, "state") ?: "off",
                message = string(text, "message") ?: "",
                uplinkHz = number(text, "uplink")?.toLong() ?: 0L,
                loHz = number(text, "lo")?.toLong() ?: 0L,
                calHz = number(text, "cal") ?: 0.0,
                tune = text.contains("\"tune\":true"),
                micDb = number(text, "mic") ?: -200.0,
                alcDb = number(text, "alc") ?: 0.0,
                outDb = number(text, "out") ?: -200.0,
                underruns = number(text, "underruns")?.toLong() ?: 0L,
                onS = number(text, "on_s") ?: 0.0,
            ),
        )
        "status" -> {
            val filter = array(text, "filter")
            StatusMessage(
                Status(
                    freqHz = number(text, "freq")?.toLong() ?: 0L,
                    beacon = when (string(text, "beacon")) {
                        "searching" -> BeaconState.SEARCHING
                        "locked" -> BeaconState.LOCKED
                        "holdover" -> BeaconState.HOLDOVER
                        else -> BeaconState.UNKNOWN
                    },
                    correctionHz = number(text, "correction") ?: 0.0,
                    driftHzPerS = number(text, "drift") ?: 0.0,
                    snrDb = number(text, "snr") ?: 0.0,
                    signalDb = number(text, "s") ?: -140.0,
                    filterLowHz = filter.getOrNull(0)?.toInt() ?: 0,
                    filterHighHz = filter.getOrNull(1)?.toInt() ?: 0,
                    agc = number(text, "agc")?.toInt() ?: 0,
                    bitrate = number(text, "bitrate")?.toInt() ?: 0,
                    clients = number(text, "clients")?.toInt() ?: 0,
                    dropped = number(text, "dropped")?.toLong() ?: 0L,
                    uptimeS = number(text, "uptime") ?: 0.0,
                ),
            )
        }
        "error" -> ErrorMessage(string(text, "message") ?: text)
        else -> Unknown(text)
    }

    /** Binary frame: 0x01, sequence (uint16 big endian), one Opus packet. */
    fun parseAudio(frame: ByteArray): AudioPacket? {
        if (frame.size < 4 || frame[0] != AUDIO_OPUS) return null
        val seq = ((frame[1].toInt() and 0xFF) shl 8) or (frame[2].toInt() and 0xFF)
        return AudioPacket(seq, frame.copyOfRange(3, frame.size))
    }

    /**
     * Binary frame 0x02 (big endian): seq u16, view centre minus band centre
     * (Hz) i32, span (Hz) u32, ref (0.1 dB) i16, step (0.1 dB) u8, then one
     * byte per bin.
     */
    fun parseSpectrum(frame: ByteArray): SpectrumLine? {
        if (frame.size < 15 || frame[0] != SPECTRUM_LINE) return null
        fun u(i: Int) = frame[i].toInt() and 0xFF
        val seq = (u(1) shl 8) or u(2)
        val center = (u(3) shl 24) or (u(4) shl 16) or (u(5) shl 8) or u(6)
        val span = (u(7) shl 24) or (u(8) shl 16) or (u(9) shl 8) or u(10)
        val ref = ((u(11) shl 8) or u(12)).toShort().toInt()
        val step = u(13)
        if (span <= 0 || step == 0) return null
        return SpectrumLine(seq, center, span, ref / 10f, step / 10f, frame.copyOfRange(14, frame.size))
    }

    /** Binary frame: 0x03, sequence (uint16 big endian), one Opus packet of the microphone. */
    fun micPacket(seq: Int, opus: ByteArray): ByteArray =
        byteArrayOf(MIC_OPUS, (seq shr 8).toByte(), seq.toByte()) + opus

    fun txCommand(on: Boolean) = """{"cmd":"tx","on":${if (on) 1 else 0}}"""
    fun tuneCommand(on: Boolean, hz: Int = 1000) = """{"cmd":"tune","on":${if (on) 1 else 0},"hz":$hz}"""
    fun txCalCommand(hz: Int) = """{"cmd":"txcal","hz":$hz}"""
    fun micGainCommand(db: Int) = """{"cmd":"micgain","db":$db}"""

    fun freqCommand(hz: Long) = """{"cmd":"freq","hz":$hz}"""
    fun filterCommand(lowHz: Int, highHz: Int) = """{"cmd":"filter","low":$lowHz,"high":$highHz}"""
    fun agcCommand(mode: Int) = """{"cmd":"agc","mode":$mode}"""
    fun bitrateCommand(bps: Int) = """{"cmd":"bitrate","bps":$bps}"""
    /** Waterfall view shared by all clients; rate 0 stops it. centerHz 0 = band centre. */
    fun spectrumCommand(centerHz: Long, spanHz: Int, bins: Int, rate: Double) =
        """{"cmd":"spectrum","center":$centerHz,"span":$spanHz,"bins":$bins,"rate":$rate}"""

    private fun valueStart(json: String, key: String): Int? {
        val k = "\"$key\""
        val p = json.indexOf(k)
        if (p < 0) return null
        var i = json.indexOf(':', p + k.length)
        if (i < 0) return null
        i++
        while (i < json.length && json[i].isWhitespace()) i++
        return i
    }

    internal fun number(json: String, key: String): Double? {
        val start = valueStart(json, key) ?: return null
        var end = start
        while (end < json.length && (json[end].isDigit() || json[end] in "+-.eE")) end++
        return json.substring(start, end).toDoubleOrNull()
    }

    internal fun string(json: String, key: String): String? {
        val start = valueStart(json, key) ?: return null
        if (start >= json.length || json[start] != '"') return null
        val end = json.indexOf('"', start + 1)
        return if (end < 0) null else json.substring(start + 1, end)
    }

    internal fun array(json: String, key: String): List<Double> {
        val start = valueStart(json, key) ?: return emptyList()
        if (start >= json.length || json[start] != '[') return emptyList()
        val end = json.indexOf(']', start)
        if (end < 0) return emptyList()
        return json.substring(start + 1, end).split(',').mapNotNull { it.trim().toDoubleOrNull() }
    }
}
