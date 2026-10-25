package app.musicapp.wear

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.wear.compose.material.MaterialTheme
import androidx.wear.compose.material.Scaffold
import androidx.wear.compose.material.Text
import androidx.wear.compose.material.TimeText
import app.musicapp.NativeCore
import java.io.File

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // The library folder; library.db lives inside it.
        val libraryDir = File(filesDir, "library").absolutePath
        val albums = NativeCore.albumCount(libraryDir)

        setContent {
            MaterialTheme {
                Home(version = NativeCore.version(), albums = albums)
            }
        }
    }
}

@Composable
private fun Home(version: String, albums: Long) {
    Scaffold(timeText = { TimeText() }) {
        Column(
            // Round screens clip the corners; keep text inside the inscribed square.
            modifier = Modifier.fillMaxSize().padding(horizontal = 24.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                text = if (albums >= 0) "$albums albums" else "Could not open the library ($albums)",
                style = MaterialTheme.typography.title3,
                textAlign = TextAlign.Center,
            )
            Text("musiccore $version", style = MaterialTheme.typography.caption2)
        }
    }
}
