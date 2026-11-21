package ng.kithso.android.ui

import ng.kithso.core.LostTrack
import ng.kithso.core.SyncArchiveOffer
import ng.kithso.core.SyncIssue
import ng.kithso.core.SyncIssueKind
import ng.kithso.core.SyncProblems
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SyncIssuesTest {
    private fun issue(kind: SyncIssueKind, uid: String = "arch", title: String = "Title") =
        SyncIssue(kind.ordinal, uid, "Archive", notice = false, tracks = 3, count = 0, title = title, body = "Body", fix = "Fix")

    private fun offer(uid: String = "arch", canTake: Boolean = true, asked: Boolean = false, button: String = "Make This Device the Archive") =
        SyncArchiveOffer(uid, "Archive", asked, canTake, nobodyCan = !canTake, reinstall = false, unrecoverable = 0, "Make this phone the archive?", "Body", button)

    private val lost = SyncProblems("", 0, 0, listOf(LostTrack("t1", "Song", "Artist")))

    @Test
    fun kindsFollowTheCoresOrder() {
        // mc_sync_issue_kind: MC_ISSUE_LOST is 12, MC_ISSUE_ARCHIVE_LOST 19, MC_ISSUE_NOTICE_BUDGET 23.
        assertEquals(SyncIssueKind.Lost, SyncIssue(12, "", "", false, 0, 0, "", "", "").kind)
        assertEquals(SyncIssueKind.ArchiveLost, SyncIssue(19, "", "", false, 0, 0, "", "", "").kind)
        assertEquals(SyncIssueKind.NoticeBudget, SyncIssue(23, "", "", false, 0, 0, "", "", "").kind)
        assertEquals(SyncIssueKind.Other, SyncIssue(99, "", "", false, 0, 0, "", "", "").kind)
    }

    @Test
    fun lostTracksOfferRemovalWhileThereAreAny() {
        assertEquals(IssueAction.RemoveLost, issueAction(issue(SyncIssueKind.Lost), lost, null))
        assertEquals(IssueAction.None, issueAction(issue(SyncIssueKind.Lost), SyncProblems.NONE, null))
    }

    @Test
    fun aLostArchiveOffersTakingOverOnlyWithRoom() {
        val archiveLost = issue(SyncIssueKind.ArchiveLost)
        assertEquals(IssueAction.TakeOver("arch", "Make This Device the Archive"), issueAction(archiveLost, SyncProblems.NONE, offer()))
        assertEquals(IssueAction.None, issueAction(archiveLost, SyncProblems.NONE, offer(canTake = false, button = "")))
        assertEquals(IssueAction.None, issueAction(archiveLost, SyncProblems.NONE, null))
        // An offer for another device.
        assertEquals(IssueAction.None, issueAction(archiveLost, SyncProblems.NONE, offer(uid = "other")))
        assertEquals(
            IssueAction.TakeOver("arch", "Take Over"),
            issueAction(issue(SyncIssueKind.ArchiveReinstall), SyncProblems.NONE, offer(button = "Take Over")),
        )
    }

    @Test
    fun otherIssuesOnlySayHowToFixThem() {
        for (kind in listOf(SyncIssueKind.DiskFull, SyncIssueKind.Held, SyncIssueKind.ArchiveAway, SyncIssueKind.NoticeArchive)) {
            assertEquals(IssueAction.None, issueAction(issue(kind), lost, offer()))
        }
    }

    @Test
    fun theDialogListsADevicesIssuesAndSkipsWordlessOnes() {
        val issues = listOf(issue(SyncIssueKind.Held, "pixel"), issue(SyncIssueKind.ArchiveAway, "arch"), issue(SyncIssueKind.LowSpace, "pixel", title = ""))
        assertEquals(2, shownIssues(issues).size)
        assertEquals(listOf(issues[0]), shownIssues(issues, "pixel"))
        assertTrue(shownIssues(issues, "nobody").isEmpty())
    }

    @Test
    fun theOfferIsAskedOnce() {
        assertTrue(asksOffer(offer()))
        assertFalse(asksOffer(offer(asked = true)))
        assertFalse(asksOffer(null))
        // Without room it's still asked, to say what to do.
        assertTrue(asksOffer(offer(canTake = false, button = "")))
    }
}
