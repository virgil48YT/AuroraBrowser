# Aurora Browser — R8 rules for release builds.
# The JavaScript bridge-free design means no @JavascriptInterface classes need keeping.
-keepclassmembers class * extends android.webkit.WebViewClient { *; }
-keepclassmembers class * extends android.webkit.WebChromeClient { *; }

# Built-in Tor: libtor.so calls back into TorService through JNI, and jtorctl
# is used reflectively by it; neither may be renamed or stripped.
-keep class org.torproject.jni.** { *; }
-keep class net.freehaven.tor.control.** { *; }
-dontwarn org.torproject.jni.**
-dontwarn net.freehaven.tor.control.**
