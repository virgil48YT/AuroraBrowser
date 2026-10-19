# Aurora Browser — R8 rules for release builds.
# The JavaScript bridge-free design means no @JavascriptInterface classes need keeping.
-keepclassmembers class * extends android.webkit.WebViewClient { *; }
-keepclassmembers class * extends android.webkit.WebChromeClient { *; }
