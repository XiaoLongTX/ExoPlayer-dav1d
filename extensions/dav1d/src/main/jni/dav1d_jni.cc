/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <cstdint>
#include <cstring>
#include <mutex> // NOLINT
#include <new>

#include "include/dav1d.h"

#define LOG_TAG "dav1d_jni"
#define LOGE(...) \
    ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#define LOGI(...) \
    ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))

#define DECODER_FUNC(RETURN_TYPE, NAME, ...)                               \
    extern "C"                                                             \
    {                                                                      \
        JNIEXPORT RETURN_TYPE                                              \
            Java_com_google_android_exoplayer2_ext_dav1d_Gav1Decoder_##NAME( \
                JNIEnv *env, jobject thiz, ##__VA_ARGS__);                 \
    }                                                                      \
    JNIEXPORT RETURN_TYPE                                                  \
        Java_com_google_android_exoplayer2_ext_dav1d_Gav1Decoder_##NAME(     \
            JNIEnv *env, jobject thiz, ##__VA_ARGS__)

jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
  JNIEnv *env;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
  {
    return -1;
  }
  return JNI_VERSION_1_6;
}

namespace
{

// YUV plane indices.
const int kPlaneY = 0;
const int kPlaneU = 1;
const int kPlaneV = 2;
const int kMaxPlanes = 3;

// Android YUV format. See:
// https://developer.android.com/reference/android/graphics/ImageFormat.html#YV12.
const int kImageFormatYV12 = 0x32315659;

// Output modes.
const int kOutputModeYuv = 0;
const int kOutputModeSurfaceYuv = 1;

const int kColorSpaceUnknown = 0;

// Return codes for jni methods.
const int kStatusError = 0;
const int kStatusOk = 1;
const int kStatusDecodeOnly = 2;

// Status codes specific to the JNI wrapper code.
enum JniStatusCode
{
  kJniStatusOk = 0,
  kJniStatusOutOfMemory = -1,
  kJniStatusBufferAlreadyReleased = -2,
  kJniStatusInvalidNumOfPlanes = -3,
  kJniStatusBitDepth12NotSupportedWithYuv = -4,
  kJniStatusHighBitDepthNotSupportedWithSurfaceYuv = -5,
  kJniStatusANativeWindowError = -6,
  kJniStatusBufferResizeError = -7,
  kJniStatusNeonNotSupported = -8
};

const char *GetJniErrorMessage(JniStatusCode error_code)
{
  switch (error_code)
  {
    case kJniStatusOutOfMemory:
      return "Out of memory.";
    case kJniStatusBufferAlreadyReleased:
      return "JNI buffer already released.";
    case kJniStatusBitDepth12NotSupportedWithYuv:
      return "Bit depth 12 is not supported with YUV.";
    case kJniStatusHighBitDepthNotSupportedWithSurfaceYuv:
      return "High bit depth (10 or 12 bits per pixel) output format is not "
             "supported with YUV surface.";
    case kJniStatusInvalidNumOfPlanes:
      return "Libgav1 decoded buffer has invalid number of planes.";
    case kJniStatusANativeWindowError:
      return "ANativeWindow error.";
    case kJniStatusBufferResizeError:
      return "Buffer resize failed.";
    case kJniStatusNeonNotSupported:
      return "Neon is not supported.";
    default:
      return "Unrecognized error code.";
  }
}

// Manages frame buffer and reference information.
class JniFrameBuffer
{
 public:
  explicit JniFrameBuffer(int id) : id_(id), reference_count_(0) {}

  ~JniFrameBuffer()
  {
    for (int plane_index = kPlaneY; plane_index < kMaxPlanes; plane_index++)
    {
      delete[] raw_buffer_[plane_index];
    }
  }

  // Not copyable or movable.
  JniFrameBuffer(const JniFrameBuffer &) = delete;

  JniFrameBuffer(JniFrameBuffer &&) = delete;

  JniFrameBuffer &operator=(const JniFrameBuffer &) = delete;

  JniFrameBuffer &operator=(JniFrameBuffer &&) = delete;

  void SetFrameData(const DAV1D_API::Dav1dPicture &decoder_buffer)
  {
    for (int plane_index = kPlaneY; plane_index < 3; plane_index++)
    {
      if (plane_index == 0 || plane_index == 1)
      {
        stride_[plane_index] = decoder_buffer.stride[plane_index];
      }
      else
      {
        stride_[plane_index] = decoder_buffer.stride[1];
      }
      plane_[plane_index] = static_cast<uint8_t *>(decoder_buffer.data[plane_index]);
      if (plane_index == 0)
      {
        displayed_width_[plane_index] = decoder_buffer.p.w;
        displayed_height_[plane_index] = decoder_buffer.p.h;
      }
      else
      {
        displayed_width_[plane_index] = decoder_buffer.p.w / 2;
        displayed_height_[plane_index] = decoder_buffer.p.h / 2;
      }
    }
  }

  int Stride(int plane_index) const { return stride_[plane_index]; }

  uint8_t *Plane(int plane_index) const { return plane_[plane_index]; }

  int DisplayedWidth(int plane_index) const
  {
    return displayed_width_[plane_index];
  }

  int DisplayedHeight(int plane_index) const
  {
    return displayed_height_[plane_index];
  }

  // Methods maintaining reference count are not thread-safe. They must be
  // called with a lock held.
  void AddReference() { ++reference_count_; }

  void RemoveReference() { reference_count_--; }

  bool InUse() const { return reference_count_ != 0; }

  uint8_t *RawBuffer(int plane_index) const { return raw_buffer_[plane_index]; }

  int *BufferPrivateData() const { return const_cast<int *>(&id_); }

  // Attempts to reallocate data planes if the existing ones don't have enough
  // capacity. Returns true if the allocation was successful or wasn't needed,
  // false if the allocation failed.
  bool MaybeReallocateGav1DataPlanes(int y_plane_min_size,
                                     int uv_plane_min_size)
  {
    for (int plane_index = kPlaneY; plane_index < kMaxPlanes; plane_index++)
    {
      const int min_size =
          (plane_index == kPlaneY) ? y_plane_min_size : uv_plane_min_size;
      if (raw_buffer_size_[plane_index] >= min_size)
        continue;
      delete[] raw_buffer_[plane_index];
      raw_buffer_[plane_index] = new (std::nothrow) uint8_t[min_size];
      if (!raw_buffer_[plane_index])
      {
        raw_buffer_size_[plane_index] = 0;
        return false;
      }
      raw_buffer_size_[plane_index] = min_size;
    }
    return true;
  }

 private:
  int stride_[kMaxPlanes];
  uint8_t *plane_[kMaxPlanes];
  int displayed_width_[kMaxPlanes];
  int displayed_height_[kMaxPlanes];
  const int id_;
  int reference_count_;
  // Pointers to the raw buffers allocated for the data planes.
  uint8_t *raw_buffer_[kMaxPlanes] = {};
  // Sizes of the raw buffers in bytes.
  size_t raw_buffer_size_[kMaxPlanes] = {};
};

// Manages frame buffers used by libgav1 decoder and ExoPlayer.
// Handles synchronization between libgav1 and ExoPlayer threads.
class JniBufferManager
{
 public:
  ~JniBufferManager()
  {
    // This lock does not do anything since libgav1 has released all the frame
    // buffers. It exists to merely be consistent with all other usage of
    // |all_buffers_| and |all_buffer_count_|.
    std::lock_guard<std::mutex> lock(mutex_);
    while (all_buffer_count_--)
    {
      delete all_buffers_[all_buffer_count_];
    }
  }

  JniStatusCode GetBuffer(size_t y_plane_min_size, size_t uv_plane_min_size,
                          JniFrameBuffer **jni_buffer)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    JniFrameBuffer *output_buffer;
    if (free_buffer_count_)
    {
      output_buffer = free_buffers_[--free_buffer_count_];
    }
    else if (all_buffer_count_ < kMaxFrames)
    {
      output_buffer = new (std::nothrow) JniFrameBuffer(all_buffer_count_);
      if (output_buffer == nullptr)
        return kJniStatusOutOfMemory;
      all_buffers_[all_buffer_count_++] = output_buffer;
    }
    else
    {
      // Maximum number of buffers is being used.
      return kJniStatusOutOfMemory;
    }
    if (!output_buffer->MaybeReallocateGav1DataPlanes(y_plane_min_size,
                                                      uv_plane_min_size))
    {
      return kJniStatusOutOfMemory;
    }

    output_buffer->AddReference();
    *jni_buffer = output_buffer;

    return kJniStatusOk;
  }

  JniFrameBuffer *GetBuffer(int id) const { return all_buffers_[id]; }

  void AddBufferReference(int id)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    all_buffers_[id]->AddReference();
  }

  JniStatusCode ReleaseBuffer(int id)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    JniFrameBuffer *buffer = all_buffers_[id];
    if (!buffer->InUse())
    {
      return kJniStatusBufferAlreadyReleased;
    }
    buffer->RemoveReference();
    if (!buffer->InUse())
    {
      free_buffers_[free_buffer_count_++] = buffer;
    }
    return kJniStatusOk;
  }

 private:
  static const int kMaxFrames = 32;

  JniFrameBuffer *all_buffers_[kMaxFrames];
  int all_buffer_count_ = 0;

  JniFrameBuffer *free_buffers_[kMaxFrames];
  int free_buffer_count_ = 0;

  std::mutex mutex_;
};

struct JniContext
{
  ~JniContext()
  {
    if (native_window)
    {
      ANativeWindow_release(native_window);
    }
  }

  bool MaybeAcquireNativeWindow(JNIEnv *env, jobject new_surface)
  {
    if (surface == new_surface)
    {
      return true;
    }
    if (native_window)
    {
      ANativeWindow_release(native_window);
    }
    native_window_width = 0;
    native_window_height = 0;
    native_window = ANativeWindow_fromSurface(env, new_surface);
    if (native_window == nullptr)
    {
      jni_status_code = kJniStatusANativeWindowError;
      surface = nullptr;
      return false;
    }
    surface = new_surface;
    return true;
  }

  jfieldID decoder_private_field;
  jfieldID output_mode_field;
  jfieldID data_field;
  jmethodID init_for_private_frame_method;
  jmethodID init_for_yuv_frame_method;

  JniBufferManager buffer_manager;

  Dav1dContext *c_out;

  ANativeWindow *native_window = nullptr;
  jobject surface = nullptr;
  int native_window_width = 0;
  int native_window_height = 0;

  int avid_status_code = kJniStatusOk;
  JniStatusCode jni_status_code = kJniStatusOk;
};

constexpr int AlignTo16(int value) { return (value + 15) & (~15); }

void CopyPlane(const uint8_t *source, int source_stride, uint8_t *destination,
               int destination_stride, int width, int height)
{
//        LOGI(
//                "CopyPlane  %d,  %d,  %d,  %d,  %d,  %d",
//                *source,
//                source_stride,
//                *destination,
//                destination_stride,
//                width,
//                height);

  while (height--)
  {
    std::memcpy(destination, source, width);
    source += source_stride;
    destination += destination_stride;
  }
}

void CopyFrameToDataBuffer(const DAV1D_API::Dav1dPicture *decoder_buffer,
                           jbyte *data)
{
  for (int plane_index = kPlaneY; plane_index < 3;
       plane_index++)
  {
    const uint64_t length = sizeof decoder_buffer->data[plane_index];
    LOGI("CopyFrameToDataBuffer %lu", length);
    memcpy(data, decoder_buffer->data[plane_index], length);
    data += length;
  }
}

void Convert10BitFrameTo8BitDataBuffer(
    const DAV1D_API::Dav1dPicture *decoder_buffer, jbyte *data)
{
  LOGI("Convert10BitFrameTo8BitDataBuffer");
  for (int plane_index = kPlaneY; plane_index < 3;
       plane_index++)
  {
    int sample = 0;
    const auto *source = static_cast<const uint8_t *>(decoder_buffer->data[plane_index]);
    for (int i = 0; i < decoder_buffer->p.h; i++)
    {
      const auto *source_16 = reinterpret_cast<const uint16_t *>(source);
      for (int j = 0; j < decoder_buffer->p.w; j++)
      {
        // Lightweight dither. Carryover the remainder of each 10->8 bit
        // conversion to the next pixel.
        sample += source_16[j];
        data[j] = sample >> 2;
        sample &= 3; // Remainder.
      }
      source += decoder_buffer->stride[plane_index];
      data += decoder_buffer->stride[plane_index];
    }
  }
}

void libdav1d_data_free(const uint8_t *data, void *opaque)
{
  //        AVBufferRef *buf = opaque;
  //
  //        av_buffer_unref(&buf);
}
} // namespace

DECODER_FUNC(jlong, gav1Init, jint threads)
{
  JniContext *context = new (std::nothrow) JniContext();
  if (context == nullptr)
  {
    return kStatusError;
  }
  LOGI("init %p", &context->buffer_manager);
  DAV1D_API::Dav1dSettings settings;
  dav1d_default_settings(&settings);
  context->avid_status_code = dav1d_open(&(context->c_out), &settings);
  if (context->avid_status_code != kJniStatusOk)
  {
    LOGI("dav1d_open %d", context->avid_status_code);
    return reinterpret_cast<jlong>(context);
  }

  // Populate JNI References.
  const jclass outputBufferClass = env->FindClass(
      "com/google/android/exoplayer2/decoder/VideoDecoderOutputBuffer");
  context->decoder_private_field =
      env->GetFieldID(outputBufferClass, "decoderPrivate", "I");
  context->output_mode_field = env->GetFieldID(outputBufferClass, "mode", "I");
  context->data_field =
      env->GetFieldID(outputBufferClass, "data", "Ljava/nio/ByteBuffer;");
  context->init_for_private_frame_method =
      env->GetMethodID(outputBufferClass, "initForPrivateFrame", "(II)V");
  context->init_for_yuv_frame_method =
      env->GetMethodID(outputBufferClass, "initForYuvFrame", "(IIIII)Z");
  return reinterpret_cast<jlong>(context);
}

DECODER_FUNC(void, gav1Close, jlong jContext)
{
  auto *const context = reinterpret_cast<JniContext *>(jContext);
  delete context;
}

DECODER_FUNC(jint, gav1Decode, jlong jContext, jobject encodedData,
             jint length)
{
  auto *const context = reinterpret_cast<JniContext *>(jContext);
  const auto *const buffer = reinterpret_cast<const uint8_t *>(
      env->GetDirectBufferAddress(encodedData));
  Dav1dData data;
  context->avid_status_code = dav1d_data_wrap(&data, buffer, length, libdav1d_data_free, data.ref);

  if (context->avid_status_code != kJniStatusOk)
  {
    LOGI("dav1d_data_wrap %d", context->avid_status_code);
    return kStatusError;
  }

  context->avid_status_code = dav1d_send_data(context->c_out, &data);

  if (context->avid_status_code != kJniStatusOk &&
      context->avid_status_code != DAV1D_ERR(EAGAIN))
  {
    LOGI("dav1d_send_data %d", context->avid_status_code);
    return kStatusError;
  }

  return kStatusOk;
}

DECODER_FUNC(jint, gav1GetFrame, jlong jContext, jobject jOutputBuffer,
             jboolean decodeOnly)
{
  auto *const context = reinterpret_cast<JniContext *>(jContext);
  Dav1dPicture pic = {0}, *p = &pic;

  context->avid_status_code = dav1d_get_picture(context->c_out, p);
  if (context->avid_status_code != kJniStatusOk)
  {
    LOGI("dav1d_get_picture %d", context->avid_status_code);
    return kStatusError;
  }

  if (decodeOnly != 0)
  {
    // This is not an error. The input data was decode-only or no displayable
    // frames are available.
    return kStatusDecodeOnly;
  }

  JniFrameBuffer *jni_buffer;
  context->jni_status_code = context->buffer_manager.GetBuffer(p->stride[0],
                                                               p->stride[1],
                                                               &jni_buffer);
  //    LOGI("宽 %d，高 %d，y %td，uv %td,bit %d", p->p.w, p->p.h,
  //         p->stride[0], p->stride[1], p->p.bpc);
  if (context->jni_status_code != kJniStatusOk)
  {
    LOGE("GetBuffer %s", GetJniErrorMessage(context->jni_status_code));
    return kStatusError;
  }

  const int output_mode =
      env->GetIntField(jOutputBuffer, context->output_mode_field);
  if (output_mode == kOutputModeYuv)
  {
    // Resize the buffer if required. Default color conversion will be used as
    // libgav1::DecoderBuffer doesn't expose color space info.
    const jboolean init_result = env->CallBooleanMethod(
        jOutputBuffer,
        context->init_for_yuv_frame_method,
        p->p.w,
        p->p.h,
        p->stride[kPlaneY],
        p->stride[kPlaneU],
        kColorSpaceUnknown);
    if (env->ExceptionCheck())
    {
      // Exception is thrown in Java when returning from the native call.
      return kStatusError;
    }
    if (!init_result)
    {
      context->jni_status_code = kJniStatusBufferResizeError;
      return kStatusError;
    }

    const jobject data_object =
        env->GetObjectField(jOutputBuffer, context->data_field);
    auto *const data =
        reinterpret_cast<jbyte *>(env->GetDirectBufferAddress(data_object));
    switch (p->p.bpc)
    {
      case 8:
        CopyFrameToDataBuffer(p, data);
        break;
      case 10:
        Convert10BitFrameTo8BitDataBuffer(p, data);
        break;
      default:
        context->jni_status_code = kJniStatusBitDepth12NotSupportedWithYuv;
        return kStatusError;
    }
  }
  else if (output_mode == kOutputModeSurfaceYuv)
  {
    if (p->p.bpc != 8)
    {
      context->jni_status_code =
          kJniStatusHighBitDepthNotSupportedWithSurfaceYuv;
      return kStatusError;
    }
    jni_buffer->SetFrameData(*p);
    env->CallVoidMethod(jOutputBuffer, context->init_for_private_frame_method,
                        p->p.w,
                        p->p.h);
    if (env->ExceptionCheck())
    {
      // Exception is thrown in Java when returning from the native call.gav1GetFrame
      return kStatusError;
    }
    env->SetIntField(jOutputBuffer, context->decoder_private_field,
                     *(jni_buffer->BufferPrivateData()));
  }

  return kStatusOk;
}

DECODER_FUNC(jint, gav1RenderFrame, jlong jContext, jobject jSurface,
             jobject jOutputBuffer)
{
  auto *const context = reinterpret_cast<JniContext *>(jContext);
  const int buffer_id =
      env->GetIntField(jOutputBuffer, context->decoder_private_field);

  JniFrameBuffer *const jni_buffer =
      context->buffer_manager.GetBuffer(buffer_id);
  if (!context->MaybeAcquireNativeWindow(env, jSurface))
  {
    return kStatusError;
  }

  if (context->native_window_width != jni_buffer->DisplayedWidth(kPlaneY) ||
      context->native_window_height != jni_buffer->DisplayedHeight(kPlaneY))
  {
    if (ANativeWindow_setBuffersGeometry(
        context->native_window, jni_buffer->DisplayedWidth(kPlaneY),
        jni_buffer->DisplayedHeight(kPlaneY), kImageFormatYV12))
    {
      context->jni_status_code = kJniStatusANativeWindowError;
      return kStatusError;
    }
    context->native_window_width = jni_buffer->DisplayedWidth(kPlaneY);
    context->native_window_height = jni_buffer->DisplayedHeight(kPlaneY);
  }

  ANativeWindow_Buffer native_window_buffer;
  if (ANativeWindow_lock(context->native_window, &native_window_buffer,
      /*inOutDirtyBounds=*/nullptr) ||
      native_window_buffer.bits == nullptr)
  {
    context->jni_status_code = kJniStatusANativeWindowError;
    return kStatusError;
  }

  // Y plane
  CopyPlane(jni_buffer->Plane(kPlaneY),
            jni_buffer->Stride(kPlaneY),
            reinterpret_cast<uint8_t *>(native_window_buffer.bits),
            native_window_buffer.stride,
            jni_buffer->DisplayedWidth(kPlaneY),
            jni_buffer->DisplayedHeight(kPlaneY));

  const int y_plane_size =
      native_window_buffer.stride * native_window_buffer.height;
  const int32_t native_window_buffer_uv_height =
      (native_window_buffer.height + 1) / 2;
  const int native_window_buffer_uv_stride =
      AlignTo16(native_window_buffer.stride / 2);

  // TODO(b/140606738): Handle monochrome videos.

  // V plane
  // Since the format for ANativeWindow is YV12, V plane is being processed
  // before U plane.
  const int v_plane_height =
      std::min(native_window_buffer_uv_height, jni_buffer->DisplayedHeight(kPlaneV));

  CopyPlane(
      jni_buffer->Plane(kPlaneV),
      jni_buffer->Stride(kPlaneV),
      reinterpret_cast<uint8_t *>(native_window_buffer.bits) + y_plane_size,
      native_window_buffer_uv_stride, jni_buffer->DisplayedWidth(kPlaneV),
      v_plane_height);

  const int v_plane_size = v_plane_height * native_window_buffer_uv_stride;


  // U plane
  CopyPlane(jni_buffer->Plane(kPlaneU),
            jni_buffer->Stride(kPlaneU),
            reinterpret_cast<uint8_t *>(native_window_buffer.bits) + y_plane_size + v_plane_size,
            native_window_buffer_uv_stride,
            jni_buffer->DisplayedWidth(kPlaneU),
            std::min(native_window_buffer_uv_height, jni_buffer->DisplayedHeight(kPlaneU)));

  if (ANativeWindow_unlockAndPost(context->native_window))
  {
    context->jni_status_code = kJniStatusANativeWindowError;
    return kStatusError;
  }

  return kStatusOk;
}

DECODER_FUNC(void, gav1ReleaseFrame, jlong jContext, jobject jOutputBuffer)
{
  JniContext *const context = reinterpret_cast<JniContext *>(jContext);
  const int buffer_id =
      env->GetIntField(jOutputBuffer, context->decoder_private_field);
  env->SetIntField(jOutputBuffer, context->decoder_private_field, -1);
  if (buffer_id < 0) {
    return;
  }
  context->jni_status_code = context->buffer_manager.ReleaseBuffer(buffer_id);
  if (context->jni_status_code != kJniStatusOk)
  {
    LOGE("%s", GetJniErrorMessage(context->jni_status_code));
  }
}

DECODER_FUNC(jstring, gav1GetErrorMessage, jlong jContext)
{
  if (jContext == 0)
  {
    return env->NewStringUTF("Failed to initialize JNI context.");
  }

  JniContext *const context = reinterpret_cast<JniContext *>(jContext);
  if (context->avid_status_code != kJniStatusOk)
  {
    return env->NewStringUTF(reinterpret_cast<const char *>(context->avid_status_code));
  }
  if (context->jni_status_code != kJniStatusOk)
  {
    return env->NewStringUTF(GetJniErrorMessage(context->jni_status_code));
  }

  return env->NewStringUTF("None.");
}

DECODER_FUNC(jint, gav1CheckError, jlong jContext)
{
  JniContext *const context = reinterpret_cast<JniContext *>(jContext);
  if (context->avid_status_code != kJniStatusOk ||
      context->jni_status_code != kJniStatusOk)
  {
    return kStatusError;
  }
  return kStatusOk;
}

DECODER_FUNC(jint, gav1GetThreads)
{
  return 0;
}