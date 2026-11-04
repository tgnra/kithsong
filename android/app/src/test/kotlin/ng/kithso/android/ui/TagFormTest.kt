package ng.kithso.android.ui

import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.TagTarget
import ng.kithso.android.data.TagValues
import ng.kithso.android.data.readAtMost
import ng.kithso.core.AlbumFlags
import ng.kithso.core.AlbumTags
import ng.kithso.core.ArtistTags
import ng.kithso.core.TrackTags
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File

/** The tag editor's form on its own, off the device: what counts as a change, and what's kept through a rotation. */
class TagFormTest {
    private val album = TagValues.Album(AlbumTags("OK Computer", "Radiohead", 1997, 0, ""), AlbumFlags(false, false))
    private val track = TagValues.Track(TrackTags("Airbag", "", 1, 0, 1997, "Rock"))

    private fun form(values: TagValues, kind: TagTarget.Kind) = TagForm(TagTarget(kind, "uid")).apply { start(values) }

    @Test
    fun readInIsNoChange() {
        assertFalse(form(album, TagTarget.Kind.Album).changed)
        assertFalse(form(track, TagTarget.Kind.Track).changed)
        assertFalse(form(TagValues.Artist(ArtistTags("Radiohead", "")), TagTarget.Kind.Artist).changed)
    }

    @Test
    fun beforeItsReadNothingCanChange() {
        val form = TagForm(TagTarget(TagTarget.Kind.Album, "uid"))
        form[TagField.Title] = "typed"
        assertFalse(form.changed)
        assertFalse(form.editable)
        assertNull(form.values())
    }

    @Test
    fun anUnknownNumberShowsEmpty() {
        val form = form(track, TagTarget.Kind.Track)
        assertEquals("1", form[TagField.TrackNo])
        assertEquals("", form[TagField.DiscNo])
    }

    @Test
    fun numbersCompareAsNumbers() {
        val form = form(track, TagTarget.Kind.Track)
        form[TagField.TrackNo] = "001"
        form[TagField.DiscNo] = "0"
        assertFalse(form.changed)
        form[TagField.TrackNo] = "2"
        assertTrue(form.changed)
        assertEquals(2, (form.values() as TagValues.Track).tags.trackNo)
    }

    @Test
    fun aFlagIsAChange() {
        val form = form(album, TagTarget.Kind.Album)
        form.mix = true
        assertTrue(form.changed)
        form.mix = false
        assertFalse(form.changed)
    }

    @Test
    fun aReleaseFillsTheFormForSaveToKeep() {
        val form = form(album, TagTarget.Kind.Album)
        form.fill(AlbumTags("OK Computer OKNOTOK", "Radiohead", 2017, 1997, "mbid"))
        assertTrue(form.changed)
        val tags = (form.values() as TagValues.Album).tags
        assertEquals("mbid", tags.mbAlbumId)
        assertEquals(1997, tags.originalYear)
        assertEquals(album, form.saved)
    }

    @Test
    fun whatsSavedIsNoLongerAChange() {
        val form = form(album, TagTarget.Kind.Album)
        form[TagField.Title] = "Kid A"
        form.noScrobble = true
        val now = form.values() as TagValues.Album
        form.saved = album.copy(tags = now.tags) // the flags failed
        assertTrue(form.changed)
        form.saved = now
        assertFalse(form.changed)
    }

    @Test
    fun whatsTypedOutlivesARotation() {
        val form = form(album, TagTarget.Kind.Album)
        form[TagField.Title] = "Kid A"
        form.mix = true
        val again = TagForm(form.target, form.kept())
        again.start(album)
        assertEquals("Kid A", again[TagField.Title])
        assertTrue(again.mix)
        assertTrue(again.changed)
    }

    private val cover = ArtRef("uid", "", "/cache/cover1", 5)

    @Test
    fun aNewCoverIsAChangeUntilItsGone() {
        val form = form(album, TagTarget.Kind.Album)
        form.newArt = cover
        assertTrue(form.changed)
        assertEquals(cover, (form.values() as TagValues.Album).newArt)
        form.newArt = null
        assertFalse(form.changed)
    }

    @Test
    fun theCoverAlbumHasIsNoChange() {
        val values = album.copy(art = ArtRef("uid", "OK Computer", "/lib/cover.jpg", 9), canSetArt = true)
        assertFalse(form(values, TagTarget.Kind.Album).changed)
    }

    @Test
    fun tagsSavedBeforeTheCoverFailedLeaveTheCoverToTry() {
        val form = form(album, TagTarget.Kind.Album)
        form[TagField.Title] = "Kid A"
        form.newArt = cover
        val now = form.values() as TagValues.Album
        form.saved = album.copy(tags = now.tags)
        assertTrue(form.changed)
        assertEquals(now.tags, (form.saved as TagValues.Album).tags)
        form.saved = now
        assertFalse(form.changed)
    }

    @Test
    fun aNewCoverOutlivesARotation() {
        val file = File.createTempFile("cover", null)
        try {
            val form = form(album, TagTarget.Kind.Album)
            form.newArt = ArtRef("uid", "", file.path, file.lastModified())
            val again = TagForm(form.target, form.kept())
            again.start(album)
            assertEquals(file.path, again.newArt?.file)
            assertTrue(again.changed)
        } finally {
            file.delete()
        }
    }

    @Test
    fun aNewCoverGoneFromTheCacheIsForgotten() {
        val form = form(album, TagTarget.Kind.Album)
        form.newArt = ArtRef("uid", "", File("no/such/cover").path, 1)
        val again = TagForm(form.target, form.kept())
        again.start(album)
        assertNull(again.newArt)
        assertFalse(again.changed)
    }

    @Test
    fun aSinglesMixIsAChange() {
        val single = track.copy(canSetMix = true, mix = true)
        val form = form(single, TagTarget.Kind.Track)
        assertTrue(form.mix)
        assertFalse(form.changed)
        form.mix = false
        assertTrue(form.changed)
        val again = TagForm(form.target, form.kept())
        again.start(single)
        assertFalse(again.mix)
        assertTrue(again.changed)
    }

    @Test
    fun whileACoverIsReadTheFormWaits() {
        val form = form(album, TagTarget.Kind.Album)
        form.reading = true
        assertFalse(form.editable)
    }

    @Test
    fun aCoverIsReadUpToItsLimit() {
        val image = ByteArray(200_000) { it.toByte() }
        assertEquals(image.size, readAtMost(ByteArrayInputStream(image), image.size)?.size)
        assertNull(readAtMost(ByteArrayInputStream(image), image.size - 1))
        assertEquals(0, readAtMost(ByteArrayInputStream(ByteArray(0)), 10)?.size)
    }
}
