// AGP 9 compiles Kotlin itself, so there is no org.jetbrains.kotlin.android plugin. The
// Compose compiler plugin's version also sets the Kotlin version AGP uses.
plugins {
    id("com.android.application") version "9.2.1" apply false
    id("com.android.library") version "9.2.1" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.2.21" apply false
}
