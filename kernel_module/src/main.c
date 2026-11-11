/*
CatLog
Copyright (C) 2024 Cat
Copyright (C) 2020 Princess of Sleeping
Copyright (C) 2020 Asakura Reiko

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 3 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "catlog.h"
#include "ringbuf.h"

#include <psp2/kernel/error.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/utils.h>
#include <psp2kern/netps.h>
#include <psp2kern/udcd.h>
#include <stdarg.h>
#include <taihen.h>

#define CFG_PATH "ur0:/data/catlog.cfg"
#define DEFAULT_PORT 9999
#define RINGBUF_LEN 0x2000

#define SERIAL_BUSY 0x0
#define SERIAL_READY 0x1
#define SERIAL_STOP 0x2

#define HOOKS_NUM 3

static RingBuf net_ring;
static RingBuf usb_ring;

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);
int module_get_offset(SceUID pid, SceUID modid, int segidx, uint32_t offset, uintptr_t *func);

#define DECL_FUNC_HOOK(name, ...)                                                                                 \
  static tai_hook_ref_t name##HookRef;                                                                            \
  static SceUID name##HookUid = -1;                                                                               \
  static int name##HookFunc(__VA_ARGS__)

#define BIND_FUNC_EXPORT_HOOK(name, module, lib_nid, func_nid)                                                    \
  name##HookUid = taiHookFunctionExportForKernel(KERNEL_PID, &name##HookRef, (module), (lib_nid), (func_nid),     \
                                                 (const void *)name##HookFunc)

#define BIND_FUNC_OFFSET_HOOK(name, modid, segidx, offset, thumb)    \
    name##HookUid = taiHookFunctionOffsetForKernel(KERNEL_PID, &name##HookRef, \
        (modid), (segidx), (offset), thumb, (const void*)name##HookFunc)

#define GetExport(modname, lib_nid, func_nid, func)                                                               \
  module_get_export_func(KERNEL_PID, modname, lib_nid, func_nid, (uintptr_t *)func)


CatLogConfig_t Config;

static int net_thread_run    = 0;
static SceUID net_thread_uid = 0;

static int g_usb_running    = 0;
static SceUID g_usb_thread_uid = -1;
static SceUID g_usb_flag_uid = -1;

static SceUID hook[HOOKS_NUM];
static tai_hook_ref_t hook_ref[HOOKS_NUM];

static SceNetSockaddrIn server;

int (*sceUsbSerialStatusForDriver)();
int (*sceUsbSerialStartForDriver)();
int (*sceUsbSerialCloseForDriver)();
int (*sceUsbSerialSetupForDriver)(int);
int (*SceUsbSerialForDriver_6AA4EFA4)();
int (*sceUsbSerialSendForDriver)(const void *buffer, unsigned int len, int unk1, int unk2);

int (*sceDebugRegisterPutcharHandlerForKernel)(int (*func)(void *args, char c), void *args);
int (*sceDebugSetHandlersForKernel)(int (*func)(int unk, const char *format, const va_list args), void *args);
int (*sceDebugDisableInfoDumpForKernel)(int flags);
int (*sceKernelSetAssertLevelForKernel)(int level);

// userland printf
int UserDebugPrintfCallback(void *args, char c)
{
  (void)args;
  ringbuf_put_clobber(&net_ring, &c, 1);
  ringbuf_put_clobber(&usb_ring, &c, 1);
  return 0;
}

// kernel printf's
int KernelDebugPrintfCallback(int unk, const char *fmt, const va_list args)
{
  (void)unk;
  char buf[0x400];
  int buf_len = sizeof(buf);
  int len     = vsnprintf(buf, buf_len, fmt, args);
  len         = len < 0 ? 0 : len;
  len         = len >= buf_len ? buf_len - 1 : len;
  ringbuf_put_clobber(&net_ring, buf, len);
  ringbuf_put_clobber(&usb_ring, buf, len);
  return 0;
}

DECL_FUNC_HOOK(sceSblQafMgrIsAllowKernelDebugForDriver_patched)
{
  return 1;
}

DECL_FUNC_HOOK(sceSblQafMgrIsAllowSystemAppDebugForDriver_patched)
{
  return 1;
}

DECL_FUNC_HOOK(ScePower_3e10_patched, int pid, int flags, unsigned int set)
{
  if (flags == 3 && Config.net) // WLAN/COM
    set = 1;
  return TAI_CONTINUE(int, ScePower_3e10_patchedHookRef, pid, flags, set);
}

/* flags for sceNetShutdown */
#define SCE_NET_SHUT_RD 0
#define SCE_NET_SHUT_WR 1
#define SCE_NET_SHUT_RDWR 2

int ksceNetShutdown(int s, int how);

static int net_connect(void)
{
  int net_sock;

socket:
  net_sock = ksceNetSocket("CatLogTCP", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
  if (net_sock < 0)
  {
    goto socket_error;
  }

  int timeout = 5 * 1000 * 1000;
  ksceNetSetsockopt(net_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeout, sizeof(timeout));

  if (ksceNetConnect(net_sock, (SceNetSockaddr *)&server, sizeof(server)) == 0)
  {
    return net_sock;
  }
  ksceNetShutdown(net_sock, SCE_NET_SHUT_RDWR);

socket_error:
  ksceNetClose(net_sock);
  ksceKernelDelayThread(1000 * 1000);
  goto socket;
}

static void net_close(int net_sock)
{
  ksceNetShutdown(net_sock, SCE_NET_SHUT_RDWR);
  ksceNetClose(net_sock);
}

static int net_thread(SceSize args, void *argp)
{
  (void)args;
  (void)argp;

  ksceKernelDelayThread(8 * 1000 * 1000);

  ksceKernelPrintf("\n");
  ksceKernelPrintf("start catlog net_thread\n");
  ksceKernelPrintf("\n");

  while (net_thread_run)
  {
    char buf[0x400];
    int received_len = ringbuf_get_wait(&net_ring, buf, sizeof(buf), NULL);
    if (received_len == 0)
    {
      continue;
    }

    int net_sock;

  connect:
    net_sock = net_connect();

  send:
    if (ksceNetSend(net_sock, buf, received_len, 0) < 0)
    {
      net_close(net_sock);
      ksceKernelDelayThread(1000 * 1000);
      goto connect;
    }

    received_len = ringbuf_get_wait(&net_ring, buf, sizeof(buf), (SceUInt[]) {1000 * 1000});

    if (received_len > 0)
    {
      goto send;
    }

    net_close(net_sock);
  }

  return 0;
}

static void usb_serial_stop(void)
{
  SceUsbSerialForDriver_6AA4EFA4();
  sceUsbSerialCloseForDriver();
}

static int SceUsbMtpForDriver_6FC97594_patched(void)
{
  return 0;
}

static int sceUsbstorVStorStart_patched(int r0)
{
  ksceKernelWaitEventFlag(g_usb_flag_uid, SERIAL_READY, SCE_EVENT_WAITCLEAR, NULL, 0);
  usb_serial_stop();
  ksceUdcdStopCurrentInternal(2);
  return TAI_CONTINUE(int, hook_ref[1], r0);
}

static int sceUsbstorVStorStop_patched(void)
{
  ksceUdcdStopCurrentInternal(2);
  ksceKernelSetEventFlag(g_usb_flag_uid, SERIAL_STOP);
  return TAI_CONTINUE(int, hook_ref[2]);
}

static int usb_thread(SceSize args, void *argp)
{
  (void)args;
  (void)argp;

  ksceKernelPrintf("\n");
  ksceKernelPrintf("start catlog usb_thread\n");
  ksceKernelPrintf("\n");

  while (g_usb_running)
  {
    char buf[0x400];
    int received_len = ringbuf_get_wait(&usb_ring, buf, sizeof(buf), NULL);

    int ret = 0;
  connect:
    ret = sceUsbSerialStatusForDriver();
    if (ret != (int)0x80244401)
    {
      if (ret == 1)
      {
        goto send;
      }
      ksceKernelDelayThread(1000 * 1000);
      if (ret == 0)
      {
        goto connect;
      }
    }

    ksceKernelWaitEventFlag(g_usb_flag_uid, SERIAL_STOP, SCE_EVENT_WAITCLEAR, NULL, 0);

    sceUsbSerialStartForDriver();
    sceUsbSerialSetupForDriver(1);
    if (ret == (int)0x80244403)
    {
      SceUsbSerialForDriver_6AA4EFA4();
      sceUsbSerialSetupForDriver(1);
    }
    ksceKernelSetEventFlag(g_usb_flag_uid, SERIAL_READY);

  send:
    if (received_len == 0)
    {
      continue;
    }

    ksceKernelClearEventFlag(g_usb_flag_uid, SERIAL_READY);

    if (sceUsbSerialSendForDriver(buf, received_len, 0, -1) < 0)
    {
      ksceKernelSetEventFlag(g_usb_flag_uid, SERIAL_STOP);
      ksceKernelDelayThread(1000 * 1000);
      goto connect;
    }

    received_len = ringbuf_get_wait(&usb_ring, buf, sizeof(buf), (SceUInt[]) {1000 * 1000});
    ksceKernelSetEventFlag(g_usb_flag_uid, SERIAL_READY);
    if (received_len > 0)
    {
      goto send;
    }
  }

  return 0;
}


int SaveConfig(void)
{
  SceUID fd = ksceIoOpen(CFG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return fd;

  ksceIoWrite(fd, &Config, sizeof(CatLogConfig_t));
  ksceIoClose(fd);

  return 0;
}

int CreateConfig(void)
{
  Config.host = 0x0100007f; // 127.0.0.1
  Config.port = DEFAULT_PORT;
  Config.loglevel = 2;
  Config.net = 0;

  SceUID fd = ksceIoOpen(CFG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return fd;

  ksceIoWrite(fd, &Config, sizeof(CatLogConfig_t));
  ksceIoClose(fd);

  return 0;
}

int CheckConfig(void)
{
  SceUID fd = ksceIoOpen(CFG_PATH, SCE_O_RDONLY, 0);
  if (fd < 0)
  {
    return fd;
  }

  CatLogConfig_t tmp;

  int res = ksceIoRead(fd, &tmp, sizeof(CatLogConfig_t));

  if (res != sizeof(CatLogConfig_t))
  {
    ksceIoClose(fd);
    return -1;
  }

  ksceIoClose(fd);

  return 0;
}

int LoadConfig(void)
{
  int res;
  SceUID fd;

  server.sin_len    = sizeof(server);
  server.sin_family = SCE_NET_AF_INET;

  memset(server.sin_zero, 0, sizeof(server.sin_zero));

  fd = ksceIoOpen(CFG_PATH, SCE_O_RDONLY, 0);
  if (fd < 0)
  {
    res = fd;
    goto end;
  }
  res = ksceIoRead(fd, &Config, sizeof(CatLogConfig_t));
  if (res != sizeof(CatLogConfig_t))
  {
    res = -1;
    goto end;
  }

  server.sin_addr.s_addr = Config.host;
  server.sin_port        = ksceNetHtons(Config.port ? Config.port : DEFAULT_PORT);

  res = 0;

end:
  if (fd > 0)
  {
    ksceIoClose(fd);
  }

  return res;
}


int CatLogUpdateConfig(uint32_t host, uint16_t port, uint16_t level, uint8_t net)
{
  uint32_t state;

  ENTER_SYSCALL(state);

  Config.host = host;
  Config.port = port;
  Config.loglevel = level;
  Config.net = net;
  sceKernelSetAssertLevelForKernel(Config.loglevel);

  server.sin_addr.s_addr = host;
  server.sin_port        = ksceNetHtons(port ? port : DEFAULT_PORT);

  SaveConfig();

  EXIT_SYSCALL(state);

  return 0;
}

int CatLogReadConfig(uint32_t* host, uint16_t* port, uint16_t* level, uint8_t* net)
{
  int res;
  uint32_t state;

  ENTER_SYSCALL(state);

  res = ksceKernelMemcpyKernelToUser((void *)host, &Config.host, 4);
  if (res < 0)
  {
    goto end;
  }

  res = ksceKernelMemcpyKernelToUser((void *)port, &Config.port, 2);
  if (res < 0)
  {
    goto end;
  }

  res = ksceKernelMemcpyKernelToUser((void *)level, &Config.loglevel, 2);
  if (res < 0)
  {
    goto end;
  }

  res = ksceKernelMemcpyKernelToUser((void *)net, &Config.net, 1);
  if (res < 0)
  {
    goto end;
  }

end:
  EXIT_SYSCALL(state);

  return res;
}


static int usb_serial_init(void)
{
  int ret;
  tai_module_info_t usbInfo;
  usbInfo.size = sizeof(tai_module_info_t);

  g_usb_flag_uid = ksceKernelCreateEventFlag("CatLog USB Flag", 0, SERIAL_STOP, NULL);
  if (g_usb_flag_uid < 0)
  {
    ret = g_usb_flag_uid;
    goto end;
  }

  hook[0] = taiHookFunctionExportForKernel(KERNEL_PID, &hook_ref[0], "SceUsbMtp", TAI_ANY_LIBRARY, 0x6FC97594, SceUsbMtpForDriver_6FC97594_patched);
  hook[1] = taiHookFunctionExportForKernel(KERNEL_PID, &hook_ref[1], "SceUsbstorVStorDriver", TAI_ANY_LIBRARY, 0xB606F1AF, sceUsbstorVStorStart_patched);
  hook[2] = taiHookFunctionExportForKernel(KERNEL_PID, &hook_ref[2], "SceUsbstorVStorDriver", TAI_ANY_LIBRARY, 0x0FD67059, sceUsbstorVStorStop_patched);

  if (taiGetModuleInfoForKernel(KERNEL_PID, "SceUsbSerial", &usbInfo) < 0)
  {
    ret = -1;
    goto end;
  }

  ret = module_get_offset(KERNEL_PID, usbInfo.modid, 0, 0x1E78 | 1, (uintptr_t *)&sceUsbSerialStatusForDriver);
  if (ret < 0)
  {
    goto end;
  }

  ret = module_get_offset(KERNEL_PID, usbInfo.modid, 0, 0x168C | 1, (uintptr_t *)&sceUsbSerialStartForDriver);
  if (ret < 0)
  {
    goto end;
  }

  ret = module_get_offset(KERNEL_PID, usbInfo.modid, 0, 0x17A0 | 1, (uintptr_t *)&sceUsbSerialSetupForDriver);
  if (ret < 0)
  {
    goto end;
  }

  ret = module_get_offset(KERNEL_PID, usbInfo.modid, 0, 0x18F4 | 1, (uintptr_t *)&SceUsbSerialForDriver_6AA4EFA4);
  if (ret < 0)
  {
    goto end;
  }

  ret = module_get_offset(KERNEL_PID, usbInfo.modid, 0, 0x1A68 | 1, (uintptr_t *)&sceUsbSerialSendForDriver);
  if (ret < 0)
  {
    goto end;
  }

  ret = module_get_offset(KERNEL_PID, usbInfo.modid, 0, 0x1738 | 1, (uintptr_t *)&sceUsbSerialCloseForDriver);
  if (ret < 0)
  {
    goto end;
  }

  g_usb_thread_uid = ksceKernelCreateThread("usb_thread", usb_thread, 0x40, 0x1000, 0, 0, 0);
  if (g_usb_thread_uid < 0)
  {
    ret = g_usb_thread_uid;
    goto end;
  }

  g_usb_running = 1;

  ksceKernelStartThread(g_usb_thread_uid, 0, NULL);

  ret = 0;

end:
  return ret;
}

int CatLogInit(void)
{
  int ret;

  ksceIoMkdir("ur0:/data", 0777);

  if (CheckConfig() < 0)
  {
    CreateConfig();
  }

  ret = LoadConfig();
  if (ret < 0)
  {
    goto end;
  }

  ret = ringbuf_init(&net_ring, RINGBUF_LEN);
  if (ret < 0)
  {
    goto end;
  }

  ret = ringbuf_init(&usb_ring, RINGBUF_LEN);
  if (ret < 0)
  {
    ringbuf_term(&net_ring);
    goto end;
  }

  tai_module_info_t modInfo;
  modInfo.size = sizeof(tai_module_info_t);

  if (taiGetModuleInfoForKernel(KERNEL_PID, "ScePower", &modInfo) < 0)
  {
    ret = -1;
    goto end;
  }

  if (GetExport("SceSysmem", 0x88C17370, 0xCE9060F1, &sceKernelSetAssertLevelForKernel) < 0)
    if (GetExport("SceSysmem", 0x13D793B7, 0xC5889385, &sceKernelSetAssertLevelForKernel) < 0)
    {
      ret = -1;
      goto end;
    }

  if (GetExport("SceSysmem", 0x88C17370, 0xE6115A72, &sceDebugRegisterPutcharHandlerForKernel) < 0)
    if (GetExport("SceSysmem", 0x13D793B7, 0x22546577, &sceDebugRegisterPutcharHandlerForKernel) < 0)
    {
      ret = -1;
      goto end;
    }

  if (GetExport("SceSysmem", 0x88C17370, 0x10067B7B, &sceDebugSetHandlersForKernel) < 0)
    if (GetExport("SceSysmem", 0x13D793B7, 0x88AD6D0C, &sceDebugSetHandlersForKernel) < 0)
    {
      ret = -1;
      goto end;
    }

  if (GetExport("SceSysmem", 0x88C17370, 0xF857CDD6, &sceDebugDisableInfoDumpForKernel) < 0)
    if (GetExport("SceSysmem", 0x13D793B7, 0xA465A31A, &sceDebugDisableInfoDumpForKernel) < 0)
    {
      ret = -1;
      goto end;
    }

  BIND_FUNC_EXPORT_HOOK(sceSblQafMgrIsAllowKernelDebugForDriver_patched, "SceSysmem", 0xFFFFFFFF, 0x382C71E8);
  BIND_FUNC_EXPORT_HOOK(sceSblQafMgrIsAllowSystemAppDebugForDriver_patched, "SceSysmem", 0xFFFFFFFF, 0xCAD47130);

  BIND_FUNC_OFFSET_HOOK(ScePower_3e10_patched, modInfo.modid, 0, 0x3E10, 1);

  ret = sceDebugDisableInfoDumpForKernel(0);
  if (ret < 0)
  {
    goto end;
  }

  sceKernelSetAssertLevelForKernel(Config.loglevel);

  sceDebugSetHandlersForKernel(KernelDebugPrintfCallback, 0);
  sceDebugRegisterPutcharHandlerForKernel(UserDebugPrintfCallback, 0);

  net_thread_uid = ksceKernelCreateThread("net_thread", net_thread, 0x40, 0x1000, 0, 0, 0);
  if (net_thread_uid < 0)
  {
    ret = net_thread_uid;
    goto end;
  }

  net_thread_run = 1;

  ksceKernelStartThread(net_thread_uid, 0, NULL);

  ret = usb_serial_init();
  if (ret < 0)
  {
    ksceKernelPrintf("catlog: usb serial init failed %x\n", ret);
    ret = 0;
  }

  ret = 0;

end:

  return ret;
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize argc, const void *args)
{
  (void)argc;
  (void)args;

  CatLogInit();

  return SCE_KERNEL_START_SUCCESS;
}
