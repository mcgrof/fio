LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := fio
LOCAL_SRC_FILES := fio_jni.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../../../
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)
