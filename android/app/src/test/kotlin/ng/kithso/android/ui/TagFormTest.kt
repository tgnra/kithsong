package ng.kithso.android.ui

import ng.kithso.android.data.TagTarget
import ng.kithso.android.data.TagValues
import ng.kithso.core.AlbumFlags
import ng.kithso.core.AlbumTags
import ng.kithso.core.ArtistTags
import ng.kithso.core.TrackTags
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

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
        val kept = TagField.entries.map { form[it] } + form.noScrobble.toString() + form.mix.toString()
        val again = TagForm(form.target, kept)
        again.start(album)
        assertEquals("Kid A", again[TagField.Title])
        assertTrue(again.mix)
        assertTrue(again.changed)
    }
}
