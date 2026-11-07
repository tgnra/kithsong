package ng.kithso.android.ui

import ng.kithso.android.data.CollectionRef
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BackStackTest {
    private val album = Page.Collection(CollectionRef(CollectionRef.Kind.Album, "a:1"))
    private val albumInGroup = Page.Collection(CollectionRef(CollectionRef.Kind.Album, "a:1", "g:1"))

    // Back as a layer takes it: the top goes on sliding out, off the stack.
    private fun <T> BackStack<T>.back() = leave(top!!.id)

    @Test
    fun backGoesDownAPageAtATime() {
        var stack = BackStack.of(listOf(Page.Settings, Page.Licenses, Page.License("mit")))
        stack = stack.back()
        assertEquals(Page.Licenses, stack.top?.page)
        stack = stack.back()
        assertEquals(Page.Settings, stack.top?.page)
        stack = stack.back()
        assertNull(stack.top)
    }

    @Test
    fun aSecondPressWhileTheFirstSlidesOutIsForThePageBeneath() {
        val stack = BackStack.of(listOf(Page.Search, album)).back().back()
        // Both are still drawn, and neither can come back: only [remove] touches them now.
        assertEquals(2, stack.layers.size)
        assertTrue(stack.layers.all { it.leaving })
        assertNull(stack.top)
        assertEquals(emptyList<Page>(), stack.pages)
    }

    @Test
    fun aLayerSlidOutGoesWithoutDisturbingTheOthers() {
        val stack = BackStack.of(listOf(Page.Search, album))
        val leaving = stack.top!!.id
        val after = stack.leave(leaving).push(Page.Settings).remove(leaving)
        assertEquals(listOf(Page.Search, Page.Settings), after.pages)
    }

    @Test
    fun removingALayerTwiceIsHarmless() {
        val stack = BackStack.of(listOf(Page.Search, album))
        val top = stack.top!!.id
        assertEquals(listOf<Page>(Page.Search), stack.remove(top).remove(top).pages)
    }

    @Test
    fun theSamePageOnTopOpensOnce() {
        val stack = BackStack<Page>().push(Page.Settings).push(Page.Settings)
        assertEquals(1, stack.layers.size)
    }

    @Test
    fun aPageOpenedAgainWhileItSlidesOutIsANewLayer() {
        val first = BackStack<Page>().push(Page.Player)
        val again = first.back().push(Page.Player)
        assertEquals(2, again.layers.size)
        assertTrue(again.layers[0].id != again.layers[1].id)
        assertEquals(again.layers[1].id, again.top?.id)
    }

    @Test
    fun keepLeavesTheLowestPages() {
        val stack = BackStack.of(listOf(1, 2, 3, 4)).keep(2)
        assertEquals(listOf(1, 2), stack.pages)
        assertEquals(2, stack.layers.size)
    }

    @Test
    fun everyPageSurvivesSaving() {
        val pages = listOf(
            albumInGroup,
            Page.Settings, Page.Licenses, Page.License("licenses/a:b.txt"), Page.Sync, Page.RecycleBin,
            Page.Device("uid:1"), Page.Search, album, Page.Player,
        )
        for (page in pages) assertEquals(page, Page.decode(Page.encode(page)))
    }

    private val otherAlbum = Page.Collection(CollectionRef(CollectionRef.Kind.Album, "a:2"))

    @Test
    fun aSecondAlbumFromHomeReplacesThePageSoBackGoesHomeInOnePress() {
        val stack = BackStack<Page>().besideHome(album, shifted = true).besideHome(otherAlbum, shifted = true)
        assertEquals(listOf<Page>(otherAlbum), stack.pages)
        assertTrue(stack.shifted)
        assertNull(stack.back().top)
    }

    @Test
    fun replacingTakesThePagesStackedInTheRightPaneToo() {
        val stack = BackStack<Page>().besideHome(album).push(Page.Search).besideHome(otherAlbum)
        assertEquals(listOf<Page>(otherAlbum), stack.pages)
    }

    @Test
    fun aPageFromAPageStillStacks() {
        val stack = BackStack<Page>().besideHome(album, shifted = true).push(otherAlbum)
        assertEquals(listOf<Page>(album, otherAlbum), stack.pages)
        assertTrue(stack.shifted)
    }

    @Test
    fun aReplacedPageStaysDrawnUntilTheNewOneCoversIt() {
        var stack = BackStack<Page>().besideHome(album).besideHome(otherAlbum)
        assertEquals(2, stack.layers.size)
        stack = stack.settle(stack.top!!.id)
        assertEquals(listOf<Page>(otherAlbum), stack.layers.map { it.page })
    }

    @Test
    fun aPageDismissedBeforeItArrivedTakesWhatItReplacedWithIt() {
        val stack = BackStack<Page>().besideHome(album).besideHome(otherAlbum)
        val id = stack.top!!.id
        assertTrue(stack.settle(id).remove(id).layers.isEmpty())
    }

    @Test
    fun settlingKeepsWhatsOpenAboveAndBeneath() {
        val stack = BackStack.of(listOf(Page.Settings, Page.Licenses))
        assertEquals(stack, stack.settle(stack.top!!.id))
    }

    @Test
    fun theSamePageFromHomeOpensOnce() {
        val stack = BackStack<Page>().besideHome(album, shifted = true)
        assertEquals(stack, stack.besideHome(album, shifted = true))
    }

    @Test
    fun theTopPageFromHomeDropsThoseBeneath() {
        val stack = BackStack<Page>().besideHome(Page.Search).push(album)
        val id = stack.top!!.id
        val again = stack.besideHome(album)
        assertEquals(listOf<Page>(album), again.pages)
        assertEquals(id, again.top?.id)
    }

    @Test
    fun homeIsShiftedOnlyWhileAShiftedPageIsOpen() {
        val stack = BackStack<Page>().besideHome(album, shifted = true)
        assertTrue(stack.shifted)
        assertFalse(stack.back().shifted)
        assertFalse(BackStack<Page>().besideHome(album).shifted)
    }

    @Test
    fun whetherHomeIsShiftedSurvivesARotation() {
        val saver = BackStack.saver(Page::encode, Page::decode)
        val stack = BackStack<Page>().besideHome(album, shifted = true).push(Page.Player)
        val scope = androidx.compose.runtime.saveable.SaverScope { true }
        val saved = with(saver) { scope.save(stack) }!!
        @Suppress("UNCHECKED_CAST")
        val restored = (saver as androidx.compose.runtime.saveable.Saver<BackStack<Page>, Any>).restore(saved)!!
        assertEquals(stack.layers, restored.layers)
    }
}
