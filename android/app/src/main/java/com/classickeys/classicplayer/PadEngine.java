package com.classickeys.classicplayer;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import java.io.File;

/** Streams long pad audio from storage instead of decoding it into app memory. */
final class PadEngine {
    private final Context context;
    private final MediaPlayer[] players = new MediaPlayer[12];
    private final String[] paths = new String[12];
    private boolean continuous;
    PadEngine(Context context) { this.context=context.getApplicationContext(); }
    void setContinuous(boolean value){continuous=value;}
    void load(int pad,String path){if(pad<0||pad>=12)return;release(pad);paths[pad]=path;}
    boolean loaded(int pad){return pad>=0&&pad<12&&paths[pad]!=null&&new File(paths[pad]).isFile();}
    String name(int pad){return loaded(pad)?new File(paths[pad]).getName():"VAZIO";}
    void trigger(int pad){
        if(!loaded(pad))return;
        if(continuous)stopAll(); else release(pad);
        try{
            MediaPlayer player=new MediaPlayer();
            player.setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build());
            player.setDataSource(paths[pad]); player.setLooping(continuous); player.prepare();
            player.setOnCompletionListener(done->{if(!continuous)release(pad);}); players[pad]=player;player.start();
        }catch(Exception ignored){release(pad);}
    }
    void stopAll(){for(int i=0;i<players.length;i++)release(i);}
    private void release(int pad){MediaPlayer p=players[pad];players[pad]=null;if(p!=null){try{p.stop();}catch(Exception ignored){}p.release();}}
}
