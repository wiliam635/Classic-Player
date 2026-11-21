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
    static final String[] THEME_NAMES={"GRAFITE ÂMBAR","CLÁSSICO","PRETO","VERMELHO","AZUL ROXO"};
    /** On-state / graph / knob-arc colour: amber in every theme (matches desktop Conceito D). */
    static final int AMBER=0xfff5a524;
    // Keep the seven desktop palettes in the same order so a theme selected
    // on either platform has the same visual identity.
    private static final int[][] PALETTES={
            {0xff0f1114,0xff171a1f,0xff1f242b,0xff2b3239,AMBER,0xff4cc9f0,0xffe8ecf1,0xff8b95a1,0xff0b0d10},
            {0xff091018,0xff151f28,0xff202c35,0xff33414c,AMBER,0xffffd84a,0xffedf4f7,0xff9eabb5,0xff0b151e},
            {0xff050608,0xff101216,0xff1c2025,0xff565e66,AMBER,0xfff3f5f6,0xfff6f7f8,0xffb1b7bd,0xff090b0e},
            {0xff22040b,0xff3b0912,0xff60101e,0xff8d7f82,AMBER,0xffbec3c8,0xfffaf7f8,0xffc9bfc2,0xff28060e},
            {0xff0d1027,0xff151a35,0xff202747,0xff5d5b88,AMBER,0xfff5f2ff,0xfffbfaff,0xffc8c4df,0xff10132d}
    };
    /** Saved index from builds that still had the 7 old themes (0 teal,1 black,2 red,3 purple,4-6 light/steel). */
    static int migrateSavedTheme(int oldIndex){return oldIndex>=0&&oldIndex<=3?oldIndex+1:0;}
    static int BACKGROUND=PALETTES[0][0],PANEL=PALETTES[0][1],SURFACE=PALETTES[0][2],
            BORDER=PALETTES[0][3],ACCENT=PALETTES[0][4],HIGHLIGHT=PALETTES[0][5],
            TEXT=PALETTES[0][6],MUTED=PALETTES[0][7],FIELD=PALETTES[0][8],ACCENT_TEXT=0xff091018;
    static int themeIndex=0;
    static void applyTheme(int index){
        themeIndex=Math.max(0,Math.min(PALETTES.length-1,index));int[] p=PALETTES[themeIndex];
        BACKGROUND=p[0];PANEL=p[1];SURFACE=p[2];BORDER=p[3];ACCENT=p[4];HIGHLIGHT=p[5];TEXT=p[6];MUTED=p[7];FIELD=p[8];

        ACCENT_TEXT=0xff0f1114;
    }
    static final int[] LAYER_ACCENTS={0xff138cff,0xffd94cff,0xff18d6a4,
            0xffff9b42,0xffff4f7b,0xfff2d13d};
    interface Change { void set(float value); }
    interface Selection { void set(int index); }
    static int dp(Context c,float n){return Math.round(n*c.getResources().getDisplayMetrics().density);}
    static GradientDrawable outlined(int fill,int stroke,int widthDp,Context c){GradientDrawable d=new GradientDrawable();d.setColor(fill);d.setCornerRadius(dp(c,6));d.setStroke(dp(c,widthDp),stroke);return d;}
    static GradientDrawable background(int color){GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(7);d.setStroke(1,BORDER);return d;}
    static LinearLayout column(Context c){LinearLayout l=new LinearLayout(c);l.setOrientation(LinearLayout.VERTICAL);return l;}
    static TextView label(Context c,String text,int size){TextView t=new TextView(c);t.setText(text);t.setTextColor(TEXT);t.setTextSize(size);t.setGravity(Gravity.CENTER);return t;}
    static Button button(Context c,String text,Runnable action){Button b=new Button(c);b.setText(text);b.setTextSize(11);b.setAllCaps(false);b.setSingleLine(true);b.setTextColor(TEXT);b.setMinHeight(dp(c,44));b.setMinimumHeight(dp(c,44));b.setPadding(dp(c,6),0,dp(c,6),0);b.setBackground(background(SURFACE));b.setOnClickListener(v->action.run());return b;}
    /** Two-step button: the first tap arms it (and changes the label), the second tap within 4 s runs the action. No dialog. */
    static void addConfirmButton(LinearLayout row,String text,String armedText,Runnable action){
        final Button[] holder=new Button[1];final boolean[] armed={false};
        final Runnable disarm=()->{armed[0]=false;holder[0].setText(text);holder[0].setBackground(background(FIELD));};
        holder[0]=button(row.getContext(),text,()->{
            if(!armed[0]){armed[0]=true;holder[0].setText(armedText);holder[0].setBackground(background(0xff7a1f24));holder[0].postDelayed(disarm,4000);}
            else{holder[0].removeCallbacks(disarm);disarm.run();action.run();}
        });
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,dp(row.getContext(),44),1);p.setMargins(2,1,2,1);row.addView(holder[0],p);
    }
    static void addButton(LinearLayout row,String text,Runnable action){Button b=button(row.getContext(),text,action);LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,dp(row.getContext(),44),1);p.setMargins(2,1,2,1);row.addView(b,p);}
    /** Drop-in replacement for AlertDialog.Builder that follows the app theme (charcoal panel, flat buttons). */
    static final class Alert {
        private final Activity activity;private CharSequence title,message;private CharSequence[] items;private int checked=-1;private boolean singleChoice;
        private android.content.DialogInterface.OnClickListener itemListener,positiveListener,negativeListener,neutralListener;
        private CharSequence positiveText,negativeText,neutralText;private View customView;private android.content.DialogInterface.OnCancelListener cancelListener;
        Alert(Activity a){activity=a;}
        Alert setTitle(CharSequence t){title=t;return this;}
        Alert setMessage(CharSequence m){message=m;return this;}
        Alert setItems(CharSequence[] list,android.content.DialogInterface.OnClickListener l){items=list;itemListener=l;singleChoice=false;return this;}
        Alert setSingleChoiceItems(CharSequence[] list,int selected,android.content.DialogInterface.OnClickListener l){items=list;checked=selected;itemListener=l;singleChoice=true;return this;}
        Alert setView(View v){customView=v;return this;}
        Alert setPositiveButton(CharSequence t,android.content.DialogInterface.OnClickListener l){positiveText=t;positiveListener=l;return this;}
        Alert setNegativeButton(CharSequence t,android.content.DialogInterface.OnClickListener l){negativeText=t;negativeListener=l;return this;}
        Alert setNeutralButton(CharSequence t,android.content.DialogInterface.OnClickListener l){neutralText=t;neutralListener=l;return this;}
        Alert setOnCancelListener(android.content.DialogInterface.OnCancelListener l){cancelListener=l;return this;}
        Dialog show(){
            final Activity a=activity;final Dialog dialog=new Dialog(a);dialog.requestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
            LinearLayout root=column(a);root.setPadding(dp(a,12),dp(a,10),dp(a,12),dp(a,8));root.setBackground(background(PANEL));
            if(title!=null){TextView heading=label(a,title.toString(),15);heading.setTypeface(android.graphics.Typeface.DEFAULT,android.graphics.Typeface.BOLD);heading.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);root.addView(heading,new LinearLayout.LayoutParams(-1,dp(a,34)));}
            final LinearLayout content=column(a);
            if(message!=null){TextView text=label(a,message.toString(),12);text.setGravity(Gravity.START);text.setTextColor(MUTED);text.setPadding(0,dp(a,2),0,dp(a,8));content.addView(text,new LinearLayout.LayoutParams(-1,-2));}
            if(customView!=null){ViewGroup old=(ViewGroup)customView.getParent();if(old!=null)old.removeView(customView);content.addView(customView,new LinearLayout.LayoutParams(-1,-2));}
            if(items!=null){
                final Button[] buttons=new Button[items.length];
                for(int i=0;i<items.length;i++){
                    final int index=i;buttons[i]=button(a,items[i].toString(),()->{});buttons[i].setGravity(Gravity.CENTER_VERTICAL|Gravity.START);
                    if(singleChoice&&i==checked){buttons[i].setBackground(background(ACCENT));buttons[i].setTextColor(ACCENT_TEXT);}
                    buttons[i].setOnClickListener(v->{
                        if(singleChoice){for(int j=0;j<buttons.length;j++){buttons[j].setBackground(background(j==index?ACCENT:FIELD));buttons[j].setTextColor(j==index?ACCENT_TEXT:TEXT);}}
                        else dialog.dismiss();
                        if(itemListener!=null)itemListener.onClick(dialog,index);
                    });
                    LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,dp(a,44));lp.setMargins(0,dp(a,2),0,dp(a,2));content.addView(buttons[i],lp);
                }
            }
            ScrollView scroll=new ScrollView(a);scroll.addView(content,new ScrollView.LayoutParams(-1,-2));
            android.util.DisplayMetrics m=a.getResources().getDisplayMetrics();
            root.addView(new BoundedScrollViewHolder(a,scroll,Math.max(dp(a,120),m.heightPixels-dp(a,190))),new LinearLayout.LayoutParams(-1,-2));
            if(positiveText!=null||negativeText!=null||neutralText!=null){
                LinearLayout footer=new LinearLayout(a);footer.setGravity(Gravity.END|Gravity.CENTER_VERTICAL);
                if(neutralText!=null){footer.addView(alertButton(a,neutralText.toString(),dialog,neutralListener,android.content.DialogInterface.BUTTON_NEUTRAL),new LinearLayout.LayoutParams(-2,dp(a,44)));View gap=new View(a);footer.addView(gap,new LinearLayout.LayoutParams(0,1,1));}
                if(negativeText!=null)footer.addView(alertButton(a,negativeText.toString(),dialog,negativeListener,android.content.DialogInterface.BUTTON_NEGATIVE),new LinearLayout.LayoutParams(-2,dp(a,44)));
                if(positiveText!=null)footer.addView(alertButton(a,positiveText.toString(),dialog,positiveListener,android.content.DialogInterface.BUTTON_POSITIVE),new LinearLayout.LayoutParams(-2,dp(a,44)));
                LinearLayout.LayoutParams fp=new LinearLayout.LayoutParams(-1,dp(a,48));fp.topMargin=dp(a,4);root.addView(footer,fp);
            }
            dialog.setContentView(root);if(cancelListener!=null)dialog.setOnCancelListener(cancelListener);
            dialog.show();android.view.Window w=dialog.getWindow();
            if(w!=null){w.setBackgroundDrawableResource(android.R.color.transparent);w.setLayout(Math.max(1,Math.min(Math.min(m.widthPixels-dp(a,24),Math.round(m.widthPixels*.94f)),dp(a,460))),ViewGroup.LayoutParams.WRAP_CONTENT);}
            return dialog;
        }
        private static Button alertButton(Activity a,String text,final Dialog dialog,final android.content.DialogInterface.OnClickListener l,final int which){
            Button b=button(a,text,()->{});b.setMinWidth(dp(a,88));b.setOnClickListener(v->{dialog.dismiss();if(l!=null)l.onClick(dialog,which);});return b;
        }
        /** Caps a scroll view's height so long lists stay inside the screen. */
        private static final class BoundedScrollViewHolder extends FrameLayout {
            private final int max;
            BoundedScrollViewHolder(Context c,View child,int maxHeight){super(c);max=maxHeight;addView(child,new FrameLayout.LayoutParams(-1,-2));}
            @Override protected void onMeasure(int w,int h){super.onMeasure(w,View.MeasureSpec.makeMeasureSpec(max,View.MeasureSpec.AT_MOST));}
        }
    }

    static final class Panel {
        /** Set right before creating an editor panel: the next Panel docks on the right edge (desktop inspector behaviour). */
        static boolean dockNext;
        /** Like the desktop: a narrow layer strip + master on the left, everything else is the inspector. */
        static int dockReserveDp=320;
        static int dockWidth(Context c){android.util.DisplayMetrics m=c.getResources().getDisplayMetrics();return Math.max(Math.round(m.widthPixels*.5f),m.widthPixels-dp(c,dockReserveDp));}
        Runnable onReset;Button resetButton;
        static java.util.function.Consumer<Panel> dockHook;
        static long dockClosedAt;
        static int dockTop,dockHeight;
        final boolean docked;Runnable onClose;
        final Dialog dialog;
        final LinearLayout body,footer,tabs;
        final BoundedScrollView scroll;
        Panel(Activity a,String title,String subtitle){
            docked=dockNext||(dockClosedAt!=0&&android.os.SystemClock.uptimeMillis()-dockClosedAt<400);dockNext=false;
            if(docked){knobDp=92;compact=true;}
            final boolean wasDocked=docked;
            dialog=new Dialog(a);dialog.setOnDismissListener(d->{if(wasDocked){dockClosedAt=android.os.SystemClock.uptimeMillis();knobDp=68;compact=false;}if(onClose!=null)onClose.run();});dialog.requestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
            LinearLayout root=column(a);root.setPadding(dp(a,7),dp(a,4),dp(a,7),dp(a,4));root.setBackground(background(PANEL));
            if(docked){java.util.regex.Matcher lm=java.util.regex.Pattern.compile("LAYER\\s*\\d+").matcher(title);if(lm.find())title=lm.group();}
            TextView heading=label(a,title,16);heading.setTypeface(android.graphics.Typeface.DEFAULT,android.graphics.Typeface.BOLD);heading.setSingleLine(true);heading.setEllipsize(android.text.TextUtils.TruncateAt.END);if(docked){
                // Desktop inspector header: title on the left, FECHAR on the right.
                heading.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);
                LinearLayout top=new LinearLayout(a);top.setGravity(Gravity.CENTER_VERTICAL);
                top.addView(heading,new LinearLayout.LayoutParams(0,dp(a,40),1));
                Button close=button(a,"FECHAR",()->dialog.dismiss());close.setTextSize(10);close.setMinHeight(dp(a,36));close.setMinimumHeight(dp(a,36));
                final Button reset=button(a,"RESET",()->{});reset.setTextSize(10);reset.setMinHeight(dp(a,36));reset.setMinimumHeight(dp(a,36));reset.setVisibility(View.GONE);resetButton=reset;
                final boolean[] armed={false};
                final Runnable disarm=()->{armed[0]=false;reset.setText("RESET");reset.setBackground(background(FIELD));};
                reset.setOnClickListener(v->{if(!armed[0]){armed[0]=true;reset.setText("CONFIRMAR?");reset.setBackground(background(0xff7a1f24));reset.postDelayed(disarm,4000);}else{reset.removeCallbacks(disarm);disarm.run();if(onReset!=null)onReset.run();}});
                LinearLayout.LayoutParams rp=new LinearLayout.LayoutParams(dp(a,110),dp(a,40));rp.setMargins(0,0,dp(a,6),0);top.addView(reset,rp);
                top.addView(close,new LinearLayout.LayoutParams(dp(a,96),dp(a,40)));
                root.addView(top,new LinearLayout.LayoutParams(-1,dp(a,44)));
            }else root.addView(heading,new LinearLayout.LayoutParams(-1,dp(a,28)));
            if(subtitle!=null&&!docked){TextView hint=label(a,subtitle,11);hint.setSingleLine(true);hint.setEllipsize(android.text.TextUtils.TruncateAt.END);hint.setTextColor(MUTED);root.addView(hint,new LinearLayout.LayoutParams(-1,dp(a,20)));}
            HorizontalScrollView tabScroller=new HorizontalScrollView(a);tabScroller.setHorizontalScrollBarEnabled(false);tabScroller.setFillViewport(true);
            tabs=new LinearLayout(a);tabs.setGravity(docked?Gravity.START|Gravity.CENTER_VERTICAL:Gravity.CENTER);tabScroller.addView(tabs,new HorizontalScrollView.LayoutParams(-2,dp(a,docked?36:42)));
            root.addView(tabScroller,new LinearLayout.LayoutParams(-1,dp(a,docked?36:42)));
            scroll=new BoundedScrollView(a);scroll.setVerticalScrollBarEnabled(true);scroll.setScrollBarStyle(View.SCROLLBARS_INSIDE_INSET);body=column(a);body.setPadding(0,0,0,dp(a,8));scroll.addView(body,new ScrollView.LayoutParams(-1,-2));root.addView(scroll,docked?new LinearLayout.LayoutParams(-1,0,1):new LinearLayout.LayoutParams(-1,-2));
            footer=new LinearLayout(a);footer.setGravity(Gravity.END);root.addView(footer,new LinearLayout.LayoutParams(-1,dp(a,48)));
            dialog.setContentView(root);
        }
        void show(){
            android.util.DisplayMetrics m=body.getResources().getDisplayMetrics();
            // Keep the body inside the usable app area on short landscape
            // screens; the footer stays fixed and never covers the last row.
                scroll.maxHeight=Math.max(dp(body.getContext(),64),m.heightPixels-dp(body.getContext(),220));
            if(docked){
                dockClosedAt=0;footer.setVisibility(View.GONE);if(dockHook!=null)dockHook.accept(this);if(resetButton!=null&&onReset!=null)resetButton.setVisibility(View.VISIBLE);
                scroll.maxHeight=0;dialog.setCanceledOnTouchOutside(false);
                dialog.show();android.view.Window dw=dialog.getWindow();
                if(dw!=null){dw.setBackgroundDrawableResource(android.R.color.transparent);dw.clearFlags(android.view.WindowManager.LayoutParams.FLAG_DIM_BEHIND);
                    dw.addFlags(android.view.WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL);
                    android.view.WindowManager.LayoutParams lp=dw.getAttributes();lp.gravity=Gravity.END|Gravity.TOP;lp.dimAmount=0f;lp.x=dp(body.getContext(),6);lp.y=dockTop;dw.setAttributes(lp);
                    dw.setLayout(dockWidth(body.getContext())-dp(body.getContext(),6),dockHeight>0?dockHeight:ViewGroup.LayoutParams.MATCH_PARENT);}
                return;
            }
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
                b.setTextSize(10);b.setMinHeight(dp(tabs.getContext(),docked?32:40));b.setMinimumHeight(dp(tabs.getContext(),docked?32:40));
                b.setBackground(background(i==selected?ACCENT:SURFACE));b.setTextColor(i==selected?ACCENT_TEXT:TEXT);
                LinearLayout.LayoutParams p=docked?new LinearLayout.LayoutParams(dp(tabs.getContext(),104),dp(tabs.getContext(),32)):new LinearLayout.LayoutParams(dp(tabs.getContext(),108),dp(tabs.getContext(),40));p.setMargins(2,1,2,1);tabs.addView(b,p);
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
        TextView title=label(c,caption,compact?8:10);title.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);title.setSingleLine(true);if(!caption.isEmpty())cell.addView(title,new LinearLayout.LayoutParams(-1,dp(c,compact?14:20)));
        Spinner spinner=new Spinner(c,Spinner.MODE_DROPDOWN);spinner.setBackground(background(FIELD));spinner.setPopupBackgroundDrawable(background(FIELD));
        ArrayAdapter<String> adapter=new ArrayAdapter<String>(c,android.R.layout.simple_spinner_item,values){
            private View item(int position,boolean closed){TextView t=label(c,getItem(position)+" ▾",compact&&closed?10:11);t.setGravity(Gravity.CENTER_VERTICAL|Gravity.START);t.setSingleLine(true);t.setEllipsize(android.text.TextUtils.TruncateAt.END);t.setPadding(dp(c,8),0,dp(c,6),0);t.setMinHeight(dp(c,compact&&closed?32:44));return t;}
            @Override public View getView(int p,View v,ViewGroup g){return item(p,true);}
            @Override public View getDropDownView(int p,View v,ViewGroup g){return item(p,false);}
        };
        spinner.setAdapter(adapter);int initial=Math.max(0,Math.min(selected,values.length-1));spinner.setSelection(initial);
        final int[] previous={initial};spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){public void onNothingSelected(AdapterView<?> p){}public void onItemSelected(AdapterView<?> p,View v,int index,long id){if(index!=previous[0]){previous[0]=index;action.set(index);}}});
        cell.addView(spinner,new LinearLayout.LayoutParams(-1,dp(c,compact?32:44)));LinearLayout.LayoutParams cp=new LinearLayout.LayoutParams(0,-2,1);cp.setMargins(2,1,2,1);parent.addView(cell,cp);return spinner;
    }
    static LinearLayout gridRow(LinearLayout parent){LinearLayout row=new LinearLayout(parent.getContext());row.setGravity(Gravity.CENTER_VERTICAL);parent.addView(row,new LinearLayout.LayoutParams(-1,-2));return row;}
    static LinearLayout knobRow(LinearLayout parent){LinearLayout row=new LinearLayout(parent.getContext());row.setGravity(Gravity.CENTER);row.setPadding(0,dp(parent.getContext(),2),0,dp(parent.getContext(),2));parent.addView(row,new LinearLayout.LayoutParams(-1,-2));return row;}
    /** Knob row height in dp: taller while the desktop-style inspector is open. */
    static final int CYAN_OUTLINE=0xff2bb8e6;
    /** Null-safe: a release handler may rebuild the panel and detach this view before the call. */
    static void disallowIntercept(View view,boolean disallow){ViewParent parent=view.getParent();if(parent!=null)parent.requestDisallowInterceptTouchEvent(disallow);}
    static int knobDp=68;
    /** Tighter controls while the desktop-style inspector is open. */
    static boolean compact=false;
    static boolean knobDocked(){return knobDp>68;}
    static Knob knob(LinearLayout row,String label,float value,float min,float max,String unit,Change action){Knob knob=new Knob(row.getContext(),label,value,min,max,unit,action);row.addView(knob,new LinearLayout.LayoutParams(0,dp(row.getContext(),knobDp),1));return knob;}
    static final class Knob extends View {
        final Paint p=new Paint(Paint.ANTI_ALIAS_FLAG);final String label,unit;final float min,max;final Change action;
        float value,downX,downY,start;int pointer=-1,dragAxis;final int touchSlop;
        /** Long-press (without dragging) hook used for MIDI Learn assignment. */
        Runnable onLongPress;boolean longPressed;
        final Runnable longPressTask=()->{if(pointer>=0&&dragAxis==0&&onLongPress!=null){longPressed=true;performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);onLongPress.run();}};
        Knob(Context c,String label,float value,float min,float max,String unit,Change action){super(c);this.label=label;this.min=min;this.max=max;this.unit=unit;this.value=Math.max(min,Math.min(max,value));this.action=action;touchSlop=ViewConfiguration.get(c).getScaledTouchSlop();setFocusable(true);describe();}
        long lastTapTime;float lastTapX,lastTapY;
        /** Knob face from the last draw: a vertical drag that starts on it turns the knob instead of scrolling the page. */
        float bodyX,bodyY,bodyR;boolean downOnBody;
        /** Double tap: type the exact value instead of dragging (hard to be precise on a phone). */
        void promptValue(){
            Context c=getContext();
            final EditText input=new EditText(c);
            input.setInputType(android.text.InputType.TYPE_CLASS_NUMBER|android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL|(min<0?android.text.InputType.TYPE_NUMBER_FLAG_SIGNED:0));
            input.setText(String.format(Locale.ROOT,"%.1f",value).replaceAll("\\.0$",""));
            input.setSelectAllOnFocus(true);input.setSingleLine(true);
            String range=String.format(Locale.ROOT,"%.0f a %.0f%s",min,max,unit);
            final android.app.AlertDialog dialog=new android.app.AlertDialog.Builder(c)
                .setTitle(label).setMessage("Digite o valor ("+range+")").setView(input)
                .setPositiveButton("OK",(d,w)->applyTyped(input.getText().toString()))
                .setNegativeButton("Cancelar",null).create();
            dialog.getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
            input.setOnEditorActionListener((v,a,ev)->{applyTyped(input.getText().toString());dialog.dismiss();return true;});
            dialog.show();
        }
        void applyTyped(String text){
            try{float typed=Float.parseFloat(text.trim().replace(',','.'));value=Math.max(min,Math.min(max,typed));action.set(value);describe();invalidate();}
            catch(NumberFormatException ignored){}
        }
        void describe(){setContentDescription(label+" "+format()+". Arraste na horizontal para ajustar; na vertical para rolar a tela.");}
        void setValue(float next){value=Math.max(min,Math.min(max,next));describe();invalidate();}
        String format(){return String.format(Locale.ROOT,"%.1f%s",value,unit);}
        @Override protected void onDraw(Canvas c){
            float w=getWidth(),h=getHeight(),cx=w/2,top=dp(getContext(),13),foot=dp(getContext(),18),cy=top+(h-top-foot)/2f,r=Math.max(dp(getContext(),10),Math.min(w*.32f,(h-top-foot)/2f-dp(getContext(),7)));p.setShader(null);p.setStyle(Paint.Style.FILL);p.setColor(TEXT);p.setTextAlign(Paint.Align.CENTER);p.setTextSize(dp(getContext(),8));c.drawText(label,cx,dp(getContext(),10),p);
            // Same metal knob as the desktop ClassicLookAndFeel: dark bezel, grey-to-black
            // gradient body, accent arc outside the rim and a pointer ending in a dot.
            bodyX=cx;bodyY=cy;bodyR=r;p.setColor(0xff080d11);c.drawCircle(cx,cy,r+dp(getContext(),3),p);
            p.setShader(new LinearGradient(cx-r*.7f,cy-r*.7f,cx+r*.7f,cy+r*.7f,0xff69747c,0xff171e23,Shader.TileMode.CLAMP));c.drawCircle(cx,cy,r,p);p.setShader(null);
            p.setStyle(Paint.Style.STROKE);p.setStrokeWidth(dp(getContext(),1));p.setColor(0xff88939a);c.drawCircle(cx,cy,r,p);
            float fraction=(value-min)/(max-min);float arcR=r+dp(getContext(),4);
            p.setColor(BORDER);p.setStrokeWidth(dp(getContext(),2.6f));p.setStrokeCap(Paint.Cap.ROUND);c.drawArc(new RectF(cx-arcR,cy-arcR,cx+arcR,cy+arcR),135,270,false,p);
            p.setColor(ACCENT);c.drawArc(new RectF(cx-arcR,cy-arcR,cx+arcR,cy+arcR),135,270*fraction,false,p);
            double angle=Math.toRadians(135+270*fraction);float px=cx+(float)Math.cos(angle)*r*.66f,py=cy+(float)Math.sin(angle)*r*.66f;
            p.setStrokeWidth(dp(getContext(),2.2f));c.drawLine(cx,cy,px,py,p);p.setStrokeCap(Paint.Cap.BUTT);
            p.setStyle(Paint.Style.FILL);c.drawCircle(px,py,dp(getContext(),2.2f),p);
            p.setStyle(Paint.Style.FILL);p.setColor(FIELD);float bottom=cy+r+dp(getContext(),2);c.drawRect(Math.max(3,cx-dp(getContext(),34)),bottom,Math.min(w-3,cx+dp(getContext(),34)),bottom+dp(getContext(),14),p);p.setColor(TEXT);p.setTextSize(dp(getContext(),9));c.drawText(format(),cx,bottom+dp(getContext(),11),p);
        }
        @Override public boolean onTouchEvent(MotionEvent e){switch(e.getActionMasked()){
            case MotionEvent.ACTION_DOWN:pointer=e.getPointerId(0);downX=e.getX();downY=e.getY();start=value;dragAxis=0;longPressed=false;downOnBody=bodyR>0&&Math.hypot(downX-bodyX,downY-bodyY)<=bodyR*1.45f+dp(getContext(),8);removeCallbacks(longPressTask);if(onLongPress!=null)postDelayed(longPressTask,ViewConfiguration.getLongPressTimeout());return true;
            case MotionEvent.ACTION_MOVE:int i=e.findPointerIndex(pointer);if(i<0)return true;if(longPressed)return true;float dx=e.getX(i)-downX,dy=downY-e.getY(i);if(Math.abs(dx)>touchSlop||Math.abs(dy)>touchSlop)removeCallbacks(longPressTask);if(dragAxis==0){if(Math.abs(dx)>touchSlop&&Math.abs(dx)>Math.abs(dy)){dragAxis=1;EditorUi.disallowIntercept(this,true);}else if(Math.abs(dy)>touchSlop){if(!downOnBody&&canScrollParent(dy)){return true;}dragAxis=2;EditorUi.disallowIntercept(this,true);}}if(dragAxis!=0){float delta=dragAxis==1?dx:dy;value=Math.max(min,Math.min(max,start+delta/dp(getContext(),180)*(max-min)));action.set(value);describe();invalidate();}return true;
            case MotionEvent.ACTION_UP:removeCallbacks(longPressTask);if(dragAxis==0&&!longPressed){performClick();long now=e.getEventTime();if(now-lastTapTime<=ViewConfiguration.getDoubleTapTimeout()&&Math.abs(e.getX()-lastTapX)<=touchSlop*3&&Math.abs(e.getY()-lastTapY)<=touchSlop*3){lastTapTime=0;promptValue();}else{lastTapTime=now;lastTapX=e.getX();lastTapY=e.getY();}}pointer=-1;dragAxis=0;EditorUi.disallowIntercept(this,false);return true;
            case MotionEvent.ACTION_CANCEL:removeCallbacks(longPressTask);pointer=-1;dragAxis=0;EditorUi.disallowIntercept(this,false);return true;
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
        /** Optional hook: lets a replacement graph follow the knobs that still feed this one. */
        Runnable onValues;
        void setValues(float a,float b,float c){this.a=a;this.b=b;this.c=c;invalidate();if(onValues!=null)onValues.run();}
        void setParametricValues(float lowFreq,float midFreq,float highFreq,float lowQ,float midQ,float highQ,float highPassHz,float lowPassHz){frequencies=new float[]{lowFreq,midFreq,highFreq};qs=new float[]{lowQ,midQ,highQ};highPass=highPassHz;lowPass=lowPassHz;invalidate();if(onValues!=null)onValues.run();}
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

    /** ADSR cycle drawing like the desktop "Gráfico do Ciclo ADSR": attack, decay, sustain while the key is held, release. */
    static final class EnvelopeGraph extends View {
        final Paint p=new Paint(Paint.ANTI_ALIAS_FLAG);float attackMs,decayMs,sustainPercent,releaseMs;final float maxAttack,maxRelease;
        EnvelopeGraph(Context c,float attackMs,float decayMs,float sustainPercent,float releaseMs,float maxAttack,float maxRelease){super(c);this.maxAttack=maxAttack;this.maxRelease=maxRelease;set(attackMs,decayMs,sustainPercent,releaseMs);}
        void set(float attackMs,float decayMs,float sustainPercent,float releaseMs){this.attackMs=attackMs;this.decayMs=decayMs;this.sustainPercent=sustainPercent;this.releaseMs=releaseMs;invalidate();}
        private void dashed(Canvas c,float x1,float y1,float x2,float y2,int colour){p.setStyle(Paint.Style.STROKE);p.setColor(colour);p.setStrokeWidth(dp(getContext(),1));p.setPathEffect(new android.graphics.DashPathEffect(new float[]{dp(getContext(),3.5f),dp(getContext(),3.5f)},0));c.drawLine(x1,y1,x2,y2,p);p.setPathEffect(null);}
        /** Layout, colours and stage weights follow the desktop AdsrEnvelopeGraph. */
        @Override protected void onDraw(Canvas c){
            Context k=getContext();float w=getWidth(),h=getHeight();
            p.setStyle(Paint.Style.FILL);p.setColor(PANEL);c.drawRoundRect(new RectF(0,0,w,h),dp(k,5),dp(k,5),p);
            p.setColor(TEXT);p.setTextSize(dp(k,15));p.setTextAlign(Paint.Align.LEFT);p.setFakeBoldText(true);c.drawText("Gráfico do Ciclo ADSR",dp(k,14),dp(k,22),p);p.setFakeBoldText(false);
            float l=dp(k,61)+dp(k,8),r=w-dp(k,20),t=dp(k,39),b=h-dp(k,52);
            double attackSeconds=attackMs*.001,releaseSeconds=releaseMs*.001;
            double[] weights={.8+.5*Math.sqrt(Math.max(0,attackSeconds)),.8+.5*Math.sqrt(Math.max(0,decayMs*.001)),2.3,.8+.35*Math.sqrt(Math.max(0,releaseSeconds))};
            double total=weights[0]+weights[1]+weights[2]+weights[3];float span=r-l;
            float x0=l,x1=x0+span*(float)(weights[0]/total),x2=x1+span*(float)(weights[1]/total),x3=x2+span*(float)(weights[2]/total),x4=r;
            float level=decayMs>0?Math.max(0f,Math.min(1f,sustainPercent*.01f)):1f;float sustainY=b-(b-t)*level;
            int grid=(BORDER&0x00ffffff)|0x88000000;
            dashed(c,l,t,r,t,grid);dashed(c,l,sustainY,r,sustainY,grid);for(float gx:new float[]{x1,x2,x3})dashed(c,gx,t,gx,b,grid);
            p.setStyle(Paint.Style.STROKE);p.setColor(BORDER);p.setStrokeWidth(dp(k,1));c.drawLine(l,b,r,b,p);
            p.setStyle(Paint.Style.FILL);p.setColor(MUTED);p.setTextSize(dp(k,10));p.setTextAlign(Paint.Align.RIGHT);
            if(Math.abs(sustainY-t)>dp(k,12))c.drawText("100%",l-dp(k,8),t+dp(k,4),p);c.drawText("Sustain",l-dp(k,8),sustainY+dp(k,4),p);c.drawText("0%",l-dp(k,8),b+dp(k,4),p);
            android.graphics.Path path=new android.graphics.Path();path.moveTo(x0,b);path.lineTo(x1,t);path.lineTo(x2,sustainY);path.lineTo(x3,sustainY);path.lineTo(x4,b);
            p.setStyle(Paint.Style.STROKE);p.setColor(0xff1682ff);p.setStrokeWidth(dp(k,3));p.setStrokeJoin(Paint.Join.ROUND);p.setStrokeCap(Paint.Cap.ROUND);c.drawPath(path,p);p.setStrokeCap(Paint.Cap.BUTT);
            p.setStyle(Paint.Style.FILL);p.setColor(0xffff493f);c.drawCircle(x1,t,dp(k,5),p);p.setColor(0xffffc400);c.drawCircle(x2,sustainY,dp(k,5),p);p.setColor(0xff38d76b);c.drawCircle(x3,sustainY,dp(k,5),p);
            p.setColor(TEXT);p.setTextSize(dp(k,10));p.setFakeBoldText(true);p.setTextAlign(Paint.Align.CENTER);float ly=b+dp(k,17);
            c.drawText("Ataque",(x0+x1)/2f,ly,p);c.drawText("Decaimento",(x1+x2)/2f,ly,p);c.drawText("Sustentação (Tecla Pressionada)",(x2+x3)/2f,ly,p);c.drawText("Liberação",(x3+x4)/2f,ly,p);
            float ny=ly+dp(k,17);p.setColor(0xff1682ff);p.setTextAlign(Paint.Align.LEFT);c.drawText("Note On",x0-dp(k,18),ny,p);
            p.setColor(0xffff493f);p.setTextAlign(Paint.Align.CENTER);c.drawText("Note Off",x3,ny,p);
            p.setFakeBoldText(false);p.setColor(MUTED);p.setTextAlign(Paint.Align.RIGHT);c.drawText("Tempo",x4+dp(k,4),ny,p);
        }
    }

    /** Velocity curve drawing: input key force (x) against the velocity the engine receives (y). Same formulas as the native engine. */
    static final class VelocityGraph extends View {
        final Paint p=new Paint(Paint.ANTI_ALIAS_FLAG);int curve;int soft,mid,hard,outMin=1,outMax=127;float slope;
        VelocityGraph(Context c,int curve,int soft,int mid,int hard){super(c);set(curve,soft,mid,hard);}
        void set(int curve,int soft,int mid,int hard){this.curve=curve;this.soft=soft;this.mid=mid;this.hard=hard;invalidate();}
        void setCurve(int curve,int soft,int mid,int hard,int outMin,int outMax,float slope){this.outMin=outMin;this.outMax=outMax;this.slope=slope;set(curve,soft,mid,hard);}
        float map(float in){
            int v=Math.max(1,Math.min(127,Math.round(in)));
            if(curve==4){float exponent=(float)Math.pow(2,-slope/50f),input=(v-1f)/126f;return Math.max(1,Math.min(127,Math.round(outMin+(outMax-outMin)*(float)Math.pow(input,exponent))));}
            if(curve==1)return Math.max(1,Math.min(127,Math.round((float)Math.sqrt(v/127.0f)*127.0f)));
            if(curve==2)return Math.max(1,Math.min(127,Math.round((v/127.0f)*(v/127.0f)*127.0f)));
            if(curve==3&&soft>0){int[] input={1,soft,mid,hard,127};int[] output={1,24,64,hard>=120?127:112,127};
                for(int point=1;point<5;point++)if(v<=input[point]){float f=(float)(v-input[point-1])/(float)Math.max(1,input[point]-input[point-1]);return Math.max(1,Math.min(127,Math.round(output[point-1]+f*(output[point]-output[point-1]))));}}
            return v;
        }
        @Override protected void onDraw(Canvas c){
            float w=getWidth(),h=getHeight(),l=dp(getContext(),30),r=w-dp(getContext(),14),t=dp(getContext(),28),b=h-dp(getContext(),24);
            p.setStyle(Paint.Style.FILL);p.setColor(FIELD);c.drawRoundRect(new RectF(0,0,w,h),dp(getContext(),5),dp(getContext(),5),p);
            p.setColor(MUTED);p.setTextSize(dp(getContext(),10));p.setTextAlign(Paint.Align.LEFT);c.drawText("CURVA DE VELOCITY",l,dp(getContext(),17),p);
            p.setStyle(Paint.Style.STROKE);p.setStrokeWidth(dp(getContext(),1));p.setColor(BORDER);
            for(int i=0;i<=4;i++){float x=l+(r-l)*i/4f,y=t+(b-t)*i/4f;c.drawLine(x,t,x,b,p);c.drawLine(l,y,r,y,p);}
            p.setColor(MUTED);c.drawLine(l,b,r,t,p);
            p.setColor(ACCENT);p.setStrokeWidth(dp(getContext(),2));float px=l,py=b;
            for(int i=1;i<=127;i++){float x=l+(r-l)*i/127f,y=b-(b-t)*map(i)/127f;if(i>1)c.drawLine(px,py,x,y,p);px=x;py=y;}
            p.setStyle(Paint.Style.FILL);p.setColor(MUTED);p.setTextSize(dp(getContext(),9));p.setTextAlign(Paint.Align.CENTER);
            c.drawText("força da tecla",(l+r)/2f,h-dp(getContext(),7),p);p.setTextAlign(Paint.Align.RIGHT);c.drawText("127",l-dp(getContext(),4),t+dp(getContext(),4),p);c.drawText("1",l-dp(getContext(),4),b,p);
        }
    }

    /** Desktop-style engine faceplate: coloured plate on the left, black LCD on the right. */
    static final class Faceplate extends View {
        private final Paint p=new Paint(Paint.ANTI_ALIAS_FLAG);
        private final int colour;private final String title,section,state,layerText,lcdCaption,lcdTitle,lcdSub;private final boolean loaded;
        Faceplate(Context c,int colour,String title,String section,boolean loaded,String state,String layerText,String lcdCaption,String lcdTitle,String lcdSub){
            super(c);this.colour=colour;this.title=title;this.section=section;this.loaded=loaded;this.state=state;this.layerText=layerText;this.lcdCaption=lcdCaption;this.lcdTitle=lcdTitle;this.lcdSub=lcdSub;}
        @Override protected void onMeasure(int w,int h){setMeasuredDimension(MeasureSpec.getSize(w),dp(getContext(),88));}
        @Override protected void onDraw(Canvas cv){
            float w=getWidth(),h=getHeight(),r=dp(getContext(),6);float d=dp(getContext(),1);
            p.setStyle(Paint.Style.FILL);p.setColor(0xff111722);cv.drawRoundRect(0,0,w,h,r,r,p);
            p.setColor(colour);cv.drawRoundRect(d*2,d*2,w-d*2,h-d*2,r,r,p);
            float split=Math.max(dp(getContext(),190),w*.30f);
            p.setColor(0xffffffff);p.setFakeBoldText(true);p.setTextSize(dp(getContext(),20));cv.drawText(title,dp(getContext(),16),dp(getContext(),32),p);
            p.setFakeBoldText(false);p.setTextSize(dp(getContext(),10));p.setColor(0xddffffff);cv.drawText(section,dp(getContext(),16),dp(getContext(),48),p);
            p.setColor(loaded?0xff39d98a:0xff1b2a3a);cv.drawCircle(dp(getContext(),21),dp(getContext(),63),dp(getContext(),4),p);
            p.setColor(0xddffffff);p.setTextSize(dp(getContext(),9));cv.drawText(state,dp(getContext(),32),dp(getContext(),66),p);
            cv.drawText(layerText,dp(getContext(),16),dp(getContext(),80),p);
            float lx=split,lt=dp(getContext(),9),lr=w-dp(getContext(),9),lb=h-dp(getContext(),9);
            p.setColor(0xff0b0e13);cv.drawRoundRect(lx,lt,lr,lb,r,r,p);
            p.setStyle(Paint.Style.STROKE);p.setStrokeWidth(d);p.setColor(0xff2a3342);cv.drawRoundRect(lx,lt,lr,lb,r,r,p);p.setStyle(Paint.Style.FILL);
            p.setColor(0xff9aa6b8);p.setTextSize(dp(getContext(),9));cv.drawText(lcdCaption,lx+dp(getContext(),14),lt+dp(getContext(),18),p);
            p.setColor(0xffffffff);p.setFakeBoldText(true);p.setTextSize(dp(getContext(),18));
            String t=lcdTitle;float maxW=lr-lx-dp(getContext(),28);while(t.length()>3&&p.measureText(t)>maxW)t=t.substring(0,t.length()-2);if(!t.equals(lcdTitle))t+="…";
            cv.drawText(t,lx+dp(getContext(),14),lt+dp(getContext(),44),p);
            p.setFakeBoldText(false);p.setColor(0xff7c889b);p.setTextSize(dp(getContext(),10));cv.drawText(lcdSub,lx+dp(getContext(),14),lt+dp(getContext(),60),p);
        }
    }
}
