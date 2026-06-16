#include "media_store.h"

#include <stdio.h>
#include <string.h>

static void set_err(char *err, size_t errLen, const char *msg) {
    if (err && errLen > 0) {
        snprintf(err, errLen, "%s", msg);
    }
}

static bool get_env(JavaVM *vm, JNIEnv **outEnv, bool *outAttached) {
    *outAttached = false;
    if (vm->GetEnv((void **)outEnv, JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(outEnv, NULL) != JNI_OK) {
            return false;
        }
        *outAttached = true;
    }
    return true;
}

static void detach_if_needed(JavaVM *vm, bool attached) {
    if (attached) {
        vm->DetachCurrentThread();
    }
}

static jobject new_string(JNIEnv *env, const char *value) {
    return env->NewStringUTF(value);
}

bool media_store_create_audio(JavaVM *vm, jobject activity,
                              const char *displayName, const char *mimeType,
                              MediaStoreHandle *outHandle,
                              char *err, size_t errLen) {
    if (!vm || !activity || !outHandle) {
        set_err(err, errLen, "MediaStore invalid params");
        return false;
    }
    outHandle->fd = -1;
    outHandle->uri_ref = NULL;
    outHandle->pfd_ref = NULL;

    JNIEnv *env = NULL;
    bool attached = false;
    if (!get_env(vm, &env, &attached)) {
        set_err(err, errLen, "JNI attach failed");
        return false;
    }

    jclass activityCls = env->GetObjectClass(activity);
    jmethodID getContentResolver = env->GetMethodID(activityCls,
                                                       "getContentResolver",
                                                       "()Landroid/content/ContentResolver;");
    jobject resolver = env->CallObjectMethod(activity, getContentResolver);

    jclass valuesCls = env->FindClass("android/content/ContentValues");
    jmethodID valuesCtor = env->GetMethodID(valuesCls, "<init>", "()V");
    jobject values = env->NewObject(valuesCls, valuesCtor);
    jmethodID putString = env->GetMethodID(valuesCls, "put",
                                              "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID putInt = env->GetMethodID(valuesCls, "put",
                                           "(Ljava/lang/String;Ljava/lang/Integer;)V");
    jclass integerCls = env->FindClass("java/lang/Integer");
    jmethodID integerCtor = env->GetMethodID(integerCls, "<init>", "(I)V");

    jobject keyDisplay = new_string(env, "_display_name");
    jobject keyMime = new_string(env, "mime_type");
    jobject keyRelPath = new_string(env, "relative_path");
    jobject keyPending = new_string(env, "is_pending");
    jobject valueDisplay = new_string(env, displayName);
    jobject valueMime = new_string(env, mimeType);
    jobject valueRelPath = new_string(env, "Music/BGMDWLDR");
    jobject valuePending = env->NewObject(integerCls, integerCtor, 1);

    env->CallVoidMethod(values, putString, keyDisplay, valueDisplay);
    env->CallVoidMethod(values, putString, keyMime, valueMime);
    env->CallVoidMethod(values, putString, keyRelPath, valueRelPath);
    env->CallVoidMethod(values, putInt, keyPending, valuePending);

    jclass mediaCls = env->FindClass("android/provider/MediaStore$Audio$Media");
    jfieldID externalField = env->GetStaticFieldID(mediaCls,
                                                      "EXTERNAL_CONTENT_URI",
                                                      "Landroid/net/Uri;");
    jobject externalUri = env->GetStaticObjectField(mediaCls, externalField);

    jclass resolverCls = env->GetObjectClass(resolver);
    jmethodID insertMethod = env->GetMethodID(resolverCls, "insert",
                                                 "(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;");
    jobject uri = env->CallObjectMethod(resolver, insertMethod,
                                           externalUri, values);
    if (!uri) {
        detach_if_needed(vm, attached);
        set_err(err, errLen, "MediaStore insert failed");
        return false;
    }

    jmethodID openPfd = env->GetMethodID(resolverCls, "openFileDescriptor",
                                            "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
    jobject mode = new_string(env, "w");
    jobject pfd = env->CallObjectMethod(resolver, openPfd, uri, mode);
    if (!pfd) {
        detach_if_needed(vm, attached);
        set_err(err, errLen, "OpenFileDescriptor failed");
        return false;
    }
    jclass pfdCls = env->GetObjectClass(pfd);
    jmethodID getFd = env->GetMethodID(pfdCls, "getFd", "()I");
    int fd = env->CallIntMethod(pfd, getFd);

    outHandle->fd = fd;
    /* Store JNI global refs directly */
    outHandle->uri_ref = env->NewGlobalRef(uri);
    outHandle->pfd_ref = env->NewGlobalRef(pfd);
    detach_if_needed(vm, attached);
    return true;
}

bool media_store_finalize(JavaVM *vm, jobject activity,
                          MediaStoreHandle *handle,
                          char *err, size_t errLen) {
    if (!vm || !activity || !handle || handle->uri_ref == NULL) {
        set_err(err, errLen, "MediaStore finalize invalid params");
        return false;
    }
    JNIEnv *env = NULL;
    bool attached = false;
    if (!get_env(vm, &env, &attached)) {
        set_err(err, errLen, "JNI attach failed");
        return false;
    }
    jclass activityCls = env->GetObjectClass(activity);
    jmethodID getContentResolver = env->GetMethodID(activityCls,
                                                       "getContentResolver",
                                                       "()Landroid/content/ContentResolver;");
    jobject resolver = env->CallObjectMethod(activity, getContentResolver);

    jclass valuesCls = env->FindClass("android/content/ContentValues");
    jmethodID valuesCtor = env->GetMethodID(valuesCls, "<init>", "()V");
    jobject values = env->NewObject(valuesCls, valuesCtor);
    jmethodID putInt = env->GetMethodID(valuesCls, "put",
                                           "(Ljava/lang/String;Ljava/lang/Integer;)V");
    jclass integerCls = env->FindClass("java/lang/Integer");
    jmethodID integerCtor = env->GetMethodID(integerCls, "<init>", "(I)V");
    jobject keyPending = new_string(env, "is_pending");
    jobject valuePending = env->NewObject(integerCls, integerCtor, 0);
    env->CallVoidMethod(values, putInt, keyPending, valuePending);

    jclass resolverCls = env->GetObjectClass(resolver);
    jmethodID updateMethod = env->GetMethodID(resolverCls, "update",
                                                 "(Landroid/net/Uri;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;)I");
    env->CallIntMethod(resolver, updateMethod,
                          handle->uri_ref, values, NULL, NULL);

    detach_if_needed(vm, attached);
    return true;
}

void media_store_close(JavaVM *vm, MediaStoreHandle *handle) {
    if (!vm || !handle) {
        return;
    }
    JNIEnv *env = NULL;
    bool attached = false;
    if (!get_env(vm, &env, &attached)) {
        return;
    }
    if (handle->pfd_ref != NULL) {
        jclass pfdCls = env->GetObjectClass(handle->pfd_ref);
        jmethodID closeMethod = env->GetMethodID(pfdCls, "close", "()V");
        env->CallVoidMethod(handle->pfd_ref, closeMethod);
        env->DeleteGlobalRef(handle->pfd_ref);
        handle->pfd_ref = NULL;
    }
    if (handle->uri_ref != NULL) {
        env->DeleteGlobalRef(handle->uri_ref);
        handle->uri_ref = NULL;
    }
    detach_if_needed(vm, attached);
    handle->fd = -1;
}

bool media_store_get_uri_string(JavaVM *vm, MediaStoreHandle *handle,
                                char *out, size_t outLen) {
    if (!vm || !handle || handle->uri_ref == NULL || !out || outLen == 0) {
        return false;
    }
    JNIEnv *env = NULL;
    bool attached = false;
    if (!get_env(vm, &env, &attached)) {
        return false;
    }
    jclass uriCls = env->FindClass("android/net/Uri");
    jmethodID toString = env->GetMethodID(uriCls, "toString", "()Ljava/lang/String;");
    jstring uriStr = (jstring)env->CallObjectMethod(handle->uri_ref, toString);
    if (uriStr) {
        const char *utf = env->GetStringUTFChars(uriStr, NULL);
        snprintf(out, outLen, "%s", utf);
        env->ReleaseStringUTFChars(uriStr, utf);
        env->DeleteLocalRef(uriStr);
    } else {
        snprintf(out, outLen, "content://unknown");
    }
    detach_if_needed(vm, attached);
    return true;
}
