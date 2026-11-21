package com.classickeys.classicplayer;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/** Large interactive EQ and compressor graphs for the docked inspector. They draw the engine's own transfer functions. */
final class EffectGraphs {
    private EffectGraphs() {}
    static final int CYAN = 0xff2bb8e6;

    /** Compressor transfer curve with the desktop CompressorResponseView layout: -60..+6 dB input, yellow
     *  threshold line and handles, estimated input / gain-reduction / output meters. Drag the threshold point
     *  sideways and the +6 dB point vertically. */
    static final class CompressorGraph extends View {
        interface Listener { void changed(float thresholdDb, float ratio); void released(); }
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        float thresholdDb = -18f, ratio = 4f, makeupDb = 0f, mix = 0f;
        /** Live sources polled at ~15 Hz while the graph is on screen. */
        java.util.function.Supplier<Float> peakSource, mixSource;
        Listener listener;
        private int drag = -1;
        private final Runnable tick = new Runnable() { @Override public void run() { if (isAttachedToWindow()) { invalidate(); postDelayed(this, 66); } } };
        CompressorGraph(Context c) { super(c); }
        @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); postDelayed(tick, 66); }
        @Override protected void onDetachedFromWindow() { removeCallbacks(tick); super.onDetachedFromWindow(); }
        void set(float thresholdLinear, float ratio, float makeupDb) {
            this.thresholdDb = (float) Math.max(-60.0, Math.min(0.0, 20.0 * Math.log10(Math.max(.001f, thresholdLinear))));
            this.ratio = Math.max(1f, ratio); this.makeupDb = makeupDb; invalidate();
        }
        private float dp(float v) { return EditorUi.dp(getContext(), v); }
        private RectF area() { return new RectF(dp(18), dp(24), getWidth() - dp(18), getHeight() - dp(24)); }
        private RectF plot() { RectF a = area(); return new RectF(a.left + dp(74), a.top + dp(4), a.right - dp(74), a.bottom - dp(31)); }
        private float x(RectF r, float db) { return r.left + r.width() * (Math.max(-60f, Math.min(6f, db)) + 60f) / 66f; }
        private float y(RectF r, float db) { return r.bottom - r.height() * (Math.max(-60f, Math.min(12f, db)) + 60f) / 72f; }
        private float transfer(float in, float threshold, float rat, float makeup, float m) {
            float over = Math.max(0f, in - threshold), reduction = over * (1f - 1f / rat);
            float gain = (1f - m) + m * (float) Math.pow(10.0, (makeup - reduction) / 20.0);
            return in + 20f * (float) Math.log10(Math.max(1e-6f, gain));
        }
        private void meter(Canvas c, float mx, float my, float mh, float width, float amount, int colour, String label) {
            p.setStyle(Paint.Style.FILL); p.setColor(0xff090d0f); c.drawRect(mx, my, mx + width, my + mh, p);
            p.setColor(colour); c.drawRect(mx, my + mh * (1f - Math.max(0f, Math.min(1f, amount))), mx + width, my + mh, p);
            p.setStyle(Paint.Style.STROKE); p.setColor(EditorUi.MUTED); p.setStrokeWidth(dp(1)); c.drawRect(mx, my, mx + width, my + mh, p);
            p.setStyle(Paint.Style.FILL); p.setColor(EditorUi.MUTED); p.setTextSize(dp(8)); p.setFakeBoldText(true); p.setTextAlign(Paint.Align.CENTER);
            c.drawText(label, mx + width / 2f, my + mh + dp(11), p); p.setFakeBoldText(false);
        }
        @Override protected void onDraw(Canvas c) {
            Context k = getContext(); RectF area = area(), r = plot(); float w = getWidth(), h = getHeight();
            if (mixSource != null) { Float m = mixSource.get(); if (m != null) mix = Math.max(0f, Math.min(1f, m)); }
            float peak = peakSource != null && peakSource.get() != null ? peakSource.get() : 0f;
            float outputDb = Math.max(-60f, Math.min(6f, 20f * (float) Math.log10(Math.max(1e-6f, peak))));
            float inputDb = outputDb;
            for (int i = 0; i < 6; i++) inputDb = Math.max(-60f, Math.min(6f, inputDb + outputDb - transfer(inputDb, thresholdDb, ratio, makeupDb, mix)));
            float reduction = Math.max(0f, inputDb - transfer(inputDb, thresholdDb, ratio, 0f, mix));
            p.setStyle(Paint.Style.FILL); p.setColor(0xff101617); c.drawRoundRect(new RectF(0, 0, w, h), dp(6), dp(6), p);
            p.setColor(0xff161d20); c.drawRoundRect(area, dp(10), dp(10), p);
            p.setStyle(Paint.Style.STROKE); p.setColor(0xff303a3c); p.setStrokeWidth(dp(1)); c.drawRoundRect(area, dp(10), dp(10), p);
            p.setStyle(Paint.Style.FILL); p.setTextSize(dp(9)); p.setColor(EditorUi.MUTED);
            for (int db = -60; db <= 0; db += 15) {
                float gx = x(r, db), gy = y(r, db);
                p.setStyle(Paint.Style.STROKE); p.setColor(0xff354346); c.drawLine(gx, r.top, gx, r.bottom, p); c.drawLine(r.left, gy, r.right, gy, p);
                p.setStyle(Paint.Style.FILL); p.setColor(EditorUi.MUTED); p.setTextAlign(Paint.Align.CENTER); c.drawText(String.valueOf(db), gx, r.bottom + dp(12), p);
            }
            p.setStyle(Paint.Style.STROKE); p.setColor(0xff657578); c.drawLine(x(r, -60), y(r, -60), x(r, 6), y(r, 6), p);
            Path curve = new Path();
            for (int i = 0; i <= 120; i++) { float in = -60f + 66f * i / 120f; float px = x(r, in), py = y(r, transfer(in, thresholdDb, ratio, makeupDb, mix)); if (i == 0) curve.moveTo(px, py); else curve.lineTo(px, py); }
            Path fill = new Path(curve); fill.lineTo(x(r, 6), y(r, -60)); fill.lineTo(x(r, -60), y(r, -60)); fill.close();
            p.setStyle(Paint.Style.FILL); p.setColor((EditorUi.ACCENT & 0x00ffffff) | 0x38000000); c.drawPath(fill, p);
            p.setStyle(Paint.Style.STROKE); p.setColor(EditorUi.ACCENT); p.setStrokeWidth(dp(2)); c.drawPath(curve, p);
            float tx = x(r, thresholdDb);
            p.setColor(EditorUi.HIGHLIGHT); p.setStrokeWidth(dp(1.5f)); c.drawLine(tx, r.top, tx, r.bottom, p);
            float[][] handles = {{tx, y(r, transfer(thresholdDb, thresholdDb, ratio, makeupDb, mix))}, {x(r, 6), y(r, transfer(6f, thresholdDb, ratio, makeupDb, mix))}};
            for (float[] hnd : handles) {
                p.setStyle(Paint.Style.FILL); p.setColor(EditorUi.HIGHLIGHT); c.drawCircle(hnd[0], hnd[1], dp(8), p);
                p.setColor(0xff101617); c.drawCircle(hnd[0], hnd[1], dp(3), p);
            }
            p.setStyle(Paint.Style.FILL); p.setColor(EditorUi.TEXT); c.drawCircle(x(r, inputDb), y(r, outputDb), dp(4.5f), p);
            float my = area.top + dp(25), mh = area.height() - dp(58);
            meter(c, area.left + dp(18), my, mh, dp(18), (inputDb + 60f) / 60f, 0xff4ac0aa, "INPUT");
            meter(c, area.left + dp(48), my, mh, dp(12), reduction / 24f, EditorUi.HIGHLIGHT, "GR");
            meter(c, area.right - dp(45), my, mh, dp(18), (outputDb + 60f) / 60f, 0xff4ac0aa, "OUTPUT");
            p.setStyle(Paint.Style.FILL); p.setColor(EditorUi.TEXT); p.setTextSize(dp(12)); p.setTextAlign(Paint.Align.CENTER); p.setFakeBoldText(true);
            c.drawText("COMPRESSOR DA LAYER", area.centerX(), dp(18), p); p.setFakeBoldText(false);
            p.setTextSize(dp(9)); p.setColor(EditorUi.MUTED); c.drawText("CURVA", r.centerX(), area.bottom - dp(4), p);
            p.setColor(EditorUi.HIGHLIGHT); c.drawText(String.format(java.util.Locale.ROOT, "%.1f dB GR EST.", reduction), r.centerX(), area.top + dp(16), p);
        }
        @Override public boolean onTouchEvent(MotionEvent e) {
            RectF r = plot(); float reach = dp(36);
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: {
                    float d1 = (float) Math.hypot(e.getX() - x(r, thresholdDb), e.getY() - y(r, transfer(thresholdDb, thresholdDb, ratio, makeupDb, mix)));
                    float d2 = (float) Math.hypot(e.getX() - x(r, 6), e.getY() - y(r, transfer(6f, thresholdDb, ratio, makeupDb, mix)));
                    drag = Math.min(d1, d2) > reach ? -1 : (d1 <= d2 ? 0 : 1);
                    if (drag >= 0) EditorUi.disallowIntercept(this,true);
                    return drag >= 0; }
                case MotionEvent.ACTION_MOVE: {
                    if (drag < 0) return false;
                    if (drag == 0) thresholdDb = Math.max(-60f, Math.min(-1f, -60f + 66f * (e.getX() - r.left) / r.width()));
                    else { float outDb = -60f + 72f * (r.bottom - e.getY()) / r.height() - makeupDb; float gain = outDb - thresholdDb; float span = 6f - thresholdDb; ratio = Math.max(1f, Math.min(20f, gain <= .05f ? 20f : span / gain)); }
                    if (listener != null) listener.changed(thresholdDb, ratio);
                    invalidate(); return true; }
                case MotionEvent.ACTION_UP: case MotionEvent.ACTION_CANCEL:
                    if (drag >= 0 && listener != null) listener.released();
                    drag = -1; EditorUi.disallowIntercept(this,false); return true;
            }
            return true;
        }
    }

    /** Parametric EQ response (3 peaking bands plus 2nd-order high-pass and low-pass, exactly the engine's filters). Drag numbered points for frequency and gain. */
    static final class EqGraph extends View {
        interface Listener { void changed(int band, float frequency, float gainDb); void released(); }
        static final float SAMPLE_RATE = 44100f;
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        final float[] freq = {220, 1200, 4200}, gain = {0, 0, 0}, q = {.707f, 1f, .707f};
        float highPass = 20f, lowPass = 20000f;
        static final float[] MIN_F = {40, 60, 1000}, MAX_F = {2000, 12000, 20000};
        Listener listener;
        private int drag = -1;
        EqGraph(Context c) { super(c); }
        void set(float[] f, float[] g, float[] qq, float hp, float lp) { for (int i = 0; i < 3; i++) { freq[i] = f[i]; gain[i] = g[i]; q[i] = qq[i]; } highPass = hp; lowPass = lp; invalidate(); }
        private RectF plot() { Context k = getContext(); return new RectF(EditorUi.dp(k, 40), EditorUi.dp(k, 34), getWidth() - EditorUi.dp(k, 18), getHeight() - EditorUi.dp(k, 34)); }
        private float x(RectF r, float f) { return r.left + r.width() * (float) (Math.log10(Math.max(20f, Math.min(20000f, f)) / 20.0) / 3.0); }
        private float y(RectF r, float db) { return r.top + r.height() * (.5f - Math.max(-18f, Math.min(18f, db)) / 36f); }
        private static double mag2(double b0, double b1, double b2, double a0, double a1, double a2, double w) {
            double c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
            double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2), dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
            return (nr * nr + ni * ni) / Math.max(1e-30, dr * dr + di * di);
        }
        float responseDb(float f) {
            double w = 2 * Math.PI * f / SAMPLE_RATE, db = 0;
            for (int b = 0; b < 3; b++) {
                double ww = 2 * Math.PI * Math.max(20f, Math.min(20000f, freq[b])) / SAMPLE_RATE, cosine = Math.cos(ww), alpha = Math.sin(ww) / (2.0 * Math.max(.1f, Math.min(20f, q[b]))), a = Math.pow(10.0, Math.max(-18f, Math.min(18f, gain[b])) / 40.0);
                db += 10 * Math.log10(mag2(1 + alpha * a, -2 * cosine, 1 - alpha * a, 1 + alpha / a, -2 * cosine, 1 - alpha / a, w));
            }
            double wh = 2 * Math.PI * Math.max(20f, highPass) / SAMPLE_RATE, ch = Math.cos(wh), ah = Math.sin(wh) / (2 * .70710678);
            db += 10 * Math.log10(mag2((1 + ch) * .5, -(1 + ch), (1 + ch) * .5, 1 + ah, -2 * ch, 1 - ah, w));
            double wl = 2 * Math.PI * Math.min(20000f, lowPass) / SAMPLE_RATE, cl = Math.cos(wl), al = Math.sin(wl) / (2 * .70710678);
            db += 10 * Math.log10(mag2((1 - cl) * .5, 1 - cl, (1 - cl) * .5, 1 + al, -2 * cl, 1 - al, w));
            return (float) db;
        }
        @Override protected void onDraw(Canvas c) {
            Context k = getContext(); RectF r = plot(); float w = getWidth(), h = getHeight();
            p.setStyle(Paint.Style.FILL); p.setColor(0xff0a1118); c.drawRoundRect(new RectF(0, 0, w, h), EditorUi.dp(k, 6), EditorUi.dp(k, 6), p);
            p.setColor(0xff25323c); c.drawRect(r, p);
            p.setColor(EditorUi.TEXT); p.setTextSize(EditorUi.dp(k, 12)); p.setTextAlign(Paint.Align.CENTER); p.setFakeBoldText(true); c.drawText("EQ PARAMETRICO DA LAYER", w / 2f, EditorUi.dp(k, 20), p); p.setFakeBoldText(false);
            p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(EditorUi.dp(k, 1)); p.setColor(0xff33434e);
            for (int db = -18; db <= 18; db += 6) { float gy = y(r, db); c.drawLine(r.left, gy, r.right, gy, p); }
            float[] marks = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
            for (float m : marks) { float gx = x(r, m); c.drawLine(gx, r.top, gx, r.bottom, p); }
            p.setStyle(Paint.Style.FILL); p.setColor(EditorUi.MUTED); p.setTextSize(EditorUi.dp(k, 10));
            p.setTextAlign(Paint.Align.RIGHT); for (int db = -18; db <= 18; db += 6) c.drawText(String.valueOf(db), r.left - EditorUi.dp(k, 5), y(r, db) + EditorUi.dp(k, 4), p);
            p.setTextAlign(Paint.Align.CENTER); for (float m : marks) c.drawText(m >= 1000 ? (m / 1000f == (int) (m / 1000f) ? String.valueOf((int) (m / 1000f)) : String.valueOf(m / 1000f)) + "k" : String.valueOf((int) m), x(r, m), r.bottom + EditorUi.dp(k, 14), p);
            c.drawText("Arraste os pontos para ajustar frequência e ganho", r.centerX(), h - EditorUi.dp(k, 6), p);
            p.setStyle(Paint.Style.STROKE); p.setColor(0xff60727e); p.setStrokeWidth(EditorUi.dp(k, 1)); c.drawLine(r.left, y(r, 0), r.right, y(r, 0), p);
            Path curve = new Path();
            for (int i = 0; i <= 240; i++) { float f = (float) (20.0 * Math.pow(1000.0, i / 240.0)); float px = x(r, f), py = y(r, responseDb(f)); if (i == 0) curve.moveTo(px, py); else curve.lineTo(px, py); }
            Path area = new Path(curve); area.lineTo(r.right, y(r, 0)); area.lineTo(r.left, y(r, 0)); area.close();
            p.setStyle(Paint.Style.FILL); p.setColor((EditorUi.ACCENT & 0x00ffffff) | 0x2e000000); c.drawPath(area, p);
            p.setStyle(Paint.Style.STROKE); p.setColor(EditorUi.ACCENT); p.setStrokeWidth(EditorUi.dp(k, 2.2f)); c.drawPath(curve, p);
            for (int b = 0; b < 3; b++) {
                float px = x(r, freq[b]), py = y(r, gain[b]);
                p.setStyle(Paint.Style.FILL); p.setColor(0xffd9e4e8); c.drawCircle(px, py, EditorUi.dp(k, 8), p);
                p.setStyle(Paint.Style.STROKE); p.setColor(EditorUi.BACKGROUND); p.setStrokeWidth(EditorUi.dp(k, 1)); c.drawCircle(px, py, EditorUi.dp(k, 8), p);
                p.setStyle(Paint.Style.FILL); p.setColor(EditorUi.BACKGROUND); p.setTextAlign(Paint.Align.CENTER); p.setFakeBoldText(true); p.setTextSize(EditorUi.dp(k, 10)); c.drawText(String.valueOf(b + 1), px, py + EditorUi.dp(k, 4), p); p.setFakeBoldText(false);
            }
        }
        @Override public boolean onTouchEvent(MotionEvent e) {
            RectF r = plot(); float reach = EditorUi.dp(getContext(), 40);
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: {
                    float best = reach; drag = -1;
                    for (int b = 0; b < 3; b++) { float d = (float) Math.hypot(e.getX() - x(r, freq[b]), e.getY() - y(r, gain[b])); if (d < best) { best = d; drag = b; } }
                    if (drag >= 0) EditorUi.disallowIntercept(this,true);
                    return drag >= 0; }
                case MotionEvent.ACTION_MOVE: {
                    if (drag < 0) return false;
                    float f = (float) (20.0 * Math.pow(1000.0, Math.max(0f, Math.min(1f, (e.getX() - r.left) / r.width()))));
                    freq[drag] = Math.max(MIN_F[drag], Math.min(MAX_F[drag], f));
                    gain[drag] = Math.max(-18f, Math.min(18f, (.5f - (e.getY() - r.top) / r.height()) * 36f));
                    if (listener != null) listener.changed(drag, freq[drag], gain[drag]);
                    invalidate(); return true; }
                case MotionEvent.ACTION_UP: case MotionEvent.ACTION_CANCEL:
                    if (drag >= 0 && listener != null) listener.released();
                    drag = -1; EditorUi.disallowIntercept(this,false); return true;
            }
            return true;
        }
    }

    /** Nine vertical drawbars (8 at the top, 0 at the bottom) with Hammond cap colours, like the desktop panel. */
    static final class Drawbars extends View {
        interface Listener { void changed(int bar, int value); }
        static final String[] NAMES = {"16 ft", "5 1/3 ft", "8 ft", "4 ft", "2 2/3 ft", "2 ft", "1 3/5 ft", "1 1/3 ft", "1 ft"};
        static final int[] CAPS = {0xffc4302b, 0xffc4302b, 0xfff0ece4, 0xfff0ece4, 0xff262626, 0xfff0ece4, 0xff262626, 0xff262626, 0xfff0ece4};
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        final int[] values = new int[9];
        Listener listener;
        private int drag = -1;
        Drawbars(Context c) { super(c); }
        void set(int[] v) { for (int i = 0; i < 9; i++) values[i] = Math.max(0, Math.min(8, v[i])); invalidate(); }
        private float trackTop() { return EditorUi.dp(getContext(), 30); }
        private float trackBottom() { return getHeight() - EditorUi.dp(getContext(), 34); }
        private float yOf(int value) { return trackBottom() - (trackBottom() - trackTop()) * value / 8f; }
        @Override protected void onDraw(Canvas c) {
            Context k = getContext(); float w = getWidth(), colW = w / 9f, top = trackTop(), bottom = trackBottom();
            for (int i = 0; i < 9; i++) {
                float cx = colW * (i + .5f);
                p.setStyle(Paint.Style.FILL); p.setColor(EditorUi.FIELD); c.drawRoundRect(new RectF(cx - colW * .46f, 0, cx + colW * .46f, getHeight()), EditorUi.dp(k, 5), EditorUi.dp(k, 5), p);
                p.setColor(EditorUi.TEXT); p.setTextAlign(Paint.Align.CENTER); p.setTextSize(EditorUi.dp(k, 10)); p.setFakeBoldText(true); c.drawText(NAMES[i], cx, EditorUi.dp(k, 16), p); p.setFakeBoldText(false);
                p.setColor(0xff0c0e11); c.drawRoundRect(new RectF(cx - EditorUi.dp(k, 4), top, cx + EditorUi.dp(k, 4), bottom), EditorUi.dp(k, 4), EditorUi.dp(k, 4), p);
                p.setColor(EditorUi.MUTED); p.setTextSize(EditorUi.dp(k, 8)); p.setTextAlign(Paint.Align.LEFT);
                for (int n = 0; n <= 8; n++) c.drawText(String.valueOf(n), cx + EditorUi.dp(k, 12), yOf(n) + EditorUi.dp(k, 3), p);
                float cy = yOf(values[i]), capW = Math.min(colW * .34f, EditorUi.dp(k, 26)), capH = EditorUi.dp(k, 22);
                p.setStyle(Paint.Style.FILL); p.setColor(CAPS[i]); c.drawRoundRect(new RectF(cx - capW, cy - capH / 2f, cx + capW, cy + capH / 2f), EditorUi.dp(k, 3), EditorUi.dp(k, 3), p);
                p.setColor(0x55000000); c.drawRect(cx - capW + EditorUi.dp(k, 3), cy - EditorUi.dp(k, 1), cx + capW - EditorUi.dp(k, 3), cy + EditorUi.dp(k, 1), p);
                p.setColor(EditorUi.BACKGROUND); c.drawRoundRect(new RectF(cx - EditorUi.dp(k, 16), bottom + EditorUi.dp(k, 6), cx + EditorUi.dp(k, 16), bottom + EditorUi.dp(k, 26)), EditorUi.dp(k, 3), EditorUi.dp(k, 3), p);
                p.setColor(EditorUi.TEXT); p.setTextAlign(Paint.Align.CENTER); p.setTextSize(EditorUi.dp(k, 11)); c.drawText(String.valueOf(values[i]), cx, bottom + EditorUi.dp(k, 21), p);
            }
        }
        @Override public boolean onTouchEvent(MotionEvent e) {
            float colW = getWidth() / 9f;
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN) { drag = Math.max(0, Math.min(8, (int) (e.getX() / colW))); EditorUi.disallowIntercept(this,true); }
            if (drag < 0) return false;
            if (e.getActionMasked() == MotionEvent.ACTION_UP || e.getActionMasked() == MotionEvent.ACTION_CANCEL) { drag = -1; EditorUi.disallowIntercept(this,false); return true; }
            float fraction = (trackBottom() - e.getY()) / Math.max(1f, trackBottom() - trackTop());
            int v = Math.max(0, Math.min(8, Math.round(fraction * 8f)));
            if (v != values[drag]) { values[drag] = v; if (listener != null) listener.changed(drag, v); invalidate(); }
            return true;
        }
    }
}
