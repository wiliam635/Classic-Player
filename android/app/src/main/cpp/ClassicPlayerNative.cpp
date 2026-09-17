#include <jni.h>

// Placeholder bridge for the FluidSynth-backed renderer. Keeping this JNI
// library in the APK gives the Java engine a stable native entry point while
// the workflow supplies the SoundFont implementation for the target ABI.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_classickeys_classicplayer_MainActivity_nativeSoundFontAvailable(JNIEnv*, jobject)
{
    return JNI_FALSE;
}
