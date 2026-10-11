# Keep the WebView JavaScript bridge methods.
-keepclassmembers class com.xf1rebalx.hearbridgebrowser.PlayerActivity$Bridge {
    @android.webkit.JavascriptInterface <methods>;
}
-keepattributes JavascriptInterface
