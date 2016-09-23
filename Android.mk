# Copyright (C) 2010 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


# We're moving the emulator-specific platform libs to
# development.git/tools/emulator/. The following test is to ensure
# smooth builds even if the tree contains both versions.
#

LOCAL_PATH := $(call my-dir)

# HAL module implemenation stored in
# hw/<GPS_HARDWARE_MODULE_ID>.<ro.hardware>.so
include $(CLEAR_VARS)

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_SHARED_LIBRARIES := liblog libcutils libhardware
LOCAL_CFLAGS := -O2 -fpermissive -Wmissing-field-initializers
LOCAL_SRC_FILES := gps_goby.cpp
LOCAL_MODULE := gps.goby
LOCAL_MODULE_TAGS := debug

include $(BUILD_SHARED_LIBRARY)
#############################################
include $(CLEAR_VARS)

LOCAL_SRC_FILES := local_gps.cpp
LOCAL_PBUF_INTERMEDIATES := $(call intermediates-dir-for,SHARED_LIBRARIES,libcppsensors_packet,,)/proto/external/aic/libaicd/

LOCAL_C_INCLUDES	:= bionic \
				   external/stlport/stlport \
				   external/aic/libaicd \
				   external/protobuf/src \
				   $(LOCAL_PBUF_INTERMEDIATES)

IGNORED_WARNINGS := -Wno-sign-compare -Wno-unused-parameter -Wno-sign-promo -Wno-error=return-type
LOCAL_CFLAGS := -O2 -fpermissive -Wmissing-field-initializers -DGOOGLE_PROTOBUF_NO_RTTI

LOCAL_MODULE := local_gps
LOCAL_SHARED_LIBRARIES := liblog libcutils libstlport libcppsensors_packet
LOCAL_STATIC_LIBRARIES += libprotobuf-cpp-2.3.0-lite libprotobuf-cpp-2.3.0-full
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)
