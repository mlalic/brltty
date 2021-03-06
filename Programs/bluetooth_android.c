/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2016 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any
 * later version. Please see the file LICENSE-GPL for details.
 *
 * Web Page: http://brltty.com/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

#include "prologue.h"

#include <errno.h>

#include "io_bluetooth.h"
#include "bluetooth_internal.h"
#include "log.h"
#include "io_misc.h"
#include "thread.h"
#include "system_java.h"

struct BluetoothConnectionExtensionStruct {
  JNIEnv *env;

  jclass connectionClass;
  jmethodID connectionConstructor;
  jmethodID openMethod;
  jmethodID closeMethod;
  jmethodID writeMethod;

  jobject connection;
  AsyncHandle inputMonitor;
  int inputPipe[2];
};

static void
releaseConnectionClass (BluetoothConnectionExtension *bcx) {
  (*bcx->env)->DeleteGlobalRef(bcx->env, bcx->connectionClass);
}

BluetoothConnectionExtension *
bthNewConnectionExtension (uint64_t bda) {
  BluetoothConnectionExtension *bcx;

  if ((bcx = malloc(sizeof(*bcx)))) {
    memset(bcx, 0, sizeof(*bcx));

    bcx->connectionClass = NULL;
    bcx->connectionConstructor = 0;
    bcx->openMethod = 0;
    bcx->closeMethod = 0;
    bcx->writeMethod = 0;

    bcx->inputPipe[0] = INVALID_FILE_DESCRIPTOR;
    bcx->inputPipe[1] = INVALID_FILE_DESCRIPTOR;

    if ((bcx->env = getJavaNativeInterface())) {
      if (findJavaClass(bcx->env, &bcx->connectionClass, "org/a11y/brltty/android/BluetoothConnection")) {
        if (findJavaConstructor(bcx->env, &bcx->connectionConstructor, bcx->connectionClass,
                                JAVA_SIG_CONSTRUCTOR(
                                                     JAVA_SIG_LONG // deviceAddress
                                                    ))) {
          jobject localReference = (*bcx->env)->NewObject(bcx->env, bcx->connectionClass, bcx->connectionConstructor, bda);

          if (!clearJavaException(bcx->env, 1)) {
            jobject globalReference = (*bcx->env)->NewGlobalRef(bcx->env, localReference);

            (*bcx->env)->DeleteLocalRef(bcx->env, localReference);
            localReference = NULL;

            if (globalReference) {
              bcx->connection = globalReference;
              return bcx;
            } else {
              logMallocError();
              clearJavaException(bcx->env, 0);
            }
          }
        }

        releaseConnectionClass(bcx);
      }
    }

    free(bcx);
  } else {
    logMallocError();
  }

  return NULL;
}

static void
bthCancelInputMonitor (BluetoothConnectionExtension *bcx) {
  if (bcx->inputMonitor) {
    asyncCancelRequest(bcx->inputMonitor);
    bcx->inputMonitor = NULL;
  }
}

void
bthReleaseConnectionExtension (BluetoothConnectionExtension *bcx) {
  bthCancelInputMonitor(bcx);

  if (bcx->connection) {
    if (findJavaInstanceMethod(bcx->env, &bcx->closeMethod, bcx->connectionClass, "close",
                               JAVA_SIG_METHOD(JAVA_SIG_VOID, ))) {
      (*bcx->env)->CallVoidMethod(bcx->env, bcx->connection, bcx->closeMethod);
    }

    (*bcx->env)->DeleteGlobalRef(bcx->env, bcx->connection);
    releaseConnectionClass(bcx);
    clearJavaException(bcx->env, 1);
  }

  closeFile(&bcx->inputPipe[0]);
  closeFile(&bcx->inputPipe[1]);

  free(bcx);
}

typedef struct {
  BluetoothConnectionExtension *const bcx;
  uint8_t const channel;
  int const timeout;

  int error;
} OpenBluetoothConnectionData;

THREAD_FUNCTION(runOpenBluetoothConnection) {
  OpenBluetoothConnectionData *obc = argument;
  JNIEnv *env;

  if ((env = getJavaNativeInterface())) {
    if (pipe(obc->bcx->inputPipe) != -1) {
      if (setBlockingIo(obc->bcx->inputPipe[0], 0)) {
        if (findJavaInstanceMethod(env, &obc->bcx->openMethod, obc->bcx->connectionClass, "open",
                                   JAVA_SIG_METHOD(JAVA_SIG_BOOLEAN,
                                                   JAVA_SIG_INT // inputPipe
                                                   JAVA_SIG_INT // channel
                                                   JAVA_SIG_BOOLEAN // secure
                                                  ))) {
          jboolean result = (*env)->CallBooleanMethod(env, obc->bcx->connection, obc->bcx->openMethod,
                                                      obc->bcx->inputPipe[1], obc->channel, JNI_FALSE);

          if (!clearJavaException(env, 1)) {
            if (result == JNI_TRUE) {
              closeFile(&obc->bcx->inputPipe[1]);
              obc->error = 0;
              goto done;
            }
          }

          errno = EIO;
        }
      }

      closeFile(&obc->bcx->inputPipe[0]);
      closeFile(&obc->bcx->inputPipe[1]);
    } else {
      logSystemError("pipe");
    }
  }

  obc->error = errno;
done:
  return NULL;
}

int
bthOpenChannel (BluetoothConnectionExtension *bcx, uint8_t channel, int timeout) {
  OpenBluetoothConnectionData obc = {
    .bcx = bcx,
    .channel = channel,
    .timeout = timeout,

    .error = EIO
  };

  if (callThreadFunction("bluetooth-open", runOpenBluetoothConnection, &obc, NULL)) {
    if (!obc.error) return 1;
    errno = obc.error;
  }

  return 0;
}

int
bthDiscoverChannel (
  uint8_t *channel, BluetoothConnectionExtension *bcx,
  const void *uuidBytes, size_t uuidLength,
  int timeout
) {
  *channel = 0;
  return 1;
}

int
bthMonitorInput (BluetoothConnection *connection, AsyncMonitorCallback *callback, void *data) {
  BluetoothConnectionExtension *bcx = connection->extension;

  bthCancelInputMonitor(bcx);
  if (!callback) return 1;
  return asyncMonitorFileInput(&bcx->inputMonitor, bcx->inputPipe[0], callback, data);
}

int
bthPollInput (BluetoothConnectionExtension *bcx, int timeout) {
  return awaitFileInput(bcx->inputPipe[0], timeout);
}

ssize_t
bthGetData (
  BluetoothConnectionExtension *bcx, void *buffer, size_t size,
  int initialTimeout, int subsequentTimeout
) {
  return readFile(bcx->inputPipe[0], buffer, size, initialTimeout, subsequentTimeout);
}

ssize_t
bthPutData (BluetoothConnectionExtension *bcx, const void *buffer, size_t size) {
  if (findJavaInstanceMethod(bcx->env, &bcx->writeMethod, bcx->connectionClass, "write",
                             JAVA_SIG_METHOD(JAVA_SIG_BOOLEAN,
                                             JAVA_SIG_ARRAY(JAVA_SIG_BYTE)) // bytes
                                            )) {
    jbyteArray bytes = (*bcx->env)->NewByteArray(bcx->env, size);

    if (bytes) {
      jboolean result;

      (*bcx->env)->SetByteArrayRegion(bcx->env, bytes, 0, size, buffer);
      result = (*bcx->env)->CallBooleanMethod(bcx->env, bcx->connection, bcx->writeMethod, bytes);
      (*bcx->env)->DeleteLocalRef(bcx->env, bytes);

      if (!clearJavaException(bcx->env, 1)) {
        if (result == JNI_TRUE) {
          return size;
        }
      }

      errno = EIO;
    } else {
      errno = ENOMEM;
    }
  } else {
    errno = ENOSYS;
  }

  logSystemError("Bluetooth write");
  return -1;
}

char *
bthObtainDeviceName (uint64_t bda, int timeout) {
  char *name = NULL;
  JNIEnv *env = getJavaNativeInterface();

  if (env) {
    static jclass class = NULL;

    if (findJavaClass(env, &class, "org/a11y/brltty/android/BluetoothConnection")) {
      static jmethodID method = 0;

      if (findJavaStaticMethod(env, &method, class, "getName",
                               JAVA_SIG_METHOD(JAVA_SIG_OBJECT(java/lang/String),
                                               JAVA_SIG_LONG // deviceAddress
                                              ))) {
        jstring jName = (*env)->CallStaticObjectMethod(env, class, method, bda);

        if (jName) {
          const char *cName = (*env)->GetStringUTFChars(env, jName, NULL);

          if (cName) {
            if (!(name = strdup(cName))) logMallocError();
            (*env)->ReleaseStringUTFChars(env, jName, cName);
          } else {
            logMallocError();
            clearJavaException(env, 0);
          }

          (*env)->DeleteLocalRef(env, jName);
        } else {
          logMallocError();
          clearJavaException(env, 0);
        }
      }
    }
  }

  return name;
}
