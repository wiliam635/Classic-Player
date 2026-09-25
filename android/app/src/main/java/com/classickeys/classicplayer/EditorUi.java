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
import android.widget.*;
import java.util.Locale;

/** Touch-sized version of the desktop editor's charcoal panels and teal knobs. */
final class EditorUi {
    static final int PANEL=0xff253238, FIELD=0xff0a151b, TEXT=0xffe9eff0,
            MUTED=0xffa7bac2, ACCENT=0xff13b8ad, BORDER=0xff41545e;
    interface Change { void set(float value); }
    interface Selection { void set(int index); }
    static int dp(Context c,float n){return Math.round(n*c.getResources().getDisplayMetrics().density);}
    static GradientDrawable background(int color){GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(5);d.setStroke(1,BORDER);return d;}
    static LinearLayout column(Context c){LinearLayout l=new LinearLayout(c);l.setOrientation(LinearLayout.VERTICAL);return l;}
    static TextView label(Context c,String text,int size){TextView t=new TextView(c);t.setText(text);t.setTextColor(TEXT);t.setTextSize(size);t.setGravity(Gravity.CENTER);return t;}
    static Button button(Context c,String text,Runnable action){Button b=new Button(c);b.setText(text);b.setTextSize(12);b.setTextColor(TEXT);b.setMinHeight(dp(c,44));b.setMinimumHeight(dp(c,44));b.setBackground(background(0xff1d2c35));b.setOnClickListener(v->action.run());return b;}
    static void addButton(LinearLayout row,String text,Runnable action){Button b=button(row.getContext(),text,action);LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,dp(row.getContext(),48),1);p.setMargins(3,3,3,3);row.addView(b,p);}
    static final class Panel {
        final Dialog dialog;
        final LinearLayout body,footer;
        Panel(Activity a,String title,String subtitle){
            dialog=new Dialog(a);dialog.requestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
            LinearLayout root=column(a);root.setPadding(dp(a,16),dp(a,12),dp(a,16),dp(a,12));root.setBackground(background(PANEL));
            root.addView(label(a,title,19),new LinearLayout.LayoutParams(-1,dp(a,34)));
            if(subtitle!=null){TextView hint=label(a,subtitle,12);hint.setTextColor(MUTED);hint.setPadding(0,0,0,dp(a,8));root.addView(hint);}
            ScrollView scroll=new ScrollView(a);scroll.setFillViewport(true);body=column(a);scroll.addView(body,new ScrollView.LayoutParams(-1,-2));root.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
            footer=new LinearLayout(a);footer.setGravity(Gravity.END);root.addView(footer,new LinearLayout.LayoutParams(-1,-2));
            dialog.setContentView(root);
        }
        void show(){dialog.show();android.view.Window w=dialog.getWindow();if(w!=null){w.setBackgroundDrawableResource(android.R.color.transparent);android.util.DisplayMetrics m=body.getResources().getDisplayMetrics();w.setLayout(Math.min(m.widthPixels-dp(body.getContext(),16),dp(body.getContext(),1040)),m.heightPixels-dp(body.getContext(),24));}}
    }
    static Spinner selector(LinearLayout parent,String caption,String[] values,int selected,Selection action){
        Context c=parent.getContext();LinearLayout row=new LinearLayout(c);row.setGravity(Gravity.CENTER_VERTICAL);row.setPadding(0,dp(c,4),0,dp(c,4));
        TextView title=label(c,caption,11);title.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);row.addView(title,new LinearLayout.LayoutParams(dp(c,100),dp(c,44)));
        Spinner spinner=new Spinner(c,Spinner.MODE_DROPDOWN);spinner.setBackground(background(FIELD));spinner.setPopupBackgroundDrawable(background(FIELD));
        ArrayAdapter<String> adapter=new ArrayAdapter<String>(c,android.R.layout.simple_spinner_item,values){
            private View item(int position,View convert,ViewGroup group){TextView t=label(c,getItem(position)+"  ▾",13);t.setGravity(Gravity.CENTER_VERTICAL|Gravity.START);t.setPadding(dp(c,10),dp(c,8),dp(c,10),dp(c,8));t.setMinHeight(dp(c,44));return t;}
            @Override public View getView(int p,View v,ViewGroup g){return item(p,v,g);}
            @Override public View getDropDownView(int p,View v,ViewGroup g){return item(p,v,g);}
        };
        spinner.setAdapter(adapter);int initial=Math.max(0,Math.min(selected,values.length-1));spinner.setSelection(initial);
        final int[] previous={initial};spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){public void onNothingSelected(AdapterView<?> p){}public void onItemSelected(AdapterView<?> p,View v,int index,long id){if(index!=previous[0]){previous[0]=index;action.set(index);}}});
        row.addView(spinner,new LinearLayout.LayoutParams(0,dp(c,48),1));parent.addView(row,new LinearLayout.LayoutParams(-1,-2));return spinner;
    }
    static LinearLayout knobRow(LinearLayout parent){LinearLayout row=new LinearLayout(parent.getContext());row.setGravity(Gravity.CENTER);row.setPadding(0,dp(parent.getContext(),10),0,dp(parent.getContext(),8));parent.addView(row,new LinearLayout.LayoutParams(-1,-2));return row;}
    static void knob(LinearLayout row,String label,float value,float min,float max,String unit,Change action){row.addView(new Knob(row.getContext(),label,value,min,max,unit,action),new LinearLayout.LayoutParams(0,dp(row.getContext(),134),1));}
    static final class Knob extends View {
        final Paint p=new Paint(Paint.ANTI_ALIAS_FLAG);final String label,unit;final float min,max;final Change action;
        float value,downY,start;int pointer=-1;
        Knob(Context c,String label,float value,float min,float max,String unit,Change action){super(c);this.label=label;this.min=min;this.max=max;this.unit=unit;this.value=Math.max(min,Math.min(max,value));this.action=action;setFocusable(true);describe();}
        void describe(){setContentDescription(label+" "+format());}
        String format(){return String.format(Locale.ROOT,"%.1f%s",value,unit);}
        @Override protected void onDraw(Canvas c){
            float w=getWidth(),h=getHeight(),cx=w/2,cy=h*.48f,r=Math.min(w*.32f,h*.26f);p.setShader(null);p.setStyle(Paint.Style.FILL);p.setColor(TEXT);p.setTextAlign(Paint.Align.CENTER);p.setTextSize(dp(getContext(),10));c.drawText(label,cx,dp(getContext(),15),p);
            p.setColor(FIELD);c.drawCircle(cx,cy,r+dp(getContext(),3),p);p.setShader(new LinearGradient(cx,cy-r,cx,cy+r,0xff57656b,0xff263238,Shader.TileMode.CLAMP));c.drawCircle(cx,cy,r,p);p.setShader(null);p.setStyle(Paint.Style.STROKE);p.setStrokeWidth(dp(getContext(),1));p.setColor(0xff8b999f);c.drawCircle(cx,cy,r,p);
            float fraction=(value-min)/(max-min);p.setColor(ACCENT);p.setStrokeWidth(dp(getContext(),2));c.drawArc(new RectF(cx-r-3,cy-r-3,cx+r+3,cy+r+3),135,270*fraction,false,p);double angle=Math.toRadians(135+270*fraction);c.drawLine(cx+(float)Math.cos(angle)*r*.25f,cy+(float)Math.sin(angle)*r*.25f,cx+(float)Math.cos(angle)*r*.78f,cy+(float)Math.sin(angle)*r*.78f,p);
            p.setStyle(Paint.Style.FILL);p.setColor(FIELD);float bottom=cy+r+dp(getContext(),8);c.drawRect(Math.max(3,cx-dp(getContext(),48)),bottom,Math.min(w-3,cx+dp(getContext(),48)),bottom+dp(getContext(),23),p);p.setColor(TEXT);p.setTextSize(dp(getContext(),12));c.drawText(format(),cx,bottom+dp(getContext(),16),p);
        }
        @Override public boolean onTouchEvent(MotionEvent e){switch(e.getActionMasked()){
            case MotionEvent.ACTION_DOWN:pointer=e.getPointerId(0);downY=e.getY();start=value;getParent().requestDisallowInterceptTouchEvent(true);return true;
            case MotionEvent.ACTION_MOVE:int i=e.findPointerIndex(pointer);if(i<0)return true;value=Math.max(min,Math.min(max,start+(downY-e.getY(i))/dp(getContext(),220)*(max-min)));action.set(value);describe();invalidate();return true;
            case MotionEvent.ACTION_UP:performClick();pointer=-1;getParent().requestDisallowInterceptTouchEvent(false);return true;
            case MotionEvent.ACTION_CANCEL:pointer=-1;getParent().requestDisallowInterceptTouchEvent(false);return true;
            default:return true;}}
        @Override public boolean performClick(){super.performClick();return true;}
    }

    /** Small response graph for the EQ and compressor values available in the Android engine. */
    static final class ResponseGraph extends View {
        final Paint p=new Paint(Paint.ANTI_ALIAS_FLAG);
        final boolean compressor;
        float a,b,c;
        ResponseGraph(Context context,boolean compressor,float a,float b,float c){super(context);this.compressor=compressor;setValues(a,b,c);}
        void setValues(float a,float b,float c){this.a=a;this.b=b;this.c=c;invalidate();}
        @Override protected void onDraw(Canvas canvas){
            int width=getWidth(),height=getHeight();float left=dp(getContext(),33),top=dp(getContext(),24),right=width-dp(getContext(),12),bottom=height-dp(getContext(),25);
            p.setStyle(Paint.Style.FILL);p.setColor(FIELD);canvas.drawRoundRect(new RectF(0,0,width,height),dp(getContext(),5),dp(getContext(),5),p);
            p.setTextSize(dp(getContext(),10));p.setColor(MUTED);p.setTextAlign(Paint.Align.LEFT);canvas.drawText(compressor?"COMPRESSOR DA LAYER":"EQ DA LAYER",left,dp(getContext(),16),p);
            p.setStyle(Paint.Style.STROKE);p.setStrokeWidth(dp(getContext(),1));p.setColor(0xff344850);
            for(int i=0;i<=4;i++){float x=left+(right-left)*i/4f,y=top+(bottom-top)*i/4f;canvas.drawLine(x,top,x,bottom,p);canvas.drawLine(left,y,right,y,p);}
            p.setColor(ACCENT);p.setStrokeWidth(dp(getContext(),2));
            float prevX=left,prevY=bottom;
            for(int i=0;i<=100;i++){
                float x=left+(right-left)*i/100f,y;
                if(compressor){float input=i/100f,threshold=Math.max(.1f,Math.min(1f,a)),ratio=Math.max(1f,b);float output=input<=threshold?input:threshold+(input-threshold)/ratio;y=bottom-(bottom-top)*output;}
                else{float band=i/100f,gain=(float)(a*Math.exp(-Math.pow((band-.12f)/.2f,2))+b*Math.exp(-Math.pow((band-.5f)/.22f,2))+c*Math.exp(-Math.pow((band-.88f)/.2f,2)));y=top+(bottom-top)*(.5f-(gain-1f)*.25f);}
                if(i>0)canvas.drawLine(prevX,prevY,x,y,p);prevX=x;prevY=y;
            }
            p.setStyle(Paint.Style.FILL);p.setColor(MUTED);p.setTextAlign(Paint.Align.CENTER);p.setTextSize(dp(getContext(),9));
            String[] ticks=compressor?new String[]{"−60","−45","−30","−15","0 dB"}:new String[]{"20","100","1k","5k","20k"};
            for(int i=0;i<5;i++)canvas.drawText(ticks[i],left+(right-left)*i/4f,bottom+dp(getContext(),15),p);
        }
    }
}
