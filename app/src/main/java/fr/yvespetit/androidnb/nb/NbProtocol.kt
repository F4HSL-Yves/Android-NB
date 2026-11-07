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

    data class Hello(
        val version: Int,
        val centerHz: Long,
        val rate: Double,
        val minFreqHz: Long,
        val maxFreqHz: Long,
        val source: String,
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
    data class Unknown(val raw: String) : Message

    class AudioPacket(val seq: Int, val opus: ByteArray)

    fun parse(text: String): Message = when (string(text, "type")) {
        "hello" -> HelloMessage(
            Hello(
                version = number(text, "version")?.toInt() ?: 0,
                centerHz = number(text, "center")?.toLong() ?: 0L,
                rate = number(text, "rate") ?: 0.0,
                minFreqHz = number(text, "min_freq")?.toLong() ?: 0L,
                maxFreqHz = number(text, "max_freq")?.toLong() ?: 0L,
                source = string(text, "source") ?: "",
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

    fun freqCommand(hz: Long) = """{"cmd":"freq","hz":$hz}"""
    fun filterCommand(lowHz: Int, highHz: Int) = """{"cmd":"filter","low":$lowHz,"high":$highHz}"""
    fun agcCommand(mode: Int) = """{"cmd":"agc","mode":$mode}"""
    fun bitrateCommand(bps: Int) = """{"cmd":"bitrate","bps":$bps}"""

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
