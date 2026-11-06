package ng.kithso.core

// The Home page's radio stations (musiccore/radio_stations.h), bound in jni_radio.c.

/**
 * mc_radio_station_info: a station, named by [seedTrack], which starts from [startTrack] this time,
 * a track of its own kind drawn afresh with each listing. [artistUid] is the seed's first artist;
 * [name] is what it's called, "Zedd Radio", and [sub] the line beneath, which tells it from any
 * station of the same name. [coverDir] holds the seed's album's cover, if there's one here.
 */
data class StationInfo(
    val seedTrack: String,
    val startTrack: String,
    val artistUid: String?,
    val name: String,
    val sub: String?,
    val seedTitle: String?,
    val coverDir: String?,
)

/**
 * The library's stations as of now, the most played first; none for a library with too little
 * analysed, and one for each kind of music the analysis finds. Every seed has a file here. It
 * reads every analysis result, so it's for a background
 * thread.
 */
fun CoreLibrary.radioStations(): List<StationInfo> = RadioNatives.nativeStations(handle, 0).asList()

/**
 * Whether track [uid] can start a station here (mc_radio_seed_check): it has a file here and a
 * result, and it's not too short, too long or part of a continuous mix.
 */
fun CoreLibrary.radioSeedUsable(uid: String): Boolean = RadioNatives.nativeSeedUsable(handle, uid)

/** Track [uid], whether or not it has a file here; null if the library has no such track. */
fun CoreLibrary.track(uid: String): TrackInfo? = RadioNatives.nativeTrack(handle, uid).firstOrNull()

internal object RadioNatives {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativeStations(handle: Long, nowMs: Long): Array<StationInfo>
    @JvmStatic external fun nativeSeedUsable(handle: Long, uid: String): Boolean
    @JvmStatic external fun nativeTrack(handle: Long, uid: String): Array<TrackInfo>
}
