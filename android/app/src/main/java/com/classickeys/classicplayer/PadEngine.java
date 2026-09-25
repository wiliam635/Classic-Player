package com.classickeys.classicplayer;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.audiofx.Equalizer;
import android.os.Handler;
import android.os.Looper;
import java.io.File;

/** Streams long pad audio and performs the desktop-style continuous-pad crossfade. */
final class PadEngine {
    private final Context context;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final MediaPlayer[] players=new MediaPlayer[12];
    private final Equalizer[] equalizers=new Equalizer[12];
    private final String[] paths=new String[12];
    private final float[] levels=new float[12];
    private final int[] fadeGeneration=new int[12];
    private boolean continuous;
    private int active=-1;
    private float layerGain=1f,masterGain=1f,pan=0f;
    private long fadeMs=1000;
    private float lowDb,midDb,highDb,lowHz=220,midHz=1200,highHz=4200,lowQ=.707f,midQ=1f,highQ=.707f,highPassHz=20,lowPassHz=20000;

    PadEngine(Context context){this.context=context.getApplicationContext();}
    void setContinuous(boolean value){continuous=value;}
    void load(int pad,String path){if(pad<0||pad>=12)return;release(pad);paths[pad]=path;}
    boolean loaded(int pad){return pad>=0&&pad<12&&paths[pad]!=null&&new File(paths[pad]).isFile();}
    String name(int pad){return loaded(pad)?new File(paths[pad]).getName():"VAZIO";}
    void setFadeSeconds(float seconds){fadeMs=Math.max(20,Math.min(10000,(long)(seconds*1000)));}
    float fadeSeconds(){return fadeMs/1000f;}
    void setGain(float gain){layerGain=clamp(gain);main.post(this::refreshVolumes);}
    void setMaster(float gain){masterGain=clamp(gain);main.post(this::refreshVolumes);}
    void setPan(float value){pan=Math.max(-1,Math.min(1,value));main.post(this::refreshVolumes);}
    void setEq(float low,float mid,float high,float lowFrequency,float midFrequency,float highFrequency,float lowBandwidth,float midBandwidth,float highBandwidth,float highPass,float lowPass){
        lowDb=low;midDb=mid;highDb=high;lowHz=lowFrequency;midHz=midFrequency;highHz=highFrequency;lowQ=lowBandwidth;midQ=midBandwidth;highQ=highBandwidth;highPassHz=highPass;lowPassHz=lowPass;
        main.post(()->{for(int i=0;i<equalizers.length;i++)applyEq(i);});
    }

    void trigger(int pad){if(pad<0||pad>=12)return;main.post(()->triggerOnMain(pad));}
    private void triggerOnMain(int pad){
        if(!loaded(pad))return;
        if(continuous){
            if(active==pad&&players[pad]!=null)return;
            for(int i=0;i<players.length;i++)if(i!=pad&&players[i]!=null)fadeTo(i,0f,fadeMs,true);
        }else release(pad);
        try{
            MediaPlayer player=new MediaPlayer();player.setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build());
            player.setDataSource(paths[pad]);player.setLooping(continuous);player.prepare();player.setOnCompletionListener(done->main.post(()->{if(players[pad]==done)release(pad);}));
            players[pad]=player;active=pad;levels[pad]=continuous?0f:1f;attachEq(pad,player);setPlayerVolume(pad);player.start();
            if(continuous)fadeTo(pad,1f,fadeMs,false);
        }catch(Exception ignored){release(pad);}
    }

    void stopAll(){main.post(()->{
        if(continuous){for(int i=0;i<players.length;i++)if(players[i]!=null)fadeTo(i,0f,fadeMs,true);}
        else for(int i=0;i<players.length;i++)release(i);
        active=-1;
    });}
    void stopImmediately(){main.post(()->{for(int i=0;i<players.length;i++)release(i);active=-1;});}

    private void fadeTo(int pad,float target,long duration,boolean releaseWhenDone){
        MediaPlayer player=players[pad];if(player==null)return;
        final int generation=++fadeGeneration[pad];final float start=levels[pad];final long began=android.os.SystemClock.uptimeMillis();final long span=Math.max(20,duration);
        Runnable step=new Runnable(){@Override public void run(){
            if(players[pad]!=player||fadeGeneration[pad]!=generation)return;
            float progress=Math.min(1f,(android.os.SystemClock.uptimeMillis()-began)/(float)span);
            // Equal-power curves are perceptually smoother when crossfading pads.
            float shaped=target>=start?(float)Math.sin(progress*Math.PI*.5):(float)(1-Math.cos(progress*Math.PI*.5));
            levels[pad]=start+(target-start)*shaped;setPlayerVolume(pad);
            if(progress>=1f){levels[pad]=target;if(releaseWhenDone)release(pad);else setPlayerVolume(pad);}
            else main.postDelayed(this,16);
        }};main.post(step);
    }
    private void refreshVolumes(){for(int i=0;i<players.length;i++)if(players[i]!=null)setPlayerVolume(i);}
    private void attachEq(int pad,MediaPlayer player){
        try{Equalizer eq=new Equalizer(0,player.getAudioSessionId());eq.setEnabled(true);equalizers[pad]=eq;applyEq(pad);}catch(RuntimeException ignored){equalizers[pad]=null;}
    }
    private void applyEq(int pad){
        Equalizer eq=equalizers[pad];if(eq==null)return;
        try{
            short[] range=eq.getBandLevelRange();short bands=eq.getNumberOfBands();
            for(short band=0;band<bands;band++){
                double hz=Math.max(20,eq.getCenterFreq(band)/1000.0);double octave=Math.log(hz/20.0)/Math.log(2.0);
                double gain=bell(octave,lowHz,lowDb,lowQ)+bell(octave,midHz,midDb,midQ)+bell(octave,highHz,highDb,highQ);
                if(hz<highPassHz)gain-=Math.min(24,24*Math.log(highPassHz/hz)/Math.log(2.0));
                if(hz>lowPassHz)gain-=Math.min(24,24*Math.log(hz/lowPassHz)/Math.log(2.0));
                short millibels=(short)Math.max(range[0],Math.min(range[1],Math.round(gain*100)));
                eq.setBandLevel(band,millibels);
            }
        }catch(RuntimeException ignored){}
    }
    private static double bell(double octave,float centerHz,float gainDb,float q){double center=Math.log(Math.max(20,centerHz)/20.0)/Math.log(2.0);double width=Math.max(.16,1.0/Math.max(.1,q));double delta=(octave-center)/width;return gainDb*Math.exp(-.5*delta*delta);}
    private void setPlayerVolume(int pad){
        MediaPlayer player=players[pad];if(player==null)return;
        float value=clamp(levels[pad]*layerGain*masterGain);float left=value,right=value;
        if(pan>0)left*=1-pan;else if(pan<0)right*=1+pan;
        try{player.setVolume(left,right);}catch(IllegalStateException ignored){}
    }
    private void release(int pad){
        fadeGeneration[pad]++;MediaPlayer player=players[pad];players[pad]=null;levels[pad]=0;Equalizer eq=equalizers[pad];equalizers[pad]=null;if(eq!=null){try{eq.setEnabled(false);}catch(RuntimeException ignored){}try{eq.release();}catch(RuntimeException ignored){}}
        if(active==pad)active=-1;
        if(player!=null){try{player.stop();}catch(Exception ignored){}try{player.release();}catch(Exception ignored){}}
    }
    private static float clamp(float value){return Math.max(0f,Math.min(1f,value));}
}
