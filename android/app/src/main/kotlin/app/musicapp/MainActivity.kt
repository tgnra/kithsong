package app.musicapp

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import app.musicapp.ui.theme.MusicappTheme
import java.io.File

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        // The library folder; library.db lives inside it.
        val libraryDir = File(filesDir, "library").absolutePath
        val albums = NativeCore.albumCount(libraryDir)

        setContent {
            MusicappTheme {
                Home(version = NativeCore.version(), albums = albums)
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Home(version: String, albums: Long) {
    Scaffold(
        topBar = { TopAppBar(title = { Text("musicapp") }) },
    ) { padding ->
        Column(
            modifier = Modifier.fillMaxSize().padding(padding),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                text = if (albums >= 0) "$albums albums" else "Could not open the library ($albums)",
                style = MaterialTheme.typography.titleMedium,
            )
            Text("musiccore $version", style = MaterialTheme.typography.bodySmall)
        }
    }
}
