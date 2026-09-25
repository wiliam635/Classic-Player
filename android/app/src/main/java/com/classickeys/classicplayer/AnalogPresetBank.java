package com.classickeys.classicplayer;

/** Factory bank mirrored from Source/AnalogBrowserPresets.h. */
final class AnalogPresetBank {
    static final String[] NAMES = {
        "Solo Lead", "Modern Lead", "Classic Minimoog Lead", "Lucky Man",
        "Unison Lead", "Vintage Lead", "Easy Lead", "Screaming Lead",
        "Waterfall Lead", "Brass Lead", "Taurus Bass", "Funk Bass",
        "Authentic Minimoog Bass", "Thick Bass", "Analog Bass", "Percussive Bass",
        "Dub Bass", "Pulse Bass", "Crystal Pad", "Dream Pad", "Space Mod",
        "Atmospheric Pad", "Warm Pad", "Vintage Minimoog Pad", "Shimmer Pad",
        "Velvet Cloud", "Alien Landscape", "Digital Rain", "Submarine Sonar",
        "Thunder Storm", "Glass Harmonica", "Cosmic Drone"
    };
    // oscillator levels 1..3, semitones 1..3, cutoff Hz, resonance dB,
    // amp ADSR (seconds, seconds, normalized, seconds), LFO Hz, modulation.
    private static final float[][] VALUES = {
        {.66f,.36f,0,0,0,0,2350,.2f,.034f,3.9f,.76f,.048f,4,.25f},
        {.57f,.38f,.28f,0,0,12,6100,2.4f,.006f,.18f,.62f,.09f,5.8f,.38f},
        {.76f,.41f,.13f,0,0,-12,760,12,.013f,.28f,.58f,.1f,4.5f,.16f},
        {.55f,.52f,.12f,-12,-12,0,1450,4.5f,.022f,.55f,.68f,.12f,3.2f,.12f},
        {.62f,.48f,.18f,0,0,12,4100,3.2f,.009f,.24f,.64f,.1f,5.1f,.28f},
        {.72f,.27f,.12f,0,0,-12,1120,5.2f,.025f,.48f,.66f,.14f,3.6f,.11f},
        {.62f,.26f,.19f,0,0,7,2500,4,.015f,.34f,.7f,.13f,2.2f,.34f},
        {.7f,.42f,.15f,0,0,12,3800,11,.004f,.14f,.57f,.07f,6.5f,.42f},
        {.49f,.31f,.28f,0,0,12,2050,2.6f,.03f,.7f,.72f,.2f,1.1f,.46f},
        {.63f,.31f,.14f,0,0,0,1700,7.4f,.012f,.31f,.52f,.09f,4,.14f},
        {.8f,.35f,.15f,-12,-12,-24,380,13,.008f,.25f,.7f,.18f,2,.05f},
        {.62f,.45f,.1f,-12,-12,0,720,6,.004f,.13f,.45f,.1f,3,.08f},
        {.75f,.32f,.09f,-12,-12,-24,460,11,.008f,.22f,.55f,.16f,2.5f,.08f},
        {.7f,.4f,.13f,-12,-12,-24,540,8,.006f,.2f,.69f,.13f,2.2f,.06f},
        {.48f,.46f,.18f,-12,-12,-24,650,5.5f,.01f,.29f,.61f,.17f,1.8f,.05f},
        {.62f,.37f,.11f,-12,-12,-24,980,8.6f,.004f,.09f,.34f,.055f,3,.04f},
        {.52f,.32f,.28f,-24,-24,-12,390,4.2f,.018f,.45f,.74f,.27f,.75f,.09f},
        {.54f,.31f,.17f,-12,-12,0,830,6.8f,.008f,.18f,.54f,.12f,4.8f,.22f},
        {.5f,.33f,.11f,0,0,12,2800,2,.38f,.6f,.72f,1.5f,.33f,.38f},
        {.62f,.36f,.12f,0,0,7,1250,2,.5f,.7f,.78f,1.9f,.23f,.46f},
        {.34f,.52f,.16f,0,0,12,1100,3,.65f,.9f,.7f,2.3f,.15f,.6f},
        {.36f,.44f,.15f,0,0,12,820,2.1f,.9f,1.1f,.76f,3.1f,.12f,.52f},
        {.55f,.34f,.16f,0,0,7,920,1.5f,.56f,.72f,.8f,2.4f,.28f,.31f},
        {.31f,.44f,.09f,0,0,12,1030,3.8f,.48f,.8f,.73f,2.1f,.22f,.48f},
        {.41f,.34f,.24f,0,0,12,3600,1.2f,.42f,.62f,.7f,2.6f,.38f,.28f},
        {.59f,.3f,.13f,0,0,7,1270,1.8f,.72f,1.2f,.78f,3.4f,.18f,.4f},
        {.4f,.27f,.09f,0,0,19,900,5,.7f,1.1f,.65f,2.6f,.12f,.72f},
        {.33f,.4f,.19f,0,0,12,2300,4,.01f,.12f,.25f,.45f,7,.3f},
        {.8f,.25f,.08f,0,0,12,650,12,.01f,.4f,.1f,2.8f,.18f,.18f},
        {.18f,.26f,.33f,-12,-12,0,720,8,.3f,.9f,.55f,3.2f,.14f,.78f},
        {.52f,.24f,.18f,0,0,12,3150,2.8f,.16f,.75f,.62f,2.2f,.42f,.34f},
        {.27f,.43f,.25f,-12,-12,0,1250,4.5f,.18f,1.2f,.83f,4,.09f,.7f}
    };
    private static final int[][] WAVES = {
        {1,0,1},{2,1,1},{1,1,2},{0,1,4},{1,1,2},{0,1,0},{2,0,1},{1,1,2},
        {1,4,0},{1,2,0},{1,2,0},{1,2,0},{1,1,0},{1,1,2},{2,0,1},{2,1,0},
        {4,2,0},{2,2,0},{0,4,1},{0,0,4},{1,0,4},{4,0,1},{0,0,4},{1,0,2},
        {4,0,4},{0,4,0},{4,0,2},{2,4,0},{4,4,0},{2,1,4},{4,0,4},{1,4,0}
    };
    private static final boolean[] MONO = {
        true,true,true,true,true,true,true,true,true,true,true,false,true,true,true,true,
        true,false,false,false,false,false,false,false,false,false,false,false,true,false,false,false
    };

    static void apply(int index,float[] controls,int[] waves,boolean[] enabled,boolean[] pinkNoise,boolean[] mono,float[] osc1Tune) {
        int i=Math.max(0,Math.min(NAMES.length-1,index));float[] v=VALUES[i];
        controls[0]=v[0]*100;controls[1]=v[1]*100;controls[2]=v[2]*100;
        osc1Tune[0]=v[3];controls[3]=v[4];controls[4]=v[5];controls[5]=0;
        controls[6]=(float)(100*Math.log(v[6]/25.0)/Math.log(700.0));
        controls[7]=v[7]*5;controls[8]=0;
        controls[9]=v[8]*1000;controls[10]=v[9]*1000;controls[11]=v[10]*100;controls[12]=v[11]*1000;
        controls[13]=v[12];controls[14]=0;controls[15]=v[13]*100;
        controls[16]=controls[17]=controls[18]=0;
        System.arraycopy(WAVES[i],0,waves,0,3);for(int osc=0;osc<3;osc++)enabled[osc]=true;
        pinkNoise[0]=false;mono[0]=MONO[i];
    }
    private AnalogPresetBank(){}
}
