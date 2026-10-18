/* Screenshot compression (image.c). */
#ifndef RD_IMAGE_H
#define RD_IMAGE_H

#include <jni.h>

/* Registers Native.hasImageEncoder / Native.encodeImage; < 0 on failure. */
int image_register(JNIEnv *env, jclass cls);

#endif
