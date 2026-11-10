package ng.kithso.android.screenshots

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import ng.kithso.android.data.CoreLibrarySource
import ng.kithso.android.data.ProfileChoices
import ng.kithso.android.importer.ImportEntry
import ng.kithso.android.importer.ImportProgress
import ng.kithso.android.importer.Importer
import ng.kithso.android.importer.ItemKey
import ng.kithso.android.importer.ReviewDetails
import ng.kithso.android.sync.SyncProgress
import ng.kithso.android.sync.SyncStatus
import ng.kithso.android.sync.Syncer
import ng.kithso.core.ConfirmState
import ng.kithso.core.CoreImport
import ng.kithso.core.CoreSync
import ng.kithso.core.DeviceKind
import ng.kithso.core.KithCoreException
import ng.kithso.core.ProfileItem
import ng.kithso.core.ProfileItemKind
import ng.kithso.core.SyncDevice
import ng.kithso.core.SyncEstimate
import ng.kithso.core.SyncProfile
import java.io.File

/**
 * The real [Importer], showing [shown] and [StoreImport.details] in place of the core's: an
 * import stopped part way. Nothing it's asked to do reaches the core: no session was started.
 */
class StoreImporter(context: Context, source: CoreLibrarySource, scope: CoroutineScope, shown: ImportProgress?) :
    Importer(context, source, scope) {
    override val progress: StateFlow<ImportProgress?> = MutableStateFlow(shown)
    override suspend fun details(key: ItemKey): ReviewDetails? = StoreImport.details.takeIf { it.key == key }
}

/**
 * The real [Syncer], showing [shown] and [StoreSync]'s profiles and estimates in place of the
 * core's: a sync stopped part way. It's never held, so it never opens the core.
 */
class StoreSyncer(
    context: Context,
    source: CoreLibrarySource,
    configDir: File,
    scope: CoroutineScope,
    shown: SyncStatus,
) : Syncer(context, source, configDir, shown.deviceName, scope) {
    override val status: StateFlow<SyncStatus> = MutableStateFlow(shown)
    override suspend fun profile(uid: String): SyncProfile = StoreSync.profiles[uid] ?: throw KithCoreException(CoreSync.ERR_NOT_FOUND)
    override suspend fun estimate(uid: String, profile: SyncProfile): SyncEstimate =
        StoreSync.estimates[uid] ?: throw KithCoreException(CoreSync.ERR_NOT_FOUND)
    override suspend fun profileChoices(): ProfileChoices = throw KithCoreException(CoreSync.ERR_NOT_FOUND)
}

/**
 * An import of four made-up album folders, part way: two imported, one being matched, and one
 * waiting for a decision between three candidates on MusicBrainz. The names are made up, as the
 * store library's are, and meant to be absent from MusicBrainz.
 */
object StoreImport {
    private const val ARTIST = "Wren Calloway"
    private const val ALBUM = "Lanterns Over Lowtide"
    val reviewKey = ItemKey(1, 0)

    val progress = ImportProgress(
        phase = ImportProgress.Phase.Matching,
        batch = 2,
        batches = 4,
        filesFound = 41,
        items = 4,
        imported = 2,
        review = 1,
        entries = listOf(
            ImportEntry(reviewKey, "$ARTIST – $ALBUM", CoreImport.State.Review, null),
            ImportEntry(ItemKey(2, 0), "Harrow & Finch – Saltmarsh Hymns", CoreImport.State.Matching, null),
            ImportEntry(ItemKey(0, 0), "Juniper Hale – Quiet Machines", CoreImport.State.Done, null),
            ImportEntry(ItemKey(0, 1), "The Lowland Signal – Northbound", CoreImport.State.Done, null),
        ),
    )

    private val titles = listOf(
        "Tidewater", "Lanterns Over Lowtide", "The Ferryman's Daughter", "Shingle", "Low Light, High Water",
        "Gull Song", "Breakwater", "A Map of Small Harbours", "Moorings", "Slack Tide",
    )
    private val seconds = listOf(214, 247, 198, 176, 263, 151, 229, 305, 187, 242)

    private val files = titles.mapIndexed { i, t ->
        CoreImport.ItemFile("$ARTIST/$ALBUM/%02d %s.flac".format(i + 1, t), t, ARTIST, i + 1, 1, seconds[i] * 1000 + 400)
    }

    private fun tracks(extra: List<String> = emptyList()) =
        titles.mapIndexed { i, t -> CoreImport.MatchTrack(t, ARTIST, 1, i + 1, seconds[i] * 1000, i) } +
            extra.mapIndexed { i, t -> CoreImport.MatchTrack(t, ARTIST, 1, titles.size + i + 1, (201 + i * 17) * 1000, -1) }

    val details = ReviewDetails(
        key = reviewKey,
        name = "$ARTIST – $ALBUM",
        info = CoreImport.ItemInfo(
            CoreImport.Kind.Album, CoreImport.State.Review, files.size, ARTIST, ALBUM, "$ARTIST/$ALBUM",
            error = "Several releases match about as well: choose one.",
            searchedWider = false, outcome = CoreImport.Outcome.None, chosen = -1, matchCount = 3,
        ),
        matches = listOf(
            CoreImport.Match(
                ALBUM, ARTIST, ALBUM, 2018, 0.86, 10, "CD", "GB", "Driftwood Records", false, "",
                releaseType = "Album", catalog = "DRIFT 014", tracks = tracks(),
            ),
            CoreImport.Match(
                "$ALBUM (Deluxe Edition)", ARTIST, "$ALBUM (Deluxe Edition)", 2019, 0.81, 13, "Digital Media", "XW",
                "Driftwood Records", false, "", releaseType = "Album",
                tracks = tracks(listOf("Tidewater (Demo)", "Shingle (Live at the Pier)", "Undertow")),
            ),
            CoreImport.Match(
                ALBUM, ARTIST, ALBUM, 2018, 0.74, 10, "12\" Vinyl", "GB", "Driftwood Records", false, "",
                releaseType = "Album", catalog = "DRIFT 014LP", tracks = tracks(),
            ),
        ),
        files = files,
        related = emptyList(),
        folder = "Music/$ARTIST/$ALBUM",
    )
}

/**
 * A sync group of this device, "Bob's laptop" (the archive), "Bob's PC" and Bob's watch,
 * part way through a session with Bob's laptop that brings this device the files its selection wants:
 * three albums, an artist and a playlist, within a 16 GB budget.
 */
object StoreSync {
    private const val SELF = "self"
    private const val DESK = "desk"
    /** The shots' "now", 9 November 2026 at 18:40 UTC: their times are the same on every run (LocalNow). */
    const val NOW = 1_794_249_600_000L
    private const val now = NOW

    private fun device(
        uid: String,
        name: String,
        kind: DeviceKind,
        self: Boolean = false,
        online: Boolean = true,
        archive: Boolean = false,
        address: String? = null,
        lastSyncMs: Long = 0,
        expected: Long = 0,
        ok: Long = 0,
        bytes: Long = 0,
    ) = SyncDevice(
        uid, name, kind.ordinal, self, online, archive, removed = false, address = address, port = CoreSync.DEFAULT_PORT,
        lastSyncMs = lastSyncMs, autoSyncAfterMs = 0, hasStatus = expected > 0, verifiedMs = lastSyncMs, expected = expected,
        ok = ok, missing = 0, corrupt = 0, bytes = bytes, confirmCode = ConfirmState.Confirmed.ordinal,
    )

    /** Where it isn't in a group: as the tablet shots always were. */
    fun alone(deviceName: String) = SyncStatus(deviceName, running = true, address = "192.168.1.34", port = CoreSync.DEFAULT_PORT)

    fun syncing(deviceName: String) = SyncStatus(
        deviceName = deviceName,
        running = true,
        address = "192.168.1.34",
        port = CoreSync.DEFAULT_PORT,
        libraryState = CoreSync.LibraryState.Group,
        self = device(SELF, deviceName, DeviceKind.Phone, self = true, address = "192.168.1.34", lastSyncMs = now - 26 * 3_600_000L,
            expected = 214, ok = 88, bytes = 2_410_000_000L),
        members = listOf(
            device(DESK, "Bob's laptop", DeviceKind.Desktop, archive = true, address = "192.168.1.20", lastSyncMs = now - 26 * 3_600_000L,
                expected = 1204, ok = 1204, bytes = 38_700_000_000L),
            device("living", "Bob's PC", DeviceKind.Desktop, online = false, address = "192.168.1.27",
                lastSyncMs = now - 3 * 86_400_000L, expected = 655, ok = 655, bytes = 19_200_000_000L),
            device("watch", "Bob's watch", DeviceKind.Watch, address = "192.168.1.41", lastSyncMs = now - 40 * 60_000L,
                expected = 62, ok = 62, bytes = 290_000_000L),
        ),
        selfConfirm = ConfirmState.Confirmed,
        sessions = mapOf(
            DESK to SyncProgress(
                CoreSync.Phase.Files, rowsSent = 214, rowsReceived = 1_840, filesDone = 126, filesTotal = 214,
                bytesReceived = 1_310_000_000L, bytesSent = 0, pace = "8.4 MB/s · about 2 min left",
            ),
        ),
    )

    /** This device's selection: what the device page's profile shows. */
    private val selection = SyncProfile(
        name = null,
        archive = false,
        syncAll = false,
        opusKbps = 0,
        budgetEnabled = true,
        budgetBytes = 16L * 1024 * 1024 * 1024,
        items = listOf(
            ProfileItem(ProfileItemKind.Album, StoreLibrary.COPPERLINE),
            ProfileItem(ProfileItemKind.Album, StoreLibrary.NIGHTJAR),
            ProfileItem(ProfileItemKind.Album, "album-4"),
            ProfileItem(ProfileItemKind.Artist, "artist-1"),
            ProfileItem(ProfileItemKind.Playlist, "pl-0"),
        ),
    )

    val profiles: Map<String, SyncProfile> = mapOf(
        SELF to selection,
        DESK to SyncProfile(null, archive = true, syncAll = true, opusKbps = 0, budgetEnabled = false, budgetBytes = 0, items = emptyList()),
    )

    val estimates: Map<String, SyncEstimate> = mapOf(
        SELF to SyncEstimate(
            tracks = 214, totalBytes = 5_830_000_000L, kbps = 0, archive = false, overBudget = false, shortfallBytes = 0,
            limitBytes = 16L * 1024 * 1024 * 1024, freeBytes = 48L * 1024 * 1024 * 1024, importsDropped = 0,
            stepBytes = listOf(5_830_000_000L, 1_640_000_000L, 1_230_000_000L, 1_030_000_000L, 820_000_000L, 620_000_000L, 410_000_000L),
        ),
        DESK to SyncEstimate(
            tracks = 1204, totalBytes = 38_700_000_000L, kbps = 0, archive = true, overBudget = false, shortfallBytes = 0,
            limitBytes = -1, freeBytes = 1_120_000_000_000L, importsDropped = 0,
            stepBytes = listOf(38_700_000_000L, 9_200_000_000L, 6_900_000_000L, 5_800_000_000L, 4_600_000_000L, 3_500_000_000L, 2_300_000_000L),
        ),
    )
}
