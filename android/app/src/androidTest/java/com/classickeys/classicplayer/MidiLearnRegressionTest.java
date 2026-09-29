package com.classickeys.classicplayer;

import android.content.Context;
import android.content.SharedPreferences;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.util.ArrayList;
import java.util.List;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class MidiLearnRegressionTest {
    private static final class Message {
        final int status, data1, data2;
        final long timestamp;
        Message(int status, int data1, int data2, long timestamp) {
            this.status=status;this.data1=data1;this.data2=data2;this.timestamp=timestamp;
        }
    }

    private MidiByteParser parser;
    private List<Message> messages;
    private SharedPreferences preferences;

    @Before public void prepare() {
        parser = new MidiByteParser();
        messages = new ArrayList<>();
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        preferences = context.getSharedPreferences("midi_learn_test", Context.MODE_PRIVATE);
        preferences.edit().clear().commit();
    }

    private void accept(byte[] data, long timestamp) {
        parser.accept(data, 0, data.length, timestamp,
                (status, data1, data2, time) -> messages.add(new Message(status,data1,data2,time)));
    }

    @Test public void controllerValuesSurviveSplitPacketsAndRunningStatus() {
        accept(new byte[]{(byte)0xb4,72}, 100);
        assertTrue(messages.isEmpty());
        accept(new byte[]{0,(byte)0xb4,72,1,72,63,72,64,72,127}, 200);
        assertEquals(5,messages.size());
        int[] expected={0,1,63,64,127};
        for(int i=0;i<expected.length;i++){
            Message message=messages.get(i);
            assertEquals(0xb4,message.status);
            assertEquals(72,message.data1);
            assertEquals(expected[i],message.data2);
        }
        assertEquals("timestamp comes from the callback completing the message",200,messages.get(0).timestamp);
    }

    @Test public void realtimeBytesDoNotBreakControllerDataOrRunningStatus() {
        accept(new byte[]{(byte)0xb4,72,(byte)0xf8,64,72,(byte)0xfa,127}, 8);
        assertEquals(2,messages.size());
        assertEquals(64,messages.get(0).data2);
        assertEquals(127,messages.get(1).data2);
    }

    @Test public void parserResetPreventsStaleRunningStatusAcrossConnections() {
        accept(new byte[]{(byte)0xb4,72}, 1);
        parser.reset();
        accept(new byte[]{1,72,2}, 2);
        assertTrue(messages.isEmpty());
        accept(new byte[]{(byte)0xb4,72,3}, 3);
        assertEquals(1,messages.size());assertEquals(3,messages.get(0).data2);
    }

    @Test public void noteOnOffAndRunningStatusAreParsedAcrossCalls() {
        accept(new byte[]{(byte)0x94,60}, 10);
        accept(new byte[]{100,60,0,61,127,61,0}, 11);
        assertEquals(4,messages.size());
        assertEquals(0x94,messages.get(0).status);assertEquals(60,messages.get(0).data1);assertEquals(100,messages.get(0).data2);
        assertEquals(0x94,messages.get(1).status);assertEquals(0,messages.get(1).data2);
        assertEquals(61,messages.get(2).data1);assertEquals(127,messages.get(2).data2);
        assertEquals(0,messages.get(3).data2);
    }

    @Test public void learnPersistsAndMatchesTheExactCcAndOneBasedDisplayedChannel() {
        MidiLearnMapping.save(preferences,2,3,72,4); // MIDI channel nibble 4 is channel 5 to musicians.
        MidiLearnMapping learned=MidiLearnMapping.load(preferences,2,3);
        assertEquals(72,learned.cc);assertEquals(4,learned.channel);
        assertEquals("CC 72 CH 5",learned.label());
        assertTrue(learned.matches(72,4));
        assertFalse(learned.matches(72,3));
        assertFalse(learned.matches(71,4));
        assertFalse(learned.matches(72,5));
    }

    @Test public void channelBoundariesAndTargetsStayIndependent() {
        MidiLearnMapping.save(preferences,0,0,1,0);
        MidiLearnMapping.save(preferences,5,4,127,15);
        MidiLearnMapping first=MidiLearnMapping.load(preferences,0,0);
        MidiLearnMapping last=MidiLearnMapping.load(preferences,5,4);
        assertEquals("CC 1 CH 1",first.label());assertTrue(first.matches(1,0));assertFalse(first.matches(1,1));
        assertEquals("CC 127 CH 16",last.label());assertTrue(last.matches(127,15));assertFalse(last.matches(127,14));
        assertFalse(MidiLearnMapping.load(preferences,0,4).matches(1,0));
    }

    @Test public void oneByteChannelMessagesAreConsumedWithoutCorruptingFollowingCc() {
        accept(new byte[]{(byte)0xc5,10,(byte)0xb5,7,99,(byte)0xd5,64},20);
        assertEquals(3,messages.size());
        assertEquals(0xc5,messages.get(0).status);assertEquals(10,messages.get(0).data1);assertEquals(-1,messages.get(0).data2);
        assertEquals(0xb5,messages.get(1).status);assertEquals(7,messages.get(1).data1);assertEquals(99,messages.get(1).data2);
        assertEquals(0xd5,messages.get(2).status);assertEquals(64,messages.get(2).data1);assertEquals(-1,messages.get(2).data2);
    }

    @Test public void oldMappingsWithoutChannelRemainWildcardAndClearRemovesBothFields() {
        String key=MidiLearnMapping.key(0,1);
        preferences.edit().putInt(key,74).apply();
        MidiLearnMapping legacy=MidiLearnMapping.load(preferences,0,1);
        assertTrue(legacy.matches(74,0));
        assertTrue(legacy.matches(74,15));
        assertEquals("CC 74",legacy.label());
        MidiLearnMapping.clear(preferences,0,1);
        assertFalse(MidiLearnMapping.load(preferences,0,1).matches(74,0));
        assertFalse(preferences.contains(key+"_channel"));
    }

    @Test public void allSevenBitControllerValuesMapAbsolutelyToNormalizedParameterRange() {
        int[] values={0,1,63,64,127};
        float[] expected={0f,1f/127f,63f/127f,64f/127f,1f};
        for(int i=0;i<values.length;i++)assertEquals(expected[i],MidiLearnMapping.normalizedValue(values[i]),0.000001f);
        assertEquals(0f,MidiLearnMapping.normalizedValue(-1),0f);
        assertEquals(1f,MidiLearnMapping.normalizedValue(128),0f);
    }
}
