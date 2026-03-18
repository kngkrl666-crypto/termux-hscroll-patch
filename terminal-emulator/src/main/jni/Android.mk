LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE:= libtermux
LOCAL_SRC_FILES:= termux.c
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE:= libtermux-linker-exec
LOCAL_SRC_FILES:= termux-linker-exec.c
LOCAL_LDLIBS:= -ldl -llog
include $(BUILD_SHARED_LIBRARY)
