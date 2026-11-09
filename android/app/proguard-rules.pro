# The app's own R8 rules, for release builds (see build.gradle.kts).

# Google Cast finds its options through the class named in the manifest's meta-data
# (com.google.android.gms.cast.framework.OPTIONS_PROVIDER_CLASS_NAME) and makes one by
# reflection. The Cast library's own rule keeps public implementations of OptionsProvider and
# their methods, but not explicitly the constructor, so this keeps both, whatever it says.
-keep class ng.kithso.android.cast.CastOptionsProvider {
    public <init>();
    public <methods>;
}
