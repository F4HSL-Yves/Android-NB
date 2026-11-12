package fr.yvespetit.androidnb.nb

import android.content.Context
import com.jcraft.jsch.ChannelExec
import com.jcraft.jsch.JSch
import com.jcraft.jsch.JSchException
import com.jcraft.jsch.Session
import java.io.ByteArrayOutputStream
import java.io.File

/**
 * Installs or updates QO-100 NB on the user's own Tezuka board from the
 * phone, over ssh, with the root password the user types. The board files
 * ship inside the APK (assets/board/), so nothing is copied by hand.
 *
 * Why ssh at all: the Tezuka firmware only runs /mnt/jffs2/autorun.sh at
 * boot, in the board's internal flash, which a PC copying to the SD card
 * cannot reach; install.sh adds one line there and leaves the rest alone.
 *
 * Host key: trusted on first use and remembered (known_hosts in the app's
 * private files). If it later changes, the install stops and says so; the
 * user can forget the old key (after a firmware reflash) with [forgetHost].
 *
 * Files go to /boot/nb on the SD card. nb.conf and the FFTW plans
 * (wdspWisdom00) are only written when missing, so the user's settings
 * survive updates.
 */
class BoardInstaller(private val context: Context) {

    /** Files shipped in assets/board/, in upload order; nbd is the big one. */
    private val files = listOf("nbctl.sh", "install.sh", "nb.conf", "wdspWisdom00", "nbd")
    /**
     * Written only when missing: the user's settings, and the FFTW plans
     * (made on a Zynq board; without them nbd spends minutes planning its
     * FFTs at first start, and it rewrites the file itself if it must).
     */
    private val keepIfPresent = setOf("nb.conf", "wdspWisdom00")
    private val knownHosts = File(context.filesDir, "known_hosts")
    private val prefs = context.getSharedPreferences("board_install", Context.MODE_PRIVATE)

    fun available(): Boolean = runCatching { context.assets.open("board/nbd").close() }.isSuccess

    /** This APK's version: the board is updated when it changes. */
    private val appVersion: Long = runCatching {
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        if (android.os.Build.VERSION.SDK_INT >= 28) info.longVersionCode else @Suppress("DEPRECATION") info.versionCode.toLong()
    }.getOrDefault(0L)

    /** Root password last accepted by the board, else Tezuka's default. */
    fun password(): String = prefs.getString("password", null) ?: DEFAULT_PASSWORD

    /** True when [host] has not received this version of the board files yet. */
    fun needsUpdate(host: String): Boolean = available() && prefs.getLong("installed_$host", -1L) != appVersion

    /** Forgets that [host] is up to date (its SD card was wiped, for instance). */
    fun forgetInstalled(host: String) = prefs.edit().remove("installed_$host").apply()

    /** Forgets the remembered host key of [host] (after reflashing the board). */
    fun forgetHost(host: String) {
        if (!knownHosts.exists()) return
        knownHosts.writeText(knownHosts.readLines().filterNot { it.startsWith("$host ") }.joinToString("\n", postfix = "\n"))
    }

    /** Runs the whole install; progress lines go to [log]. Throws on failure. */
    fun install(host: String, password: String, log: (String) -> Unit) {
        knownHosts.createNewFile()
        val jsch = JSch().apply { setKnownHosts(knownHosts.absolutePath) }
        val session = jsch.getSession("root", host, 22).apply {
            setPassword(password)
            // First connection ("no"): JSch remembers the key in known_hosts.
            // Later ("yes"): it must match the remembered one.
            val known = knownHosts.readLines().any { it.startsWith("$host ") }
            setConfig("StrictHostKeyChecking", if (known) "yes" else "no")
            setConfig("PreferredAuthentications", "password,keyboard-interactive")
            timeout = 15_000
        }
        log("Connexion à root@$host…")
        try {
            session.connect(15_000)
        } catch (e: JSchException) {
            val msg = e.message.orEmpty()
            throw IllegalStateException(
                when {
                    "HostKey has been changed" in msg || "reject HostKey" in msg ->
                        "La clé ssh de la carte a changé depuis la dernière installation. Si tu as reflashé la carte, " +
                            "appuie sur « Oublier la clé de la carte » puis recommence ; sinon, vérifie que c'est bien ta carte."
                    "Auth fail" in msg || "Auth cancel" in msg -> "Mot de passe root refusé par la carte."
                    else -> "Connexion ssh impossible : $msg"
                },
            )
        }
        try {
            run(session, "test -d /mnt/jffs2 && test -d /boot", "Ce n'est pas une carte Tezuka (pas de /mnt/jffs2 ou /boot)")
            run(session, "mkdir -p /boot/nb", "Impossible de créer /boot/nb sur la carte SD")
            for (name in files) {
                val target = "/boot/nb/$name"
                if (name in keepIfPresent && exec(session, "test -f $target").first == 0) {
                    log("$name déjà présent : conservé")
                    continue
                }
                val bytes = context.assets.open("board/$name").use { it.readBytes() }
                log("Copie de $name (${bytes.size / 1024} ko)…")
                upload(session, bytes, target)
            }
            log("Démarrage automatique et lancement…")
            val (code, out) = exec(session, "sh /boot/nb/install.sh 2>&1")
            out.lines().filter { it.isNotBlank() }.forEach(log)
            if (code != 0) throw IllegalStateException("install.sh a échoué (code $code)")
            prefs.edit().putLong("installed_$host", appVersion).putString("password", password).apply()
            log("Terminé : QO-100 NB est installé sur la carte.")
        } finally {
            session.disconnect()
        }
    }

    companion object {
        /** Tezuka's root password out of the box. */
        const val DEFAULT_PASSWORD = "analog"
    }

    private fun run(session: Session, command: String, error: String) {
        if (exec(session, command).first != 0) throw IllegalStateException(error)
    }

    /** The board has no sftp-server: stream through cat, then rename in one step. */
    private fun upload(session: Session, bytes: ByteArray, target: String) {
        val channel = session.openChannel("exec") as ChannelExec
        channel.setCommand("cat > $target.part && mv $target.part $target && chmod 755 $target")
        val out = channel.outputStream
        channel.connect(15_000)
        out.write(bytes)
        out.flush()
        out.close()
        while (!channel.isClosed) Thread.sleep(50)
        val code = channel.exitStatus
        channel.disconnect()
        if (code != 0) throw IllegalStateException("Copie de $target échouée (code $code)")
    }

    private fun exec(session: Session, command: String): Pair<Int, String> {
        val channel = session.openChannel("exec") as ChannelExec
        channel.setCommand(command)
        val output = ByteArrayOutputStream()
        val input = channel.inputStream
        channel.connect(15_000)
        val buf = ByteArray(4096)
        while (true) {
            while (input.available() > 0) {
                val n = input.read(buf)
                if (n < 0) break
                output.write(buf, 0, n)
            }
            if (channel.isClosed && input.available() == 0) break
            Thread.sleep(50)
        }
        val code = channel.exitStatus
        channel.disconnect()
        return code to String(output.toByteArray(), Charsets.UTF_8)
    }
}
