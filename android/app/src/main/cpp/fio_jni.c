#include <jni.h>
#include "../../../../fio.h"

JNIEXPORT jstring JNICALL
Java_com_fio_FioActivity_runFio(JNIEnv *env, jobject thiz)
{
    char *argv[] = { "fio", "--name=test", "--ioengine=null", "--size=1M" };
    fio_main(4, argv, NULL);
    return (*env)->NewStringUTF(env, "fio run complete");
}
