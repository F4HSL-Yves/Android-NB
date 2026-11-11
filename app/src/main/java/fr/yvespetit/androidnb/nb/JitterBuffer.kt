package fr.yvespetit.androidnb.nb

/**
 * Packet queue between the network and the audio decoder.
 *
 * Playback starts once [prefill] packets are queued (20 ms each), which
 * absorbs the irregular arrival of a 4G link. When packets pile up beyond
 * [maxPackets] (after a network stall), the oldest are thrown away so the
 * delay goes back down instead of staying long. Sequence gaps are counted as
 * lost packets. Thread safe.
 */
class JitterBuffer(prefill: Int = 6, val maxPackets: Int = 25) {
    @Volatile var prefill: Int = prefill
        set(value) {
            field = value.coerceIn(1, maxPackets)
        }

    private val queue = ArrayDeque<NbProtocol.AudioPacket>()
    private val lock = Object()
    private var playing = false
    private var lastSeq = -1
    private var released = false

    var received = 0L
        private set
    var lost = 0L
        private set
    var discarded = 0L
        private set
    var underruns = 0L
        private set

    fun put(packet: NbProtocol.AudioPacket) = synchronized(lock) {
        if (lastSeq >= 0) {
            val gap = (packet.seq - lastSeq - 1) and 0xFFFF
            // A huge gap is nbd restarting its counter, not 20 minutes of loss.
            if (gap in 1..1000) lost += gap
        }
        lastSeq = packet.seq
        received++
        queue.addLast(packet)
        while (queue.size > maxPackets) {
            queue.removeFirst()
            discarded++
        }
        if (!playing && queue.size >= prefill) playing = true
        lock.notifyAll()
    }

    /**
     * Next packet to decode, waiting up to [timeoutMs]. Returns null while
     * pre-filling, on timeout or after [release]. An empty queue during
     * playback counts as an underrun and restarts the pre-fill.
     */
    fun take(timeoutMs: Long): NbProtocol.AudioPacket? = synchronized(lock) {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (!released) {
            if (playing) {
                queue.removeFirstOrNull()?.let { return it }
                playing = false
                underruns++
            }
            val wait = deadline - System.currentTimeMillis()
            if (wait <= 0) return null
            lock.wait(wait)
        }
        null
    }

    val size: Int get() = synchronized(lock) { queue.size }

    /** Empties the queue and makes the buffer usable again after [release]. */
    fun clear() = synchronized(lock) {
        queue.clear()
        playing = false
        lastSeq = -1
        released = false
    }

    fun release() = synchronized(lock) {
        released = true
        lock.notifyAll()
    }
}
