#!/bin/bash



NDK_HOME=/home/netresults.wintranet/benfatti/Android/Sdk/ndk/23.1.7779620
TARGET_ARCH=$1
SRC_DIR=/home/netresults.wintranet/benfatti/portaudio_oboe-master/

cmake -DCMAKE_TOOLCHAIN_FILE=${NDK_HOME}/build/cmake/android.toolchain.cmake -DANDROID_ABI=${TARGET_ARCH} -DANDROID_PLATFORM=android-30 -S ${SRC_DIR} -B ${SRC_DIR}/Build_${TARGET_ARCH}
