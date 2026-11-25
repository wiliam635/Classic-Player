package com.classickeys.classicplayer;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Keeps the process alive while the app is in the background, so audio and the MIDI keyboard keep
 * working when the screen is off or another app is in front. It only shows the notification; the
 * sound engine itself lives in MainActivity.
 */
public final class PlaybackService extends Service {
    private static final String CHANNEL = "classic_player_playback";
    private static final int NOTIFICATION_ID = 41;

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && manager != null)
            manager.createNotificationChannel(new NotificationChannel(CHANNEL, "Classic Player em segundo plano", NotificationManager.IMPORTANCE_LOW));
        Intent open = new Intent(this, MainActivity.class).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        PendingIntent pending = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
        Notification notification = builder.setContentTitle("Classic Player")
                .setContentText("Som e MIDI ativos em segundo plano · toque para abrir")
                .setSmallIcon(R.drawable.classic_keys_samples_logo).setContentIntent(pending).setOngoing(true).build();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        else startForeground(NOTIFICATION_ID, notification);
        return START_NOT_STICKY;
    }

    @Override public IBinder onBind(Intent intent) { return null; }
}
