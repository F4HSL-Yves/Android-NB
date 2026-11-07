package fr.yvespetit.androidnb.nb

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Plays the Opus packets queued in [buffer]: Android's own Opus decoder
 * (MediaCodec, every device since Android 5) feeding an AudioTrack. The
 * blocking AudioTrack write paces the decoding, so the jitter buffer size is
 * the network delay we keep.
 */
class OpusPlayer(private val buffer: JitterBuffer) {
    @Volatile private var running = false
    private var thread: Thread? = null
    @Volatile var volume: Float = 1f
        set(value) {
            field = value.coerceIn(0f, 1f)
            track?.setVolume(field)
        }
    @Volatile private var track: AudioTrack? = null
    @Volatile var decoded = 0L
        private set

    fun start() {
        if (running) return
        running = true
        thread = Thread({ run() }, "nb-opus").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun stop() {
        running = false
        buffer.release()
        thread?.join(1000)
        thread = null
    }

    private fun run() {
        var codec: MediaCodec? = null
        var audio: AudioTrack? = null
        try {
            codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS).apply {
                configure(opusFormat(), null, null, 0)
                start()
            }
            val minBuf = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
            audio = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build(),
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setSampleRate(RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .build(),
                )
                .setBufferSizeInBytes(maxOf(minBuf, RATE * 2 / 10))  // 100 ms
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            audio.setVolume(volume)
            audio.play()
            track = audio
            val info = MediaCodec.BufferInfo()
            var ptsUs = 0L
            var pcm = ByteArray(0)
            while (running) {
                val packet = buffer.take(100) ?: continue
                val inIndex = codec.dequeueInputBuffer(20_000)
                if (inIndex >= 0) {
                    val inBuf = codec.getInputBuffer(inIndex)!!
                    inBuf.clear()
                    inBuf.put(packet.opus)
                    codec.queueInputBuffer(inIndex, 0, packet.opus.size, ptsUs, 0)
                    ptsUs += FRAME_US
                }
                while (true) {
                    val outIndex = codec.dequeueOutputBuffer(info, 0)
                    if (outIndex < 0) break  // try again later / format changed
                    val outBuf = codec.getOutputBuffer(outIndex)!!
                    if (pcm.size < info.size) pcm = ByteArray(info.size)
                    outBuf.position(info.offset)
                    outBuf.get(pcm, 0, info.size)
                    codec.releaseOutputBuffer(outIndex, false)
                    if (info.size > 0) {
                        audio.write(pcm, 0, info.size)
                        decoded++
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Opus playback stopped", e)
        } finally {
            track = null
            runCatching { audio?.stop() }
            runCatching { audio?.release() }
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
        }
    }

    companion object {
        private const val TAG = "OpusPlayer"
        const val RATE = 48000
        private const val FRAME_US = 20_000L
        private const val PRE_SKIP = 312  // libopus encoder lookahead at 48 kHz

        /** MediaCodec wants the Ogg identification header and two durations. */
        fun opusFormat(): MediaFormat = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, RATE, 1).apply {
            setByteBuffer("csd-0", ByteBuffer.wrap(opusHead()))
            setByteBuffer("csd-1", longLe(PRE_SKIP * 1_000_000_000L / RATE))
            setByteBuffer("csd-2", longLe(80_000_000L))
        }

        fun opusHead(): ByteArray = ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN).apply {
            put("OpusHead".toByteArray(Charsets.US_ASCII))
            put(1)  // version
            put(1)  // mono
            putShort(PRE_SKIP.toShort())
            putInt(RATE)
            putShort(0)  // output gain
            put(0)  // mapping family 0
        }.array()

        private fun longLe(v: Long): ByteBuffer =
            ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(v).apply { flip() }
    }
}
