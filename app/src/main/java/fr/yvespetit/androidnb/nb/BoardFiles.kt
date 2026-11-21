package fr.yvespetit.androidnb.nb

import android.content.Context
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.zip.ZipInputStream

/**
 * Where the board files come from: the latest GitHub release when it is
 * newer than this APK (downloaded once and kept in the app's files), else
 * the copy shipped in the APK (assets/board/). So a new nbd reaches the
 * board at the next START without updating the app, as on the iPhone.
 *
 * Blocking network calls: use from a background thread.
 */
class BoardFiles(private val context: Context) {

    /** A set of board files and the release tag they come from. */
    class Bundle(val tag: String, private val read: (String) -> ByteArray) {
        fun bytes(name: String): ByteArray = read(name)
    }

    private val prefs = context.getSharedPreferences("board_files", Context.MODE_PRIVATE)
    private val cacheDir = File(context.filesDir, "board_release")

    /** Tag of the files shipped in this APK (CI sets versionName from the tag). */
    val bundledTag: String = runCatching {
        "v" + (context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "0")
    }.getOrDefault("v0")

    private val cachedTag: String?
        get() = prefs.getString("cached_tag", null)?.takeIf { tag -> NAMES.all { File(cacheDir, it).isFile } && tag.isNotEmpty() }

    /**
     * CI debug APK (pull requests): no tag, so versionName stays at the
     * build.gradle.kts default and every release looks newer. It must use
     * its own board files, else a PR could never be tested on a board.
     */
    private val devBuild: Boolean get() = bundledTag == DEV_TAG && bundledAvailable()

    fun bundledAvailable(): Boolean = runCatching { context.assets.open("board/nbd").close() }.isSuccess

    fun available(): Boolean = bundledAvailable() || cachedTag != null

    /**
     * Asks GitHub for the latest release and downloads its board files when
     * they are newer than everything the phone has. Quiet on failure (no
     * network, GitHub slow): the files already on the phone are used.
     */
    fun refresh(log: (String) -> Unit = {}) {
        if (devBuild) return
        val latest = runCatching { latestRelease() }.getOrNull() ?: return
        val (tag, zipUrl) = latest
        val have = listOfNotNull(cachedTag, bundledTag.takeIf { bundledAvailable() }).maxWithOrNull(::compareTags)
        if (have != null && compareTags(tag, have) <= 0) return
        log("Téléchargement de QO-100 NB $tag…")
        runCatching { download(tag, zipUrl) }
            .onFailure { log("Téléchargement impossible (${it.message}) : fichiers ${have ?: "?"} gardés") }
    }

    /**
     * Tag of the latest GitHub release when it is newer than this APK (seen
     * at the last refresh), so the app can offer to download the new APK.
     */
    fun newerApp(): String? {
        if (!bundledAvailable()) return null  // development build
        val latest = prefs.getString("latest_tag", null) ?: return null
        return latest.takeIf { compareTags(it, bundledTag) > 0 }
    }

    /** The newest files the phone has: downloaded release or the APK's own. */
    fun best(): Bundle {
        val cached = cachedTag.takeUnless { devBuild }
        if (cached != null && (!bundledAvailable() || compareTags(cached, bundledTag) > 0)) {
            return Bundle(cached) { File(cacheDir, it).readBytes() }
        }
        return Bundle(bundledTag) { name -> context.assets.open("board/$name").use { it.readBytes() } }
    }

    private fun latestRelease(): Pair<String, String> {
        val conn = URL(API).openConnection() as HttpURLConnection
        conn.connectTimeout = 4000
        conn.readTimeout = 4000
        conn.useCaches = false
        conn.setRequestProperty("Accept", "application/vnd.github+json")
        conn.setRequestProperty("Cache-Control", "no-cache")
        try {
            if (conn.responseCode != 200) throw IllegalStateException("GitHub ${conn.responseCode}")
            val json = JSONObject(conn.inputStream.bufferedReader().readText())
            val tag = json.getString("tag_name")
            prefs.edit().putString("latest_tag", tag).apply()
            val assets = json.getJSONArray("assets")
            for (i in 0 until assets.length()) {
                val a = assets.getJSONObject(i)
                if (a.getString("name") == "qo100-nb-carte-$tag.zip") return tag to a.getString("browser_download_url")
            }
            throw IllegalStateException("pas d'archive carte dans la release $tag")
        } finally {
            conn.disconnect()
        }
    }

    private fun download(tag: String, zipUrl: String) {
        val conn = URL(zipUrl).openConnection() as HttpURLConnection
        conn.connectTimeout = 8000
        conn.readTimeout = 20000
        conn.instanceFollowRedirects = true
        val tmp = File(context.filesDir, "board_release.tmp")
        tmp.deleteRecursively()
        tmp.mkdirs()
        try {
            if (conn.responseCode != 200) throw IllegalStateException("HTTP ${conn.responseCode}")
            ZipInputStream(conn.inputStream.buffered()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    val name = entry.name.substringAfterLast('/')
                    if (!entry.isDirectory && name in NAMES) File(tmp, name).outputStream().use { zip.copyTo(it) }
                }
            }
            val missing = NAMES.filterNot { File(tmp, it).isFile }
            if (missing.isNotEmpty()) throw IllegalStateException("archive incomplète (${missing.joinToString()})")
            cacheDir.deleteRecursively()
            if (!tmp.renameTo(cacheDir)) throw IllegalStateException("rangement impossible")
            prefs.edit().putString("cached_tag", tag).apply()
        } finally {
            conn.disconnect()
            tmp.deleteRecursively()
        }
    }

    companion object {
        /** Page where the latest APK is downloaded. */
        const val RELEASES_URL = "https://github.com/F4HSL-Yves/Android-NB/releases/latest"

        private const val API = "https://api.github.com/repos/F4HSL-Yves/Android-NB/releases/latest"

        /** versionName of an untagged build ("0.1" in build.gradle.kts). */
        private const val DEV_TAG = "v0.1"

        /** Files of the board package, in upload order; nbd is the big one. */
        val NAMES = listOf("nbctl.sh", "install.sh", "nb.conf", "wdspWisdom00", "nbd")

        /** Compares "v1.9" and "v1.17" as versions (1.9 < 1.17). */
        fun compareTags(a: String, b: String): Int {
            fun parts(t: String) = t.trimStart('v', 'V').split('.', '-').map { it.toIntOrNull() ?: 0 }
            val x = parts(a)
            val y = parts(b)
            for (i in 0 until maxOf(x.size, y.size)) {
                val c = (x.getOrElse(i) { 0 }).compareTo(y.getOrElse(i) { 0 })
                if (c != 0) return c
            }
            return 0
        }
    }
}
