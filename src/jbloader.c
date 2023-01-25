#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <termios.h>
#include <sys/clonefile.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <spawn.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <dlfcn.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include "kerninfo.h"
#include "offsetfinder.h"

#ifndef RAMDISK
#define RAMDISK "/dev/rmd0"
#endif

#define PRINTF_BINARY_PATTERN_INT8 "%c%c%c%c%c%c%c%c"
#define PRINTF_BYTE_TO_BINARY_INT8(i) \
  (((i)&0x80ll) ? '1' : '0'),         \
      (((i)&0x40ll) ? '1' : '0'),     \
      (((i)&0x20ll) ? '1' : '0'),     \
      (((i)&0x10ll) ? '1' : '0'),     \
      (((i)&0x08ll) ? '1' : '0'),     \
      (((i)&0x04ll) ? '1' : '0'),     \
      (((i)&0x02ll) ? '1' : '0'),     \
      (((i)&0x01ll) ? '1' : '0')

#define PRINTF_BINARY_PATTERN_INT16 \
  PRINTF_BINARY_PATTERN_INT8 PRINTF_BINARY_PATTERN_INT8
#define PRINTF_BYTE_TO_BINARY_INT16(i) \
  PRINTF_BYTE_TO_BINARY_INT8((i) >> 8), PRINTF_BYTE_TO_BINARY_INT8(i)
#define PRINTF_BINARY_PATTERN_INT32 \
  PRINTF_BINARY_PATTERN_INT16 PRINTF_BINARY_PATTERN_INT16
#define PRINTF_BYTE_TO_BINARY_INT32(i) \
  PRINTF_BYTE_TO_BINARY_INT16((i) >> 16), PRINTF_BYTE_TO_BINARY_INT16(i)
#define PRINTF_BINARY_PATTERN_INT64 \
  PRINTF_BINARY_PATTERN_INT32 PRINTF_BINARY_PATTERN_INT32
#define PRINTF_BYTE_TO_BINARY_INT64(i) \
  PRINTF_BYTE_TO_BINARY_INT32((i) >> 32), PRINTF_BYTE_TO_BINARY_INT32(i)

extern char **environ;
#define serverURL "http://static.palera.in" // if doing development, change this to your local server
#define HDI_MAGIC 0x1beeffeed
struct HDIImageCreateBlock64
{
  uint64_t magic;
  const void *props;
  uint64_t props_size;
  char padding[0x100 - 24];
};
struct kerninfo info;
struct paleinfo pinfo;

void spin()
{
  puts("jbinit DIED!");
  while (1)
  {
    sleep(5);
  }
}

int run(const char *cmd, char *const *args)
{
  int pid = 0;
  int retval = 0;
  char printbuf[0x1000] = {};
  for (char *const *a = args; *a; a++)
  {
    size_t csize = strlen(printbuf);
    if (csize >= sizeof(printbuf))
      break;
    snprintf(printbuf + csize, sizeof(printbuf) - csize, "%s ", *a);
  }

  retval = posix_spawn(&pid, cmd, NULL, NULL, args, NULL);
  printf("Executing: %s (posix_spawn returned: %d)\n", printbuf, retval);
  {
    int pidret = 0;
    printf("waiting for '%s' to finish...\n", printbuf);
    retval = waitpid(pid, &pidret, 0);
    printf("waitpid for '%s' returned: %d\n", printbuf, retval);
    return pidret;
  }
  return retval;
}

int run_async(const char *cmd, char *const *args)
{
  int pid = 0;
  int retval = 0;
  char printbuf[0x1000] = {};
  for (char *const *a = args; *a; a++)
  {
    size_t csize = strlen(printbuf);
    if (csize >= sizeof(printbuf))
      break;
    snprintf(printbuf + csize, sizeof(printbuf) - csize, "%s ", *a);
  }
  retval = posix_spawn(&pid, cmd, NULL, NULL, args, NULL);
  printf("Asynchronous execution: %s (posix_spawn returned: %d)\n", printbuf, retval);
  return retval;
}

int get_boot_manifest_hash(char hash[97])
{
  const UInt8 *bytes;
  CFIndex length;
  io_registry_entry_t chosen = IORegistryEntryFromPath(0, "IODeviceTree:/chosen");
  assert(chosen);
  CFDataRef manifestHash = (CFDataRef)IORegistryEntryCreateCFProperty(chosen, CFSTR("boot-manifest-hash"), kCFAllocatorDefault, 0);
  if (manifestHash == NULL || CFGetTypeID(manifestHash) != CFDataGetTypeID())
  {
    return 1;
  }
  length = CFDataGetLength(manifestHash);
  bytes = CFDataGetBytePtr(manifestHash);
  CFRelease(manifestHash);
  for (int i = 0; i < length; i++)
  {
    snprintf(&hash[i * 2], 3, "%02X", bytes[i]);
  }
  return 0;
}

int get_kerninfo(struct kerninfo *info, char *rd)
{
  uint32_t ramdisk_size_actual;
  errno = 0;
  int fd = open(rd, O_RDONLY);
  if (fd == -1)
    return errno;
  read(fd, &ramdisk_size_actual, 4);
  lseek(fd, (long)(ramdisk_size_actual), SEEK_SET);
  if (errno != 0)
    return errno;
  ssize_t didread = read(fd, info, sizeof(struct kerninfo));
  if ((unsigned long)didread != sizeof(struct kerninfo) || info->size != (uint64_t)sizeof(struct kerninfo))
  {
    return EINVAL;
  }
  close(fd);
  return 0;
}

int get_paleinfo(struct paleinfo *info, char *rd)
{
  uint32_t ramdisk_size_actual;
  int fd = open(rd, O_RDONLY, 0);
  read(fd, &ramdisk_size_actual, 4);
  lseek(fd, (long)(ramdisk_size_actual) + 0x1000L, SEEK_SET);
  int64_t didread = read(fd, info, sizeof(struct paleinfo));
  if ((unsigned long)didread != sizeof(struct paleinfo))
  {
    return -1;
  }
  if (info->magic != PALEINFO_MAGIC)
  {
    printf("Detected corrupted paleinfo!\n");
    return -1;
  }
  if (info->version != 1)
  {
    printf("Unsupported paleinfo %u (expected 1)\n", info->version);
    return -1;
  }
  close(fd);
  return 0;
}

int mount_dmg(const char *device, const char *fstype, const char *mnt, const int mntopts, bool is_overlay)
{
  CFDictionaryKeyCallBacks key_callback = kCFTypeDictionaryKeyCallBacks;
  CFDictionaryValueCallBacks value_callback = kCFTypeDictionaryValueCallBacks;
  CFAllocatorRef allocator = kCFAllocatorDefault;
  CFMutableDictionaryRef hdix = IOServiceMatching("IOHDIXController");
  io_service_t hdix_service = IOServiceGetMatchingService(kIOMasterPortDefault, hdix);
  io_connect_t connect;
  assert(hdix_service != 0);
  kern_return_t open_hdix = IOServiceOpen(hdix_service, mach_task_self(), 0, &connect);
  assert(open_hdix == KERN_SUCCESS);
  fprintf(stderr, "IOServiceOpen: %d\n", open_hdix);
  CFMutableDictionaryRef props = CFDictionaryCreateMutable(allocator, 0, &key_callback, &value_callback);
  CFUUIDRef uuid = CFUUIDCreate(allocator);
  CFStringRef uuid_string = CFUUIDCreateString(0, uuid);
  size_t device_path_len = strlen(device);
  CFDataRef path_bytes = CFDataCreateWithBytesNoCopy(allocator, (unsigned char *)device, device_path_len, kCFAllocatorNull);
  assert(path_bytes != 0);
  CFDictionarySetValue(props, CFSTR("hdik-unique-identifier"), uuid_string);
  CFDictionarySetValue(props, CFSTR("image-path"), path_bytes);
  CFDictionarySetValue(props, CFSTR("autodiskmount"), kCFBooleanFalse);
  CFDictionarySetValue(props, CFSTR("removable"), kCFBooleanTrue);
  if (is_overlay)
  {
    CFMutableDictionaryRef image_secrets = CFDictionaryCreateMutable(allocator, 0, &key_callback, &value_callback);
    CFDictionarySetValue(image_secrets, CFSTR("checkra1n-overlay"), kCFBooleanTrue);
    CFDictionarySetValue(props, CFSTR("image-secrets"), image_secrets);
  }
  CFDataRef hdi_props = CFPropertyListCreateData(allocator, props, kCFPropertyListXMLFormat_v1_0, 0, 0);
  assert(hdi_props != 0);
  struct HDIImageCreateBlock64 hdi_stru;
  memset(&hdi_stru, 0, sizeof(hdi_stru));
  hdi_stru.magic = HDI_MAGIC;
  hdi_stru.props = (char *)CFDataGetBytePtr(hdi_props);
  hdi_stru.props_size = CFDataGetLength(hdi_props);
  volatile unsigned long four_L = 4L;
  uint32_t val;
  size_t val_size = sizeof(val);
  kern_return_t stru_ret = IOConnectCallStructMethod(connect, 0, &hdi_stru, sizeof(hdi_stru), &val, &val_size);
  if (stru_ret != 0)
  {
    fprintf(stderr, "IOConnectCallStructMethod(connect, 0, &hdi_stru, sizeof(hdi_stru), &val, &val_size) returned %x %s\n", stru_ret, mach_error_string(stru_ret));
    return 1;
  }
  assert(four_L == 4);
  CFMutableDictionaryRef pmatch = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(pmatch, CFSTR("hdik-unique-identifier"), uuid_string);
  CFMutableDictionaryRef matching = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(matching, CFSTR("IOPropertyMatch"), pmatch);
  hdix_service = IOServiceGetMatchingService(kIOMasterPortDefault, matching);
  if (hdix_service == 0)
  {
    fprintf(stderr, "successfully attached, but didn't find top entry in IO registry\n");
    return 1;
  }
  io_iterator_t iter;
  kern_return_t iterator_ret = IORegistryEntryCreateIterator(hdix_service, kIOServicePlane, kIORegistryIterateRecursively, &iter);
  if (iterator_ret != KERN_SUCCESS)
  {
    fprintf(stderr, "IORegistryEntryCreateIterator(hdix_service, kIOServicePlane, 1, &iter) returned %x %s\n", iterator_ret, mach_error_string(iterator_ret));
    return 1;
  };
  uint8_t not_mount_ret = 0;
  while (1)
  {
    io_object_t next = IOIteratorNext(iter);
    if ((int)next == 0)
      break;
    CFStringRef bsd_name = (CFStringRef)IORegistryEntryCreateCFProperty(next & 0xffffffff, CFSTR("BSD Name"), 0, 0);
    char buf[1024];
    if (bsd_name == 0)
      continue;
    char cstring = CFStringGetCString(bsd_name, buf, sizeof(buf), kCFStringEncodingUTF8);
    assert(cstring != '\0');
    puts(buf);
    char diskdev_name_buf[512];
    bzero(&diskdev_name_buf, sizeof(diskdev_name_buf));
    snprintf(diskdev_name_buf, sizeof(diskdev_name_buf), "/dev/%s", buf);
    char *dev2 = strdup(diskdev_name_buf);
    fprintf(stderr, "calling mount(fstype=%s, mnt=%s, mntopts=%d, data=%s)\n", fstype, mnt, mntopts, dev2);
    int mount_ret = mount(fstype, mnt, mntopts, &dev2);
    if (mount_ret == 0)
    {
      not_mount_ret = 1;
    }
  }
  if ((not_mount_ret & 1) == 0)
  {
    fprintf(stderr, "successfully attached, but mounting failed (potentially due to entry not found): %d (%s)\n", errno, strerror(errno));
    return 1;
  }
  return 0;
}

int check_and_mount_dmg()
{
  if (access("/cores/binpack/bin/sh", F_OK) != -1)
  {
    /* binpack already mounted */
    return 0;
  }
  if (access("/cores/binpack", F_OK) != 0)
  {
    fprintf(stderr, "/cores/binpack cannot be accessed! errno=%d\n", errno);
    return -1;
  }
  return mount_dmg("ramfile://checkra1n", "hfs", "/cores/binpack", MNT_RDONLY, true);
}

int check_and_mount_loader()
{
  if (access("/cores/binpack/Applications/palera1nLoader.app", F_OK) != -1)
  {
    /* loader already mounted */
    return 0;
  }
  if (access("/cores/binpack/Applications", F_OK) != 0)
  {
    fprintf(stderr, "/cores/binpack/Applications cannot be accessed! errno=%d\n", errno);
    return -1;
  }
  return mount_dmg("/cores/binpack/loader.dmg", "hfs", "/cores/binpack/Applications", MNT_RDONLY, false);
}

extern char **environ;

void *enable_ssh(void *__unused _)
{
  if (access("/private/var/dropbear_rsa_host_key", F_OK) != 0)
  {
    char *dropbearkey_argv[] = {"/cores/binpack/usr/bin/dropbearkey", "-f", "/private/var/dropbear_rsa_host_key", "-t", "rsa", "-s", "4096", NULL};
    run(dropbearkey_argv[0], dropbearkey_argv);
  }
  char *launchctl_argv[] = {"/cores/binpack/bin/launchctl", "load", "-w", "/cores/binpack/Library/LaunchDaemons/dropbear.plist", NULL};
  run(launchctl_argv[0], launchctl_argv);
  return NULL;
}

int jailbreak_obliterator()
{
  if (checkrain_option_enabled(pinfo.flags, palerain_option_rootful))
  {
    return 0;
  }
  else
  {
    printf("Obliterating jailbraek\n");
    char hash[97];
    char prebootPath[150] = "/private/preboot/";
    memset(hash, '\0', sizeof(hash));
    int ret = get_boot_manifest_hash(hash);
    if (ret != 0)
    {
      fprintf(stderr, "cannot get boot manifest hash\n");
      return ret;
    }
    printf("boot manifest hash: %s\n", hash);
    if (access("/var/jb/Applications", F_OK) == 0)
    {
      printf("unregistering applications\n");
      DIR *d = NULL;
      struct dirent *dir = NULL;
      if (!(d = opendir("/var/jb/Applications")))
      {
        fprintf(stderr, "Failed to open dir with err=%d (%s)\n", errno, strerror(errno));
        return -1;
      }
      while ((dir = readdir(d)))
      { // remove all subdirs and files
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
        {
          continue;
        }
        char *pp = NULL;
        asprintf(&pp, "/var/jb/Applications/%s", dir->d_name);
        {
          char *args[] = {
              "/cores/binpack/usr/bin/uicache",
              "-u",
              pp,
              NULL};
          run(args[0], args);
        }
        free(pp);
      }
      closedir(d);
    }

    printf("Apps now unregistered\n");
    strncat(prebootPath, hash, 150 - sizeof("/procursus") - sizeof("/private/preboot"));
    strncat(prebootPath, "/procursus", 150 - 97 - sizeof("/private/preboot/"));
    printf("prebootPath: %s\n", prebootPath);
    printf("%lu\n", strlen(hash));
    printf("%lu\n", strlen("/private/preboot/") + strlen(hash) + strlen("/procursus"));
    // yeah we don't want rm -rf /private/preboot
    assert(strlen(prebootPath) == strlen("/private/preboot/") + strlen(hash) + strlen("/procursus"));
    char *rm_argv[] = {
        "/cores/binpack/bin/rm",
        "-rf",
        "/var/jb",
        prebootPath,
        "/var/lib",
        "/var/cache",
        NULL};
    run(rm_argv[0], rm_argv);
    char *uicache_argv[] = {
        "/cores/binpack/usr/bin/uicache",
        "-af",
        NULL};
    run(uicache_argv[0], uicache_argv);
    printf("Jailbreak obliterated\n");
  }
  return 0;
}

int uicache_apps()
{
  if (checkrain_option_enabled(pinfo.flags, palerain_option_rootful))
  {
    if (access("/usr/bin/uicache", F_OK) == 0)
    {
      {
        char *uicache_argv[] = {
            "/usr/bin/uicache",
            "-a",
            NULL};
        run_async(uicache_argv[0], uicache_argv);
        return 0;
      };
    }
    return 0;
  }
  else
  {
    if (access("/var/jb/usr/bin/uicache", F_OK) == 0)
    {
      {
        char *uicache_argv[] = {
            "/var/jb/usr/bin/uicache",
            "-a",
            NULL};
        run_async(uicache_argv[0], uicache_argv);
        return 0;
      };
    }
    return 0;
  }
}

int load_etc_rc_d()
{
  DIR *d = NULL;
  struct dirent *dir = NULL;
  if (checkrain_option_enabled(pinfo.flags, palerain_option_rootful))
  {
    d = opendir("/etc/rc.d/");
  }
  else
  {
    d = opendir("/var/jb/etc/rc.d/");
  }
  if (!d)
  {
    printf("Failed to open dir with err=%d (%s)\n", errno, strerror(errno));
    if (access("/cores/should_userspace_reboot", F_OK) == 0)
      rmdir("/cores/should_userspace_reboot");
    return 0;
  }
  while ((dir = readdir(d)))
  { // remove all subdirs and files
    if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
    {
      continue;
    }
    char *pp = NULL;
    if (checkrain_option_enabled(pinfo.flags, palerain_option_rootful))
    {
      asprintf(&pp, "/etc/rc.d/%s", dir->d_name);
    }
    else
    {
      asprintf(&pp, "/var/jb/etc/rc.d/%s", dir->d_name);
    }
    {
      char *args[] = {
          pp,
          NULL};
      run_async(args[0], args);
    }
    free(pp);
  }
  closedir(d);
  if (access("/cores/should_userspace_reboot", F_OK) != 0) return 0;
  if (rmdir("/cores/should_userspace_reboot")) {
    printf("cannot rmdir /cores/should_userspace_reboot: %d (%s)\n", errno, strerror(errno));
    return -1;
  }
  char* userspace_reboot_args[] = {
    "/cores/binpack/bin/launchctl",
    "reboot",
    "userspace",
    NULL
  };
  run(userspace_reboot_args[0], userspace_reboot_args);
  exit(0);
  return -1;
}

int loadDaemons()
{
  if (checkrain_option_enabled(pinfo.flags, palerain_option_rootful))
  {
    if (access("/Library/LaunchDaemons", F_OK) != 0)
      return 0;
    {
      char *args[] = {
          "/bin/launchctl",
          "load",
          "/Library/LaunchDaemons",
          NULL};
      run_async(args[0], args);
    }
  }
  else
  {
    if (access("/var/jb/Library/LaunchDaemons", F_OK) != 0)
      return 0;
    {
      char *args[] = {
          "/var/jb/bin/launchctl",
          "load",
          "/var/jb/Library/LaunchDaemons",
          NULL};
      run_async(args[0], args);
    }
  }
  return 0;
}

void safemode_alert(CFNotificationCenterRef center, void *observer,
                    CFStringRef name, const void *object, CFDictionaryRef userInfo)
{
  int ret;
  CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(dict, kCFUserNotificationAlertHeaderKey, CFSTR("Entered Safe Mode"));
  if (checkrain_option_enabled(checkrain_option_failure, info.flags))
  {
    CFDictionarySetValue(dict, kCFUserNotificationAlertMessageKey, CFSTR("jbloader entered safe mode due to an error"));
  }
  else
  {
    CFDictionarySetValue(dict, kCFUserNotificationAlertMessageKey, CFSTR("jbloader entered safe mode due to an user request"));
  }
  CFUserNotificationCreate(kCFAllocatorDefault, 0, 0, &ret, dict);
  if (ret != 0)
  {
    fprintf(stderr, "CFUserNotificationCreate() returned %d %s\n", ret, mach_error_string(ret));
  }
  printf("Safe mode notification alert sent\n");
  return;
}

void *prep_jb_launch(void *__unused _)
{
  assert(info.size == sizeof(struct kerninfo));
  if (checkrain_option_enabled(checkrain_option_force_revert, info.flags))
  {
    jailbreak_obliterator();
    return NULL;
  }
  if (checkrain_option_enabled(checkrain_option_safemode, info.flags))
  {
    printf("Safe mode is enabled\n");
  }
  else
  {
    load_etc_rc_d();
    loadDaemons();
  }
  return NULL;
}

void *prep_jb_ui(void *__unused _)
{
  uicache_apps();
  return NULL;
}

int remount(char* rootdev)
{
  if (checkrain_option_enabled(pinfo.flags, palerain_option_rootful))
  {
    char dev_rootdev[0x20];
    snprintf(dev_rootdev, 0x20, "/dev/%s", rootdev);
    char *args[] = {
        "/sbin/mount_apfs",
        "-o",
        "rw,update",
        dev_rootdev,
        "/",
        NULL};
    run(args[0], args);
  }
  char *args[] = {
      "/sbin/mount",
      "-uw",
      "/private/preboot",
      NULL};
  return run(args[0], args);
}

int uicache_loader()
{
  if (checkrain_option_enabled(pinfo.flags, palerain_option_rootful))
  {
    char *loader_uicache_argv[] = {
        "/cores/binpack/usr/bin/uicache",
        "-p",
        "/jbin/loader.app",
        NULL};
    run(loader_uicache_argv[0], loader_uicache_argv);
  }
  else
  {
    char *loader_uicache_argv[] = {
        "/cores/binpack/usr/bin/uicache",
        "-p",
        "/cores/binpack/Applications/palera1nLoader.app",
        NULL};
    run(loader_uicache_argv[0], loader_uicache_argv);
  }
  return 0;
}

int sbreload()
{
  if (checkrain_option_enabled(pinfo.flags, palerain_option_rootful))
  {
    if (access("/usr/bin/sbreload", F_OK) != 0)
      return 0;
    char *args[] = {
        "/usr/bin/sbreload",
        NULL};
    return run(args[0], args);
  }
  else
  {
    if (access("/var/jb/usr/bin/sbreload", F_OK) != 0)
      return 0;
    char *args[] = {
        "/var/jb/usr/bin/sbreload",
        NULL};
    return run(args[0], args);
  }
}

int jbloader_main(int argc, char **argv)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  int ret = get_paleinfo(&pinfo, RAMDISK);
  if (ret != 0)
  {
    fprintf(stderr, "cannot get paleinfo: ret: %d, errno: %d (%s)\n", ret, errno, strerror(errno));
    return 1;
  }
  if (checkrain_option_enabled(pinfo.flags, palerain_option_jbinit_log_to_file)) {
    int fd_log = open("/cores/jbinit.log", O_WRONLY | O_APPEND | O_SYNC, 0644);
    if (fd_log != -1) {
      dup2(fd_log, STDOUT_FILENO);
      dup2(fd_log, STDERR_FILENO);
      puts("======== jbloader (system boot) log start =========");
    }
    else puts("cannot open /cores/jbinit.log for logging");
  }
  printf("========================================\n");
  printf("palera1n: init!\n");
  printf("pid: %d\n", getpid());
  printf("uid: %d\n", getuid());
  ret = get_kerninfo(&info, RAMDISK);
  if (ret != 0)
  {
    fprintf(stderr, "cannot get kerninfo: ret: %d, errno: %d (%s)\n", ret, errno, strerror(errno));
    return 1;
  }
  remount(pinfo.rootdev);
  pthread_t ssh_thread, prep_jb_launch_thread, prep_jb_ui_thread;
  pthread_create(&prep_jb_launch_thread, NULL, prep_jb_launch, NULL);
  pthread_create(&ssh_thread, NULL, enable_ssh, NULL);
  pthread_join(prep_jb_launch_thread, NULL);
  if (!checkrain_option_enabled(checkrain_option_force_revert, info.flags))
  {
    pthread_create(&prep_jb_ui_thread, NULL, prep_jb_ui, NULL);
  }
  pthread_join(ssh_thread, NULL);
  if (!checkrain_option_enabled(checkrain_option_force_revert, info.flags))
    pthread_join(prep_jb_ui_thread, NULL);
  uicache_loader();
  if (checkrain_option_enabled(checkrain_option_safemode, info.flags))
  {
    CFNotificationCenterAddObserver(
        CFNotificationCenterGetDarwinNotifyCenter(), NULL, &safemode_alert,
        CFSTR("SBSpringBoardDidLaunchNotification"), NULL, 0);
    void *sbservices = dlopen(
        "/System/Library/PrivateFrameworks/SpringBoardServices.framework/"
        "SpringBoardServices",
        RTLD_NOW);
    void *(*SBSSpringBoardServerPort)() = dlsym(sbservices, "SBSSpringBoardServerPort");
    if (SBSSpringBoardServerPort() == NULL)
      dispatch_main();
  }
  else
  {
    // sbreload();
  }
  printf("palera1n: goodbye!\n");
  printf("========================================\n");
  // startMonitoring();

  return 0;
}

int launchd_main(int argc, char **argv)
{
  int fd_console = open("/dev/console", O_RDWR);
  if (fd_console == -1) return -1; // crash
  dup2(fd_console, STDIN_FILENO);
  dup2(fd_console, STDOUT_FILENO);
  dup2(fd_console, STDERR_FILENO);
  int ret = get_paleinfo(&pinfo, RAMDISK);
  if (ret != 0)
  {
    fprintf(stderr, "cannot get paleinfo: ret: %d, errno: %d (%s)\n", ret, errno, strerror(errno));
    return 1;
  }
  if (checkrain_option_enabled(pinfo.flags, palerain_option_jbinit_log_to_file)) {
    int fd_log = open("/cores/jbinit.log", O_WRONLY | O_APPEND | O_SYNC, 0644);
    if (fd_log != -1) {
      dup2(fd_log, STDOUT_FILENO);
      dup2(fd_log, STDERR_FILENO);
      fputs("======== jbloader (early boot) log start =========", stderr);
    }
    else fputs("cannot open /cores/jbinit.log for logging", stderr);
  }
  if (getenv("XPC_USERSPACE_REBOOTED") == NULL) {
    int mount_ret = 0;
    puts("mounting overlay");
    mount_ret = check_and_mount_dmg();
    if (mount_ret)
      spin();
    puts("mounting loader");
    mount_ret = check_and_mount_loader();
    if (mount_ret)
      spin();
  }

  // patch_dyld();
  struct stat statbuf;
  {
    int err = 0;
    if ((err = stat("/sbin/launchd", &statbuf)))
    {
      fprintf(stderr, "stat /sbin/launchd FAILED with err=%d!\n", err);
      spin();
    }
    else
    {
      puts("stat /sbin/launchd OK");
    }
  }

  char *newenv = malloc(200);
  assert(newenv != NULL);
  char *env = getenv("DYLD_INSERT_LIBRARIES");
  if (env == NULL)
  {
    strncpy(newenv, "DYLD_INSERT_LIBRARIES=/cores/jb.dylib", 200);
  }
  else if (strstr(env, "/cores/jb.dylib") == NULL)
  {
    printf("Existing env: %s\n", env);
    newenv = realloc(newenv, strlen(env) + 200);
    assert(newenv != NULL);
    snprintf(newenv, strlen(env) + 200, "DYLD_INSERT_LIBRARIES=%s:/cores/jb.dylib", env);
  }
  else
  {
    newenv = realloc(newenv, 200 + strlen(env));
    assert(newenv != NULL);
    snprintf(newenv, strlen(env) + 200, "DYLD_INSERT_LIBRARIES=%s", env);
  }
  printf("newenv: %s\n", newenv);
  puts("Closing console, goodbye!");
  /*
    Launchd doesn't like it when the console is open already!
  */
  for (size_t i = 0; i < 10; i++)
  {
    close(i);
  }

  char *launchd_argv[] = {
      "/sbin/launchd",
      NULL};
  char *launchd_envp[] = {
      newenv,
      NULL};
  char *launchd_envp2[] = {
      newenv,
      "XPC_USERSPACE_REBOOTED=1",
      NULL};
  if (getenv("XPC_USERSPACE_REBOOTED") != NULL)
  {
    ret = execve(launchd_argv[0], launchd_argv, launchd_envp2);
  }
  else
  {
    ret = execve(launchd_argv[0], launchd_argv, launchd_envp);
  }

  fprintf(stderr, "execve FAILED with ret=%d\n", ret);
  spin();
  return -1;
}

int main(int argc, char **argv)
{
  if (getpid() == 1)
  {
    return launchd_main(argc, argv);
  }
  else
    return jbloader_main(argc, argv);
}