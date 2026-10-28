/* Resource ids shared by app.rc and the C sources. */
#ifndef KITHSONG_RESOURCE_H
#define KITHSONG_RESOURCE_H

/* The app icon, version.rc.in. */
#define IDI_APP 1

#define IDD_REVIEW 100
#define IDC_REV_HEADER 1001
#define IDC_REV_MATCHES 1002
#define IDC_REV_TRACKS 1003
#define IDC_REV_ARTIST 1004
#define IDC_REV_TITLE 1005
#define IDC_REV_TITLE_LABEL 1006
#define IDC_REV_SEARCH 1007
#define IDC_REV_ASIS 1008
#define IDC_REV_SKIP 1009
#define IDC_REV_MATCHES_LABEL 1010
#define IDC_REV_TRACKS_LABEL 1011
#define IDC_REV_SEARCH_LABEL 1012
#define IDC_REV_ARTIST_LABEL 1013
#define IDC_REV_TRACKS_HINT 1014
#define IDC_REV_WIDER 1030 /* Search More Widely */

/* The playlist dialogs, playlist_dialogs.c. */
#define IDD_PLAYLIST_NAME 101
#define IDD_PLAYLIST_DELETE 102
#define IDD_LIBRARY_DELETE 103 /* delete_dialog.c, with the playlist dialogs' controls */
#define IDC_PL_LABEL 1101
#define IDC_PL_NAME 1102
#define IDC_PL_HEADING 1103
#define IDC_PL_NOTE 1104

#endif
