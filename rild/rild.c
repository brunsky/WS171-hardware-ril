/* //device/system/rild/rild.c
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <telephony/ril.h>
#define LOG_TAG "RILD"
#include <utils/Log.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <linux/capability.h>
#include <linux/prctl.h>
#include <sys/wait.h>

#include <private/android_filesystem_config.h>

#define LIB_PATH_PROPERTY   "rild.libpath"
#define LIB_ARGS_PROPERTY   "rild.libargs"
#define MAX_LIB_ARGS        16

static void usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s -l <ril impl library> [-- <args for impl library>]\n", argv0);
    exit(-1);
}

extern void RIL_register (const RIL_RadioFunctions *callbacks);

extern void RIL_onRequestComplete(RIL_Token t, RIL_Errno e,
                           void *response, size_t responselen);

extern void RIL_onUnsolicitedResponse(int unsolResponse, const void *data,
                                size_t datalen);

extern void RIL_requestTimedCallback (RIL_TimedCallback callback,
                               void *param, const struct timeval *relativeTime);


static struct RIL_Env s_rilEnv = {
    RIL_onRequestComplete,
    RIL_onUnsolicitedResponse,
    RIL_requestTimedCallback
};

extern void RIL_startEventLoop();

static int make_argv(char * args, char ** argv)
{
    // Note: reserve argv[0]
    int count = 1;
    char * tok;
    char * s = args;

    while ((tok = strtok(s, " \0"))) {
        argv[count] = tok;
        s = NULL;
        count++;
    }
    return count;
}

/*
 * switchUser - Switches UID to radio, preserving CAP_NET_ADMIN capabilities.
 * Our group, cache, was set by init.
 */
void switchUser() {
    prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
    setuid(AID_RADIO);

    struct __user_cap_header_struct header;
    struct __user_cap_data_struct cap;
    header.version = _LINUX_CAPABILITY_VERSION;
    header.pid = 0;
    cap.effective = cap.permitted = 1 << CAP_NET_ADMIN;
    cap.inheritable = 0;
    capset(&header, &cap);
}

static int tgmd_process_id;
static pthread_t s_tid_waitForTGMD;
static pthread_attr_t attr_waitForTGMD;

static pthread_mutex_t s_tgmd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_tgmd_cond = PTHREAD_COND_INITIALIZER;


static void createTGMDProcess(){

	LOGD("TGMD Starts");

	execl("/system/bin/tgmd","tgmd",(char *)NULL);
}

static void *waitForTGMDTerminated(){

	int child_state;

	LOGD("waitForTGMDTerminated START");

	int err=waitpid(tgmd_process_id,&child_state,0);

    LOGE("TGMD Process exited with errno: %d",err);

	pthread_mutex_lock(&s_tgmd_mutex);

   

	tgmd_process_id = -1;

	pthread_cond_broadcast(&s_tgmd_cond);


	pthread_mutex_unlock(&s_tgmd_mutex);

	LOGD("waitForTGMDTerminated END");

	tgmd_process_id = fork();

	if(!tgmd_process_id){

		createTGMDProcess();

	}else{

		//this is mother thread
		//Create another thread to wait for tgmd terminated!!!;
		pthread_attr_init (&attr_waitForTGMD);

		pthread_attr_setdetachstate(&attr_waitForTGMD, PTHREAD_CREATE_DETACHED);

		pthread_create(&s_tid_waitForTGMD,&attr_waitForTGMD,waitForTGMDTerminated, NULL);

	}

	return 0;


}



int main(int argc, char **argv)
{

    //

	tgmd_process_id = fork();

    if(!tgmd_process_id){

    	createTGMDProcess();
    
    }else{

    	//this is mother thread

    	//Create another thread to wait for tgmd terminated!!!;
    	pthread_attr_init (&attr_waitForTGMD);

    	pthread_attr_setdetachstate(&attr_waitForTGMD, PTHREAD_CREATE_DETACHED);

    	pthread_create(&s_tid_waitForTGMD,&attr_waitForTGMD,waitForTGMDTerminated,NULL);

    }

    //Setup Memory
    property_set("ro.FOREGROUND_APP_ADJ","0");
    property_set("ro.VISIBLE_APP_ADJ","1");
    property_set("ro.SECONDARY_SERVER_ADJ","2");
    property_set("ro.HOME_APP_ADJ","4");
    property_set("ro.HIDDEN_APP_MIN_ADJ","7");
    property_set("ro.CONTENT_PROVIDER_ADJ","14");
    property_set("ro.EMPTY_APP_ADJ","15");

    property_set("ro.FOREGROUND_APP_MEM","1536");
    property_set("ro.VISIBLE_APP_MEM","8000");
    property_set("ro.SECONDARY_SERVER_MEM","8000");
    property_set("ro.HOME_APP_MEM","10000");
    property_set("ro.HIDDEN_APP_MEM","10000");
    property_set("ro.CONTENT_PROVIDER_MEM","12500");
    property_set("ro.EMPTY_APP_MEM","16000");
    
    system("echo 0,1,2,7,14,15 >/sys/module/lowmemorykiller/parameters/adj");
    system("echo 1536,8000,8000,10000,12500,16000 >/sys/module/lowmemorykiller/parameters/minfree");

    // enable the cpu frequency control for android runtime
    system("chmod 0666 /sys/devices/system/cpu/cpu0/op");
    system("chmod 0666 /sys/class/backlight/pxa3xx_pwm_bl/bl_power");
    
    // create flag for power.c
    system("echo 1 > /tmp/wakeup");
    system("chmod 0666 /tmp/wakeup");
    system("chown system /tmp/wakeup");

    const char * rilLibPath = NULL;
    char **rilArgv;
    void *dlHandle;
    const RIL_RadioFunctions *(*rilInit)(const struct RIL_Env *, int, char **);
    const RIL_RadioFunctions *funcs;
    char libPath[PROPERTY_VALUE_MAX];
    unsigned char hasLibArgs = 0;

    int i;

    for (i = 1; i < argc ;) {
        if (0 == strcmp(argv[i], "-l") && (argc - i > 1)) {
            rilLibPath = argv[i + 1];
            i += 2;
        } else if (0 == strcmp(argv[i], "--")) {
            i++;
            hasLibArgs = 1;
            break;
        } else {
            usage(argv[0]);
        }
    }

    if (rilLibPath == NULL) {
        if ( 0 == property_get(LIB_PATH_PROPERTY, libPath, NULL)) {
            // No lib sepcified on the command line, and nothing set in props.
            // Assume "no-ril" case.
            goto done;
        } else {
            rilLibPath = libPath;
        }
    }

    /* special override when in the emulator */
#if 1
    {
        static char*  arg_overrides[3];
        static char   arg_device[32];
        int           done = 0;

#define  REFERENCE_RIL_PATH  "/system/lib/libreference-ril.so"

        /* first, read /proc/cmdline into memory */
        char          buffer[1024], *p, *q;
        int           len;
        int           fd = open("/proc/cmdline",O_RDONLY);

        if (fd < 0) {
            LOGD("could not open /proc/cmdline:%s", strerror(errno));
            goto OpenLib;
        }

        do {
            len = read(fd,buffer,sizeof(buffer)); }
        while (len == -1 && errno == EINTR);

        if (len < 0) {
            LOGD("could not read /proc/cmdline:%s", strerror(errno));
            close(fd);
            goto OpenLib;
        }
        close(fd);

        if (strstr(buffer, "android.qemud=") != NULL)
        {
            /* the qemud daemon is launched after rild, so
            * give it some time to create its GSM socket
            */
            int  tries = 5;
#define  QEMUD_SOCKET_NAME    "qemud"

            while (1) {
                int  fd;

                sleep(1);

                fd = socket_local_client(
                            QEMUD_SOCKET_NAME,
                            ANDROID_SOCKET_NAMESPACE_RESERVED,
                            SOCK_STREAM );

                if (fd >= 0) {
                    close(fd);
                    snprintf( arg_device, sizeof(arg_device), "%s/%s",
                                ANDROID_SOCKET_DIR, QEMUD_SOCKET_NAME );

                    arg_overrides[1] = "-s";
                    arg_overrides[2] = arg_device;
                    done = 1;
                    break;
                }
                LOGD("could not connect to %s socket: %s",
                    QEMUD_SOCKET_NAME, strerror(errno));
                if (--tries == 0)
                    break;
            }
            if (!done) {
                LOGE("could not connect to %s socket (giving up): %s",
                    QEMUD_SOCKET_NAME, strerror(errno));
                while(1)
                    sleep(0x00ffffff);
            }
        }

        /* otherwise, try to see if we passed a device name from the kernel */
        if (!done) do {
#define  KERNEL_OPTION  "android.ril="
#define  DEV_PREFIX     "/dev/"

            p = strstr( buffer, KERNEL_OPTION );
            if (p == NULL)
                break;

            p += sizeof(KERNEL_OPTION)-1;
            q  = strpbrk( p, " \t\n\r" );
            if (q != NULL)
                *q = 0;

            snprintf( arg_device, sizeof(arg_device), DEV_PREFIX "%s", p );
            arg_device[sizeof(arg_device)-1] = 0;
            arg_overrides[1] = "-d";
            arg_overrides[2] = arg_device;
            done = 1;

        } while (0);

        if (done) {
            argv = arg_overrides;
            argc = 3;
            i    = 1;
            hasLibArgs = 1;
            rilLibPath = REFERENCE_RIL_PATH;

            LOGD("overriding with %s %s", arg_overrides[1], arg_overrides[2]);
        }
    }
OpenLib:
#endif
    switchUser();

    dlHandle = dlopen(rilLibPath, RTLD_NOW);

    if (dlHandle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        exit(-1);
    }

    RIL_startEventLoop();

    rilInit = (const RIL_RadioFunctions *(*)(const struct RIL_Env *, int, char **))dlsym(dlHandle, "RIL_Init");

    if (rilInit == NULL) {
        fprintf(stderr, "RIL_Init not defined or exported in %s\n", rilLibPath);
        exit(-1);
    }

    if (hasLibArgs) {
        rilArgv = argv + i - 1;
        argc = argc -i + 1;
    } else {
        static char * newArgv[MAX_LIB_ARGS];
        static char args[PROPERTY_VALUE_MAX];
        rilArgv = newArgv;
        property_get(LIB_ARGS_PROPERTY, args, "");
        argc = make_argv(args, rilArgv);
    }

    // Make sure there's a reasonable argv[0]
    rilArgv[0] = argv[0];

    funcs = rilInit(&s_rilEnv, argc, rilArgv);

    RIL_register(funcs);

done:

    while(1) {
        // sleep(UINT32_MAX) seems to return immediately on bionic
        sleep(0x00ffffff);
    }
}

