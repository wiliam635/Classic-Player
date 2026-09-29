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
    private final MediaPlayer[][] players=new MediaPlayer[2][12];
    private final Equalizer[][] equalizers=new Equalizer[2][12];
    private final String[][] paths=new String[2][12];
    private final String[][] names=new String[2][12];
    private final float[][] levels=new float[2][12];
    private final int[][] fadeGeneration=new int[2][12];
    private final boolean[] drumReady=new boolean[12];
    private final boolean[] drumPending=new boolean[12];
    private final boolean[] enabled={true,true};
    private final int[] active={-1,-1};
    private final float[] layerGain={1f,1f},pan={0f,0f};
    private float masterGain=1f;
    private long fadeMs=1000;
    private final float[] lowDb={0,0},midDb={0,0},highDb={0,0},lowHz={220,220},midHz={1200,1200},highHz={4200,4200},lowQ={.707f,.707f},midQ={1,1},highQ={.707f,.707f},highPassHz={20,20},lowPassHz={20000,20000};

    PadEngine(Context context){this.context=context.getApplicationContext();}
    void setEnabled(boolean value){setEnabled(value,false);setEnabled(value,true);}
    void setEnabled(boolean value,boolean continuous){
        int bank=bank(continuous);enabled[bank]=value;
        main.post(()->{
            if(!value){for(int i=0;i<players[bank].length;i++)release(bank,i);active[bank]=-1;}
            else if(!continuous)for(int i=0;i<8;i++)prepareDrum(i);
        });
    }
    private static int bank(boolean continuous){return continuous?1:0;}
    void load(int pad,String path,boolean continuous){load(pad,path,continuous,null);}
    void load(int pad,String path,boolean continuous,String displayName){if(pad<0||pad>=12)return;int bank=bank(continuous);main.post(()->{release(bank,pad);paths[bank][pad]=path;names[bank][pad]=displayName;if(!continuous)prepareDrum(pad);});}
    boolean loaded(int pad,boolean continuous){int bank=bank(continuous);String path=pad>=0&&pad<12?paths[bank][pad]:null;return path!=null&&new File(path).isFile();}
    String name(int pad,boolean continuous){int bank=bank(continuous);if(!loaded(pad,continuous))return "VAZIO";String name=names[bank][pad];return name==null||name.isEmpty()?"PAD "+(pad+1):name;}
    void setFadeSeconds(float seconds){fadeMs=Math.max(20,Math.min(10000,(long)(seconds*1000)));}
    float fadeSeconds(){return fadeMs/1000f;}
    void setGain(float gain,boolean continuous){layerGain[bank(continuous)]=clamp(gain);main.post(this::refreshVolumes);}
    void setMaster(float gain){masterGain=clamp(gain);main.post(this::refreshVolumes);}
    void setPan(float value,boolean continuous){int bank=bank(continuous);pan[bank]=Math.max(-1,Math.min(1,value));main.post(this::refreshVolumes);}
    void setEq(float low,float mid,float high,float lowFrequency,float midFrequency,float highFrequency,float lowBandwidth,float midBandwidth,float highBandwidth,float highPass,float lowPass,boolean continuous){
        int bank=bank(continuous);lowDb[bank]=low;midDb[bank]=mid;highDb[bank]=high;lowHz[bank]=lowFrequency;midHz[bank]=midFrequency;highHz[bank]=highFrequency;lowQ[bank]=lowBandwidth;midQ[bank]=midBandwidth;highQ[bank]=highBandwidth;highPassHz[bank]=highPass;lowPassHz[bank]=lowPass;
        main.post(()->{for(int i=0;i<equalizers[bank].length;i++)applyEq(bank,i);});
    }

    void trigger(int pad,boolean continuous){if(pad<0||pad>=12)return;main.post(()->triggerOnMain(pad,continuous));}
    private void prepareDrum(int pad){
        if(!enabled[0]||pad>=8||players[0][pad]!=null||!loaded(pad,false))return;
        try{
            MediaPlayer player=new MediaPlayer();
            player.setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build());
            player.setDataSource(paths[0][pad]);
            players[0][pad]=player;
            player.setOnPreparedListener(ready->{
                if(players[0][pad]!=ready)return;
                drumReady[pad]=true;attachEq(0,pad,ready);setPlayerVolume(0,pad);
                if(drumPending[pad]){drumPending[pad]=false;playDrum(pad);}
            });
            player.setOnCompletionListener(done->{
                if(players[0][pad]==done){levels[0][pad]=0f;if(active[0]==pad)active[0]=-1;}
            });
            player.setOnErrorListener((failed,what,extra)->{if(players[0][pad]==failed)release(0,pad);return true;});
            player.prepareAsync();
        }catch(Exception ignored){release(0,pad);}
    }
    private void playDrum(int pad){
        MediaPlayer player=players[0][pad];
        if(player==null||!drumReady[pad]){drumPending[pad]=true;prepareDrum(pad);return;}
        try{
            player.seekTo(0);levels[0][pad]=1f;active[0]=pad;setPlayerVolume(0,pad);player.start();
        }catch(RuntimeException ignored){release(0,pad);drumPending[pad]=true;prepareDrum(pad);}
    }
    private void triggerOnMain(int pad,boolean continuous){
        final int bank=bank(continuous);
        final String path=paths[bank][pad];
        if(!enabled[bank]||!loaded(pad,continuous))return;
        if(!continuous){playDrum(pad);return;}
        if(continuous){
            if(active[bank]==pad&&players[bank][pad]!=null)return;
            for(int i=0;i<players[bank].length;i++)if(i!=pad&&players[bank][i]!=null)fadeTo(bank,i,0f,fadeMs,true);
        }
        try{
            MediaPlayer player=new MediaPlayer();player.setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build());
            player.setDataSource(path);player.setLooping(continuous);player.prepare();player.setOnCompletionListener(done->{if(players[bank][pad]==done)release(bank,pad);});
            players[bank][pad]=player;active[bank]=pad;levels[bank][pad]=continuous?0f:1f;attachEq(bank,pad,player);setPlayerVolume(bank,pad);player.start();
            if(continuous)fadeTo(bank,pad,1f,fadeMs,false);
        }catch(Exception ignored){release(bank,pad);}
    }

    void stopAll(boolean continuous){main.post(()->{
        int bank=bank(continuous);
        if(continuous){for(int i=0;i<players[bank].length;i++)if(players[bank][i]!=null)fadeTo(bank,i,0f,fadeMs,true);}
        else for(int i=0;i<players[bank].length;i++)release(bank,i);
        active[bank]=-1;
    });}
    void stopAll(){main.post(()->{for(int bank=0;bank<players.length;bank++){
        if(bank==1){for(int i=0;i<players[bank].length;i++)if(players[bank][i]!=null)fadeTo(bank,i,0f,fadeMs,true);}
        else for(int i=0;i<players[bank].length;i++)release(bank,i);
        active[bank]=-1;
    }});}
    void stopImmediately(boolean continuous){main.post(()->{int bank=bank(continuous);for(int i=0;i<players[bank].length;i++)release(bank,i);active[bank]=-1;});}
    void stopImmediately(){main.post(()->{for(int bank=0;bank<players.length;bank++){for(int i=0;i<players[bank].length;i++)release(bank,i);active[bank]=-1;}});}

    private void fadeTo(int bank,int pad,float target,long duration,boolean releaseWhenDone){
        MediaPlayer player=players[bank][pad];if(player==null)return;
        final int generation=++fadeGeneration[bank][pad];final float start=levels[bank][pad];final long began=android.os.SystemClock.uptimeMillis();final long span=Math.max(20,duration);
        Runnable step=new Runnable(){@Override public void run(){
            if(players[bank][pad]!=player||fadeGeneration[bank][pad]!=generation)return;
            float progress=Math.min(1f,(android.os.SystemClock.uptimeMillis()-began)/(float)span);
            // Equal-power curves are perceptually smoother when crossfading pads.
            float shaped=target>=start?(float)Math.sin(progress*Math.PI*.5):(float)(1-Math.cos(progress*Math.PI*.5));
            levels[bank][pad]=start+(target-start)*shaped;setPlayerVolume(bank,pad);
            if(progress>=1f){levels[bank][pad]=target;if(releaseWhenDone)release(bank,pad);else setPlayerVolume(bank,pad);}
            else main.postDelayed(this,16);
        }};main.post(step);
    }
    private void refreshVolumes(){for(int bank=0;bank<players.length;bank++)for(int i=0;i<players[bank].length;i++)if(players[bank][i]!=null)setPlayerVolume(bank,i);}
    private void attachEq(int bank,int pad,MediaPlayer player){
        try{Equalizer eq=new Equalizer(0,player.getAudioSessionId());eq.setEnabled(true);equalizers[bank][pad]=eq;applyEq(bank,pad);}catch(RuntimeException ignored){equalizers[bank][pad]=null;}
    }
    private void applyEq(int bank,int pad){
        Equalizer eq=equalizers[bank][pad];if(eq==null)return;
        try{
            short[] range=eq.getBandLevelRange();short bands=eq.getNumberOfBands();
            for(short band=0;band<bands;band++){
                double hz=Math.max(20,eq.getCenterFreq(band)/1000.0);double octave=Math.log(hz/20.0)/Math.log(2.0);
                double gain=bell(octave,lowHz[bank],lowDb[bank],lowQ[bank])+bell(octave,midHz[bank],midDb[bank],midQ[bank])+bell(octave,highHz[bank],highDb[bank],highQ[bank]);
                if(hz<highPassHz[bank])gain-=Math.min(24,24*Math.log(highPassHz[bank]/hz)/Math.log(2.0));
                if(hz>lowPassHz[bank])gain-=Math.min(24,24*Math.log(hz/lowPassHz[bank])/Math.log(2.0));
                short millibels=(short)Math.max(range[0],Math.min(range[1],Math.round(gain*100)));
                eq.setBandLevel(band,millibels);
            }
        }catch(RuntimeException ignored){}
    }
    private static double bell(double octave,float centerHz,float gainDb,float q){double center=Math.log(Math.max(20,centerHz)/20.0)/Math.log(2.0);double width=Math.max(.16,1.0/Math.max(.1,q));double delta=(octave-center)/width;return gainDb*Math.exp(-.5*delta*delta);}
    private void setPlayerVolume(int bank,int pad){
        MediaPlayer player=players[bank][pad];if(player==null)return;
        float value=clamp(levels[bank][pad]*layerGain[bank]*masterGain);float left=value,right=value;
        if(pan[bank]>0)left*=1-pan[bank];else if(pan[bank]<0)right*=1+pan[bank];
        try{player.setVolume(left,right);}catch(IllegalStateException ignored){}
    }
    private void release(int bank,int pad){
        fadeGeneration[bank][pad]++;MediaPlayer player=players[bank][pad];players[bank][pad]=null;levels[bank][pad]=0;if(bank==0){drumReady[pad]=false;drumPending[pad]=false;}Equalizer eq=equalizers[bank][pad];equalizers[bank][pad]=null;if(eq!=null){try{eq.setEnabled(false);}catch(RuntimeException ignored){}try{eq.release();}catch(RuntimeException ignored){}}
        if(active[bank]==pad)active[bank]=-1;
        if(player!=null){try{player.stop();}catch(Exception ignored){}try{player.release();}catch(Exception ignored){}}
    }
    private static float clamp(float value){return Math.max(0f,Math.min(1f,value));}
}
