package ng.kithso.core

/** How far an export or restore has got, in bytes; returns true to stop it. */
fun interface ArchiveProgress {
    fun onProgress(done: Long, total: Long): Boolean
}

/**
 * How an export or restore ended: [status] is an mc_status (0 for done, [CANCELLED] if
 * stopped), [problem] an mc_archive_problem saying why it failed. [tooBig]: the files a restore
 * left out as 2 GiB or more, which a 32-bit phone can't play; their tracks have no file here.
 */
data class ArchiveOutcome(val status: Int, val problem: Int, val tooBig: Int = 0) {
    val ok: Boolean get() = status == 0
    val cancelled: Boolean get() = status == CANCELLED

    companion object {
        const val CANCELLED = -16

        // mc_archive_problem
        const val NO_SPACE = 1
        const val TOO_BIG = 2
        const val WRITE = 3
        const val READ = 4
        const val NOT_EMPTY = 5
        const val NEWER = 6
        const val BAD = 7
    }
}

/**
 * The whole library as one zip, with library.db, and back (kithcore/library_archive.h). Each
 * call runs for as long as the export or restore takes, on the caller's thread.
 */
object LibraryArchive {
    init {
        System.loadLibrary("kithsong_jni")
    }

    /**
     * Writes [library] as a zip to [fd], which stays the caller's to close; [tempDir] holds a
     * copy of library.db meanwhile. On failure what was written is incomplete.
     */
    fun export(library: CoreLibrary, tempDir: String, fd: Int, progress: ArchiveProgress): ArchiveOutcome =
        outcome(nativeExport(library.handle, tempDir, fd, progress))

    /**
     * Restores the export at [zipPath] into [library], which has no tracks, as the whole
     * library (mc_library_restore). Other connections to it should be opened again after.
     */
    fun restore(library: CoreLibrary, zipPath: String, tempDir: String, progress: ArchiveProgress): ArchiveOutcome =
        outcome(nativeRestore(library.handle, zipPath, tempDir, progress))

    /** The bytes of music and covers in the export at [zipPath], or null if it isn't one. */
    fun exportBytes(zipPath: String): Long? = nativeInfo(zipPath)?.get(1)

    private fun outcome(values: IntArray?): ArchiveOutcome =
        values?.let { ArchiveOutcome(it[0], it[1], it[2]) } ?: throw OutOfMemoryError()

    @JvmStatic private external fun nativeExport(handle: Long, tempDir: String, fd: Int, listener: ArchiveProgress): IntArray?
    @JvmStatic private external fun nativeRestore(handle: Long, zipPath: String, tempDir: String, listener: ArchiveProgress): IntArray?
    @JvmStatic private external fun nativeInfo(zipPath: String): LongArray?
}
