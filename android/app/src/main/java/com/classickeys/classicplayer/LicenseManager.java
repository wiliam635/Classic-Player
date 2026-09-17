package com.classickeys.classicplayer;

import android.content.Context;
import android.content.SharedPreferences;

/** Stores the authenticated session and its offline grace period for Android. */
final class LicenseManager {
    private static final long OFFLINE_GRACE_MS = 30L * 24L * 60L * 60L * 1000L;
    private final SharedPreferences prefs;

    LicenseManager(Context context) {
        prefs = context.getApplicationContext().getSharedPreferences("license", Context.MODE_PRIVATE);
    }

    boolean isActivated() { return prefs.getString("access_token", null) != null; }
    boolean isUsableOffline() { return isActivated() && prefs.getLong("offline_until", 0) > System.currentTimeMillis(); }
    String userName() { return prefs.getString("user_name", ""); }
    String userEmail() { return prefs.getString("user_email", ""); }
    String accessToken() { return prefs.getString("access_token", ""); }

    void storeSession(String token, String name, String email) {
        prefs.edit().putString("access_token", token).putString("user_name", name)
                .putString("user_email", email)
                .putLong("offline_until", System.currentTimeMillis() + OFFLINE_GRACE_MS).apply();
    }

    void clear() { prefs.edit().clear().apply(); }
    void refreshOfflineWindow() { prefs.edit().putLong("offline_until", System.currentTimeMillis() + OFFLINE_GRACE_MS).apply(); }
}
