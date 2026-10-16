package com.classickeys.classicplayer;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.LinearGradient;
import android.graphics.Shader;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.view.ViewConfiguration;
import android.widget.*;
import java.util.Locale;

/** Touch-sized version of the desktop editor's charcoal panels and teal knobs. */
final class EditorUi {
    // Shared default palette from the desktop editor; touch targets stay larger.
    static final String[] THEME_NAMES={"PADRÃO","PRETO","VERMELHO","AZUL ROXO","BRANCO AZUL","PRATEADO","AÇO ESCURO"};
    // Keep the seven desktop palettes in the same order so a theme selected
    // on either platform has the same visual identity.
    private static final int[][] PALETTES={
            {0xff091018,0xff151f28,0xff202c35,0xff33414c,0xff13b8ad,0xffffd84a,0xffedf4f7,0xff9eabb5,0xff0b151e},
            {0xff050608,0xff101216,0xff1c2025,0xff565e66,0xffbec5cc,0xfff3f5f6,0xfff6f7f8,0xffb1b7bd,0xff090b0e},
            {0xff22040b,0xff3b0912,0xff60101e,0xff8d7f82,0xffd51b38,0xffbec3c8,0xfffaf7f8,0xffc9bfc2,0xff28060e},
            {0xff0d1027,0xff151a35,0xff202747,0xff5d5b88,0xff6549cf,0xfff5f2ff,0xfffbfaff,0xffc8c4df,0xff10132d},
            {0xffe9eef4,0xfff6f8fb,0xffd6e0ec,0xff6c849d,0xff126dbe,0xff3b92dd,0xff14273b,0xff4c6073,0xffe1eaf4},
            {0xff9fa5ad,0xffc8cdd2,0xffe0e3e7,0xff717982,0xff245e83,0xffe9bc36,0xff16222c,0xff394853,0xffb4bcc4},
            {0xff24292e,0xff30363d,0xff424a53,0xff74818c,0xff9ac9df,0xffffd84a,0xfff1f4f6,0xffbbc4cc,0xff252b31}
    };
    static int BACKGROUND=PALETTES[0][0],PANEL=PALETTES[0][1],SURFACE=PALETTES[0][2],
            BORDER=PALETTES[0][3],ACCENT=PALETTES[0][4],HIGHLIGHT=PALETTES[0][5],
            TEXT=PALETTES[0][6],MUTED=PALETTES[0][7],FIELD=PALETTES[0][8],ACCENT_TEXT=0xff091018;
    static int themeIndex=0;
    static void applyTheme(int index){
        themeIndex=Math.max(0,Math.min(PALETTES.length-1,index));int[] p=PALETTES[themeIndex];
        BACKGROUND=p[0];PANEL=p[1];SURFACE=p[2];BORDER=p[3];ACCENT=p[4];HIGHLIGHT=p[5];TEXT=p[6];MUTED=p[7];FIELD=p[8];
        int brightness=(Color.red(ACCENT)*299+Color.green(ACCENT)*587+Color.blue(ACCENT)*114)/1000;
        ACCENT_TEXT=brightness<150?0xffffffff:0xff091018;
    }
    static final int[] LAYER_ACCENTS={0xff138cff,0xffd94cff,0xff18d6a4,
            0xffff9b42,0xffff4f7b,0xfff2d13d};
    interface Change { void set(float value); }
    interface Selection { void set(int index); }
    static int dp(Context c,float n){return Math.round(n*c.getResources().getDisplayMetrics().density);}
    static GradientDrawable background(int color){GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(7);d.setStroke(1,BORDER);return d;}
    static LinearLayout column(Context c){LinearLayout l=new LinearLayout(c);l.setOrientation(LinearLayout.VERTICAL);return l;}
    static TextView label(Context c,String text,int size){TextView t=new TextView(c);t.setText(text);t.setTextColor(TEXT);t.setTextSize(size);t.setGravity(Gravity.CENTER);return t;}
    static Button button(Context c,String text,Runnable action){Button b=new Button(c);b.setText(text);b.setTextSize(11);b.setAllCaps(false);b.setSingleLine(true);b.setTextColor(TEXT);b.setMinHeight(dp(c,44));b.setMinimumHeight(dp(c,44));b.setPadding(dp(c,6),0,dp(c,6),0);b.setBackground(background(SURFACE));b.setOnClickListener(v->action.run());return b;}
    static void addButton(LinearLayout row,String text,Runnable action){Button b=button(row.getContext(),text,action);LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,dp(row.getContext(),44),1);p.setMargins(2,1,2,1);row.addView(b,p);}
    static final class Panel {
        final Dialog dialog;
        final LinearLayout body,footer,tabs;
        final BoundedScrollView scroll;
        Panel(Activity a,String title,String subtitle){
            dialog=new Dialog(a);dialog.requestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
            LinearLayout root=column(a);root.setPadding(dp(a,7),dp(a,4),dp(a,7),dp(a,4));root.setBackground(background(PANEL));
            TextView heading=label(a,title,16);heading.setTypeface(android.graphics.Typeface.DEFAULT,android.graphics.Typeface.BOLD);heading.setSingleLine(true);heading.setEllipsize(android.text.TextUtils.TruncateAt.END);root.addView(heading,new LinearLayout.LayoutParams(-1,dp(a,28)));
            if(subtitle!=null){TextView hint=label(a,subtitle,11);hint.setSingleLine(true);hint.setEllipsize(android.text.TextUtils.TruncateAt.END);hint.setTextColor(MUTED);root.addView(hint,new LinearLayout.LayoutParams(-1,dp(a,20)));}
            HorizontalScrollView tabScroller=new HorizontalScrollView(a);tabScroller.setHorizontalScrollBarEnabled(false);tabScroller.setFillViewport(true);
            tabs=new LinearLayout(a);tabs.setGravity(Gravity.CENTER);tabScroller.addView(tabs,new HorizontalScrollView.LayoutParams(-2,dp(a,42)));
            root.addView(tabScroller,new LinearLayout.LayoutParams(-1,dp(a,42)));
            scroll=new BoundedScrollView(a);scroll.setVerticalScrollBarEnabled(true);scroll.setScrollBarStyle(View.SCROLLBARS_INSIDE_INSET);body=column(a);body.setPadding(0,0,0,dp(a,8));scroll.addView(body,new ScrollView.LayoutParams(-1,-2));root.addView(scroll,new LinearLayout.LayoutParams(-1,-2));
            footer=new LinearLayout(a);footer.setGravity(Gravity.END);root.addView(footer,new LinearLayout.LayoutParams(-1,dp(a,48)));
            dialog.setContentView(root);
        }
        void show(){
            android.util.DisplayMetrics m=body.getResources().getDisplayMetrics();
            // Keep the body inside the usable app area on short landscape
            // screens; the footer stays fixed and never covers the last row.
                scroll.maxHeight=Math.max(dp(body.getContext(),64),m.heightPixels-dp(body.getContext(),220));
            dialog.show();android.view.Window w=dialog.getWindow();if(w!=null){
                w.setBackgroundDrawableResource(android.R.color.transparent);
                // Compact desktop-like panels; never impose a minimum wider
                // than the display because editors must remain usable on a
                // phone held in landscape.
                int availableWidth=Math.min(m.widthPixels-dp(body.getContext(),24),Math.round(m.widthPixels*.94f));
                w.setLayout(Math.max(1,Math.min(availableWidth,dp(body.getContext(),560))),ViewGroup.LayoutParams.WRAP_CONTENT);
            }
        }
        void setTabs(String[] labels,int selected,Selection onSelect){
            tabs.removeAllViews();
            for(int i=0;i<labels.length;i++){
                final int index=i;Button b=button(tabs.getContext(),labels[i],()->onSelect.set(index));
                b.setTextSize(10);b.setMinHeight(dp(tabs.getContext(),40));b.setMinimumHeight(dp(tabs.getContext(),40));
                b.setBackground(background(i==selected?ACCENT:SURFACE));b.setTextColor(i==selected?ACCENT_TEXT:TEXT);
                LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(dp(tabs.getContext(),108),dp(tabs.getContext(),40));p.setMargins(2,1,2,1);tabs.addView(b,p);
            }
            tabs.setVisibility(labels.length==0?View.GONE:View.VISIBLE);
        }
    }
    /** Lets short effect panels wrap their controls, while long editors scroll above the fixed footer. */
    static final class BoundedScrollView extends ScrollView {
        int maxHeight;float downY;final int touchSlop;
        BoundedScrollView(Context context){super(context);touchSlop=ViewConfiguration.get(context).getScaledTouchSlop();setFillViewport(false);setClipToPadding(true);setClipChildren(true);}
        @Override public boolean onInterceptTouchEvent(MotionEvent event){
            if(event.getActionMasked()==MotionEvent.ACTION_DOWN)downY=event.getY();
            else if(event.getActionMasked()==MotionEvent.ACTION_MOVE){float delta=downY-event.getY();if(Math.abs(delta)>touchSlop&&!canScrollVertically(delta>0?1:-1))return false;}
            return super.onInterceptTouchEvent(event);
        }
        @Override protected void onMeasure(int widthMeasureSpec,int heightMeasureSpec){
            int parentLimit=MeasureSpec.getMode(heightMeasureSpec)==MeasureSpec.UNSPECIFIED
                    ?Integer.MAX_VALUE:MeasureSpec.getSize(heightMeasureSpec);
            int limit=Math.min(maxHeight>0?maxHeight:parentLimit,parentLimit);
            int bounded=MeasureSpec.makeMeasureSpec(limit,MeasureSpec.AT_MOST);
            super.onMeasure(widthMeasureSpec,bounded);
        }
    }
    static Spinner selector(LinearLayout parent,String caption,String[] values,int selected,Selection action){
        Context c=parent.getContext();LinearLayout row=new LinearLayout(c);row.setGravity(Gravity.CENTER_VERTICAL);row.setPadding(0,dp(c,2),0,dp(c,2));
        TextView title=label(c,caption,10);title.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);row.addView(title,new LinearLayout.LayoutParams(dp(c,72),dp(c,44)));
        Spinner spinner=new Spinner(c,Spinner.MODE_DROPDOWN);spinner.setBackground(background(FIELD));spinner.setPopupBackgroundDrawable(background(FIELD));
        ArrayAdapter<String> adapter=new ArrayAdapter<String>(c,android.R.layout.simple_spinner_item,values){
            private View item(int position,View convert,ViewGroup group){TextView t=label(c,getItem(position)+"  ▾",11);t.setGravity(Gravity.CENTER_VERTICAL|Gravity.START);t.setPadding(dp(c,8),0,dp(c,8),0);t.setMinHeight(dp(c,44));return t;}
            @Override public View getView(int p,View v,ViewGroup g){return item(p,v,g);}
            @Override public View getDropDownView(int p,View v,ViewGroup g){return item(p,v,g);}
        };
        spinner.setAdapter(adapter);int initial=Math.max(0,Math.min(selected,values.length-1));spinner.setSelection(initial);
        final int[] previous={initial};spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){public void onNothingSelected(AdapterView<?> p){}public void onItemSelected(AdapterView<?> p,View v,int index,long id){if(index!=previous[0]){previous[0]=index;action.set(index);}}});
        row.addView(spinner,new LinearLayout.LayoutParams(0,dp(c,44),1));parent.addView(row,new LinearLayout.LayoutParams(-1,-2));return spinner;
    }
    static Spinner selectorCell(LinearLayout parent,String caption,String[] values,int selected,Selection action){
        Context c=parent.getContext();LinearLayout cell=column(c);cell.setPadding(dp(c,2),dp(c,1),dp(c,2),dp(c,1));
        TextView title=label(c,caption,10);title.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);cell.addView(title,new LinearLayout.LayoutParams(-1,dp(c,20)));
        Spinner spinner=new Spinner(c,Spinner.MODE_DROPDOWN);spinner.setBackground(background(FIELD));spinner.setPopupBackgroundDrawable(background(FIELD));
        ArrayAdapter<String> adapter=new ArrayAdapter<String>(c,android.R.layout.simple_spinner_item,values){
            private View item(int position){TextView t=label(c,getItem(position)+" ▾",11);t.setGravity(Gravity.CENTER_VERTICAL|Gravity.START);t.setSingleLine(true);t.setEllipsize(android.text.TextUtils.TruncateAt.END);t.setPadding(dp(c,8),0,dp(c,6),0);t.setMinHeight(dp(c,44));return t;}
            @Override public View getView(int p,View v,ViewGroup g){return item(p);}
            @Override public View getDropDownView(int p,View v,ViewGroup g){return item(p);}
        };
        spinner.setAdapter(adapter);int initial=Math.max(0,Math.min(selected,values.length-1));spinner.setSelection(initial);
        final int[] previous={initial};spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){public void onNothingSelected(AdapterView<?> p){}public void onItemSelected(AdapterView<?> p,View v,int index,long id){if(index!=previous[0]){previous[0]=index;action.set(index);}}});
        cell.addView(spinner,new LinearLayout.LayoutParams(-1,dp(c,44)));LinearLayout.LayoutParams cp=new LinearLayout.LayoutParams(0,-2,1);cp.setMargins(2,1,2,1);parent.addView(cell,cp);return spinner;
    }
    static LinearLayout gridRow(LinearLayout parent){LinearLayout row=new LinearLayout(parent.getContext());row.setGravity(Gravity.CENTER_VERTICAL);parent.addView(row,new LinearLayout.LayoutParams(-1,-2));return row;}
    static LinearLayout knobRow(LinearLayout parent){LinearLayout row=new LinearLayout(parent.getContext());row.setGravity(Gravity.CENTER);row.setPadding(0,dp(parent.getContext(),2),0,dp(parent.getContext(),2));parent.addView(row,new LinearLayout.LayoutParams(-1,-2));return row;}
    static Knob knob(LinearLayout row,String label,float value,float min,float max,String unit,Change action){Knob knob=new Knob(row.getContext(),label,value,min,max,unit,action);row.addView(knob,new LinearLayout.LayoutParams(0,dp(row.getContext(),68),1));return knob;}
    static final class Knob extends View {
        final Paint p=new Paint(Paint.ANTI_ALIAS_FLAG);final String label,unit;final float min,max;final Change action;
        float value,downX,downY,start;int pointer=-1,dragAxis;final int touchSlop;
        Knob(Context c,String label,float value,float min,float max,String unit,Change action){super(c);this.label=label;this.min=min;this.max=max;this.unit=unit;this.value=Math.max(min,Math.min(max,value));this.action=action;touchSlop=ViewConfiguration.get(c).getScaledTouchSlop();setFocusable(true);describe();}
        void describe(){setContentDescription(label+" "+format()+". Arraste na horizontal para ajustar; na vertical para rolar a tela.");}
        void setValue(float next){value=Math.max(min,Math.min(max,next));describe();invalidate();}
        String format(){return String.format(Locale.ROOT,"%.1f%s",value,unit);}
        @Override protected void onDraw(Canvas c){
            float w=getWidth(),h=getHeight(),cx=w/2,cy=h*.45f,r=Math.min(w*.26f,h*.22f);p.setShader(null);p.setStyle(Paint.Style.FILL);p.setColor(TEXT);p.setTextAlign(Paint.Align.CENTER);p.setTextSize(dp(getContext(),8));c.drawText(label,cx,dp(getContext(),10),p);
            p.setColor(FIELD);c.drawCircle(cx,cy,r+dp(getContext(),3),p);p.setShader(new LinearGradient(cx,cy-r,cx,cy+r,SURFACE,PANEL,Shader.TileMode.CLAMP));c.drawCircle(cx,cy,r,p);p.setShader(null);p.setStyle(Paint.Style.STROKE);p.setStrokeWidth(dp(getContext(),1));p.setColor(BORDER);c.drawCircle(cx,cy,r,p);
            float fraction=(value-min)/(max-min);p.setColor(ACCENT);p.setStrokeWidth(dp(getContext(),2));c.drawArc(new RectF(cx-r-3,cy-r-3,cx+r+3,cy+r+3),135,270*fraction,false,p);double angle=Math.toRadians(135+270*fraction);c.drawLine(cx+(float)Math.cos(angle)*r*.25f,cy+(float)Math.sin(angle)*r*.25f,cx+(float)Math.cos(angle)*r*.78f,cy+(float)Math.sin(angle)*r*.78f,p);
            p.setStyle(Paint.Style.FILL);p.setColor(FIELD);float bottom=cy+r+dp(getContext(),2);c.drawRect(Math.max(3,cx-dp(getContext(),34)),bottom,Math.min(w-3,cx+dp(getContext(),34)),bottom+dp(getContext(),14),p);p.setColor(TEXT);p.setTextSize(dp(getContext(),9));c.drawText(format(),cx,bottom+dp(getContext(),11),p);
        }
        @Override public boolean onTouchEvent(MotionEvent e){switch(e.getActionMasked()){
            case MotionEvent.ACTION_DOWN:pointer=e.getPointerId(0);downX=e.getX();downY=e.getY();start=value;dragAxis=0;return true;
            case MotionEvent.ACTION_MOVE:int i=e.findPointerIndex(pointer);if(i<0)return true;float dx=e.getX(i)-downX,dy=downY-e.getY(i);if(dragAxis==0){if(Math.abs(dx)>touchSlop&&Math.abs(dx)>Math.abs(dy)){dragAxis=1;getParent().requestDisallowInterceptTouchEvent(true);}else if(Math.abs(dy)>touchSlop){if(canScrollParent(dy)){return true;}dragAxis=2;getParent().requestDisallowInterceptTouchEvent(true);}}if(dragAxis!=0){float delta=dragAxis==1?dx:dy;value=Math.max(min,Math.min(max,start+delta/dp(getContext(),180)*(max-min)));action.set(value);describe();invalidate();}return true;
            case MotionEvent.ACTION_UP:if(dragAxis==0)performClick();pointer=-1;dragAxis=0;getParent().requestDisallowInterceptTouchEvent(false);return true;
            case MotionEvent.ACTION_CANCEL:pointer=-1;dragAxis=0;getParent().requestDisallowInterceptTouchEvent(false);return true;
            default:return true;}}
        @Override public boolean performClick(){super.performClick();return true;}
        private boolean canScrollParent(float fingerDelta){ViewParent parent=getParent();while(parent!=null){if(parent instanceof ScrollView)return ((ScrollView)parent).canScrollVertically(fingerDelta>0?1:-1);if(parent instanceof View)parent=parent.getParent();else break;}return false;}
    }

    /** Small response graph for the EQ and compressor values available in the Android engine. */
    static final class ResponseGraph extends View {
        final Paint p=new Paint(Paint.ANTI_ALIAS_FLAG);
        final boolean compressor;
        float a,b,c;
        float[] frequencies={220,1200,4200},qs={.707f,1,.707f};float highPass=20,lowPass=20000;
        ResponseGraph(Context context,boolean compressor,float a,float b,float c){super(context);this.compressor=compressor;setValues(a,b,c);}
        void setValues(float a,float b,float c){this.a=a;this.b=b;this.c=c;invalidate();}
        void setParametricValues(float lowFreq,float midFreq,float highFreq,float lowQ,float midQ,float highQ,float highPassHz,float lowPassHz){frequencies=new float[]{lowFreq,midFreq,highFreq};qs=new float[]{lowQ,midQ,highQ};highPass=highPassHz;lowPass=lowPassHz;invalidate();}
        @Override protected void onDraw(Canvas canvas){
            int width=getWidth(),height=getHeight();float left=dp(getContext(),33),top=dp(getContext(),24),right=width-dp(getContext(),12),bottom=height-dp(getContext(),25);
            p.setStyle(Paint.Style.FILL);p.setColor(FIELD);canvas.drawRoundRect(new RectF(0,0,width,height),dp(getContext(),5),dp(getContext(),5),p);
            p.setTextSize(dp(getContext(),10));p.setColor(MUTED);p.setTextAlign(Paint.Align.LEFT);canvas.drawText(compressor?"COMPRESSOR DA LAYER":"EQ DA LAYER",left,dp(getContext(),16),p);
            p.setStyle(Paint.Style.STROKE);p.setStrokeWidth(dp(getContext(),1));p.setColor(BORDER);
            for(int i=0;i<=4;i++){float x=left+(right-left)*i/4f,y=top+(bottom-top)*i/4f;canvas.drawLine(x,top,x,bottom,p);canvas.drawLine(left,y,right,y,p);}
            p.setColor(ACCENT);p.setStrokeWidth(dp(getContext(),2));
            float prevX=left,prevY=bottom;
            for(int i=0;i<=100;i++){
                float x=left+(right-left)*i/100f,y;
                if(compressor){float inputDb=-20f+20f*i/100f,input=(float)Math.pow(10,inputDb/20f),threshold=Math.max(.001f,Math.min(1f,a)),ratio=Math.max(1f,b);float output=input<=threshold?input:threshold+(input-threshold)/ratio;float outputDb=Math.max(-20f,20f*(float)Math.log10(Math.max(.1f,output)));y=bottom-(bottom-top)*(outputDb+20f)/20f;}
                else{float normalized=i/100f,frequency=20f*(float)Math.pow(1000,normalized),octave=(float)(Math.log(frequency/20.0)/Math.log(2.0)),gain=0;float[] gains={a,b,c};for(int band=0;band<3;band++){float center=(float)(Math.log(frequencies[band]/20.0)/Math.log(2.0)),bandWidth=Math.max(.12f,1.0f/Math.max(.1f,qs[band]));gain+=gains[band]*(float)Math.exp(-Math.pow((octave-center)/bandWidth,2));}if(frequency<highPass)gain-=Math.min(36,18f*(float)(Math.log(highPass/frequency)/Math.log(2.0)));if(frequency>lowPass)gain-=Math.min(36,18f*(float)(Math.log(frequency/lowPass)/Math.log(2.0)));y=top+(bottom-top)*(.5f-gain/36f);}
                if(i>0)canvas.drawLine(prevX,prevY,x,y,p);prevX=x;prevY=y;
            }
            p.setStyle(Paint.Style.FILL);p.setColor(MUTED);p.setTextAlign(Paint.Align.CENTER);p.setTextSize(dp(getContext(),9));
            String[] ticks=compressor?new String[]{"−20","−15","−10","−5","0 dB"}:new String[]{"20","100","1k","5k","20k"};
            for(int i=0;i<5;i++){float x=compressor?left+(right-left)*i/4f:left+(right-left)*(float)(Math.log(Integer.parseInt(ticks[i].replace("k","000").replace(" ",""))/20.0)/Math.log(1000.0));canvas.drawText(ticks[i],x,bottom+dp(getContext(),15),p);}
        }
    }
}
