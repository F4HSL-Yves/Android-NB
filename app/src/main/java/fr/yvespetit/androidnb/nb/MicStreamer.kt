package fr.yvespetit.androidnb.nb

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaCodec
import android.media.MediaFormat
import android.media.MediaRecorder
import android.util.Log
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.max

/**
 * Microphone to Opus for the transmitter: AudioRecord at 48 kHz mono, then
 * Android's own Opus encoder (MediaCodec, Android 10+), one packet per
 * 20 ms handed to [onPacket] with its sequence number. nbd needs the
 * stream to keep flowing while transmitting (it stops after 1 s without).
 *
 * The caller holds the RECORD_AUDIO permission.
 */
class MicStreamer(private val onPacket: (seq: Int, opus: ByteArray) -> Unit) {
    @Volatile private var running = false
    private var thread: Thread? = null

    /** Peak level of the last 20 ms, dBFS, for a local meter. */
    @Volatile var levelDb: Double = -100.0
        private set

    /** Last failure (no encoder, microphone refused...), null when fine. */
    @Volatile var error: String? = null
        private set

    val isRunning get() = running

    fun start() {
        if (running) return
        running = true
        error = null
        thread = Thread({ run() }, "nb-mic").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun stop() {
        running = false
        thread?.join(1000)
        thread = null
        levelDb = -100.0
    }

    @SuppressLint("MissingPermission")
    private fun run() {
        var codec: MediaCodec? = null
        var rec: AudioRecord? = null
        try {
            val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, RATE, 1).apply {
                setInteger(MediaFormat.KEY_BIT_RATE, BITRATE)
                setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, FRAME * 2 * 4)
            }
            codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS).apply {
                configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                start()
            }
            val minBuf = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            rec = AudioRecord(
                MediaRecorder.AudioSource.MIC, RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
                max(minBuf, FRAME * 2 * 8),
            )
            if (rec.state != AudioRecord.STATE_INITIALIZED) throw IllegalStateException("micro indisponible")
            rec.startRecording()
            val frame = ByteArray(FRAME * 2)
            val info = MediaCodec.BufferInfo()
            var ptsUs = 0L
            var seq = 0
            while (running) {
                var off = 0
                while (off < frame.size && running) {
                    val n = rec.read(frame, off, frame.size - off)
                    if (n < 0) throw IllegalStateException("lecture micro : erreur $n")
                    off += n
                }
                if (!running) break
                levelDb = peakDb(frame)
                val inIndex = codec.dequeueInputBuffer(10_000)
                if (inIndex >= 0) {
                    codec.getInputBuffer(inIndex)!!.apply {
                        clear()
                        put(frame)
                    }
                    codec.queueInputBuffer(inIndex, 0, frame.size, ptsUs, 0)
                    ptsUs += 20_000
                }
                while (true) {
                    val outIndex = codec.dequeueOutputBuffer(info, 0)
                    if (outIndex < 0) break
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0 && info.size > 0) {
                        val out = codec.getOutputBuffer(outIndex)!!
                        val bytes = ByteArray(info.size)
                        out.position(info.offset)
                        out.get(bytes)
                        onPacket(seq, bytes)
                        seq = (seq + 1) and 0xFFFF
                    }
                    codec.releaseOutputBuffer(outIndex, false)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "microphone stopped", e)
            error = e.message ?: e.javaClass.simpleName
            running = false
        } finally {
            runCatching { rec?.stop() }
            runCatching { rec?.release() }
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
        }
    }

    companion object {
        private const val TAG = "MicStreamer"
        const val RATE = 48000
        const val FRAME = 960  // 20 ms
        const val BITRATE = 24000

        /** Peak of little-endian 16-bit PCM, dBFS. */
        fun peakDb(pcm: ByteArray): Double {
            var peak = 0
            var i = 0
            while (i + 1 < pcm.size) {
                val v = (pcm[i].toInt() and 0xFF) or (pcm[i + 1].toInt() shl 8)
                peak = max(peak, abs(v.toShort().toInt()))
                i += 2
            }
            return if (peak == 0) -100.0 else 20.0 * log10(peak / 32768.0)
        }
    }
}
