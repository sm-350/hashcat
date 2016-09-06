/**
 * Authors.....: Jens Steube <jens.steube@gmail.com>
 *               Gabriele Gristina <matrix@hashcat.net>
 *               magnum <john.magnum@hushmail.com>
 *
 * License.....: MIT
 */

#include "common.h"
#include "types_int.h"
#include "memory.h"
#include "logging.h"
#include "ext_OpenCL.h"
#include "ext_ADL.h"
#include "ext_nvapi.h"
#include "ext_nvml.h"
#include "ext_xnvctrl.h"
#include "convert.h"
#include "thread.h"
#include "timer.h"
#include "types.h"
#include "rp_cpu.h"
#include "data.h"
#include "shared.h"

extern hc_global_data_t data;

/**
 * tty
 */

#ifdef __linux__
static struct termios savemodes;
static int havemodes = 0;

static int tty_break()
{
  struct termios modmodes;

  if (tcgetattr (fileno (stdin), &savemodes) < 0) return -1;

  havemodes = 1;

  modmodes = savemodes;
  modmodes.c_lflag &= ~ICANON;
  modmodes.c_cc[VMIN] = 1;
  modmodes.c_cc[VTIME] = 0;

  return tcsetattr (fileno (stdin), TCSANOW, &modmodes);
}

static int tty_getchar()
{
  fd_set rfds;

  FD_ZERO (&rfds);

  FD_SET (fileno (stdin), &rfds);

  struct timeval tv;

  tv.tv_sec  = 1;
  tv.tv_usec = 0;

  int retval = select (1, &rfds, NULL, NULL, &tv);

  if (retval ==  0) return  0;
  if (retval == -1) return -1;

  return getchar();
}

static int tty_fix()
{
  if (!havemodes) return 0;

  return tcsetattr (fileno (stdin), TCSADRAIN, &savemodes);
}
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
static struct termios savemodes;
static int havemodes = 0;

int tty_break()
{
  struct termios modmodes;

  if (ioctl (fileno (stdin), TIOCGETA, &savemodes) < 0) return -1;

  havemodes = 1;

  modmodes = savemodes;
  modmodes.c_lflag &= ~ICANON;
  modmodes.c_cc[VMIN] = 1;
  modmodes.c_cc[VTIME] = 0;

  return ioctl (fileno (stdin), TIOCSETAW, &modmodes);
}

int tty_getchar()
{
  fd_set rfds;

  FD_ZERO (&rfds);

  FD_SET (fileno (stdin), &rfds);

  struct timeval tv;

  tv.tv_sec  = 1;
  tv.tv_usec = 0;

  int retval = select (1, &rfds, NULL, NULL, &tv);

  if (retval ==  0) return  0;
  if (retval == -1) return -1;

  return getchar();
}

int tty_fix()
{
  if (!havemodes) return 0;

  return ioctl (fileno (stdin), TIOCSETAW, &savemodes);
}
#endif

#ifdef WIN
static DWORD saveMode = 0;

static int tty_break()
{
  HANDLE stdinHandle = GetStdHandle (STD_INPUT_HANDLE);

  GetConsoleMode (stdinHandle, &saveMode);
  SetConsoleMode (stdinHandle, ENABLE_PROCESSED_INPUT);

  return 0;
}

static int tty_getchar()
{
  HANDLE stdinHandle = GetStdHandle (STD_INPUT_HANDLE);

  DWORD rc = WaitForSingleObject (stdinHandle, 1000);

  if (rc == WAIT_TIMEOUT)   return  0;
  if (rc == WAIT_ABANDONED) return -1;
  if (rc == WAIT_FAILED)    return -1;

  // The whole ReadConsoleInput () part is a workaround.
  // For some unknown reason, maybe a mingw bug, a random signal
  // is sent to stdin which unblocks WaitForSingleObject () and sets rc 0.
  // Then it wants to read with getche () a keyboard input
  // which has never been made.

  INPUT_RECORD buf[100];

  DWORD num = 0;

  memset (buf, 0, sizeof (buf));

  ReadConsoleInput (stdinHandle, buf, 100, &num);

  FlushConsoleInputBuffer (stdinHandle);

  for (uint i = 0; i < num; i++)
  {
    if (buf[i].EventType != KEY_EVENT) continue;

    KEY_EVENT_RECORD KeyEvent = buf[i].Event.KeyEvent;

    if (KeyEvent.bKeyDown != TRUE) continue;

    return KeyEvent.uChar.AsciiChar;
  }

  return 0;
}

static int tty_fix()
{
  HANDLE stdinHandle = GetStdHandle (STD_INPUT_HANDLE);

  SetConsoleMode (stdinHandle, saveMode);

  return 0;
}
#endif

/**
 * system
 */

#ifdef F_SETLKW
void lock_file (FILE *fp)
{
  struct flock lock;

  memset (&lock, 0, sizeof (struct flock));

  lock.l_type = F_WRLCK;
  while (fcntl(fileno(fp), F_SETLKW, &lock))
  {
    if (errno != EINTR)
    {
      log_error ("ERROR: Failed acquiring write lock: %s", strerror (errno));

      exit (-1);
    }
  }
}

void unlock_file (FILE *fp)
{
  struct flock lock;

  memset (&lock, 0, sizeof (struct flock));

  lock.l_type = F_UNLCK;
  fcntl(fileno(fp), F_SETLK, &lock);
}
#endif // F_SETLKW

#ifdef WIN
void fsync (int fd)
{
  HANDLE h = (HANDLE) _get_osfhandle (fd);

  FlushFileBuffers (h);
}
#endif

/**
 * thermal
 */

#ifdef HAVE_HWMON

int get_adapters_num_adl (void *adl, int *iNumberAdapters)
{
  if (hm_ADL_Adapter_NumberOfAdapters_Get ((ADL_PTR *) adl, iNumberAdapters) != ADL_OK) return -1;

  if (iNumberAdapters == 0)
  {
    log_info ("WARN: No ADL adapters found.");

    return -1;
  }

  return 0;
}

/*
int hm_show_performance_level (HM_LIB hm_dll, int iAdapterIndex)
{
  ADLODPerformanceLevels *lpOdPerformanceLevels = NULL;
  ADLODParameters lpOdParameters;

  lpOdParameters.iSize = sizeof (ADLODParameters);
  size_t plevels_size = 0;

  if (hm_ADL_Overdrive_ODParameters_Get (hm_dll, iAdapterIndex, &lpOdParameters) != ADL_OK) return -1;

  log_info ("[DEBUG] %s, adapter %d performance level (%d) : %s %s",
          __func__, iAdapterIndex,
          lpOdParameters.iNumberOfPerformanceLevels,
          (lpOdParameters.iActivityReportingSupported) ? "activity reporting" : "",
          (lpOdParameters.iDiscretePerformanceLevels) ? "discrete performance levels" : "performance ranges");

  plevels_size = sizeof (ADLODPerformanceLevels) + sizeof (ADLODPerformanceLevel) * (lpOdParameters.iNumberOfPerformanceLevels - 1);

  lpOdPerformanceLevels = (ADLODPerformanceLevels *) mymalloc (plevels_size);

  lpOdPerformanceLevels->iSize = sizeof (ADLODPerformanceLevels) + sizeof (ADLODPerformanceLevel) * (lpOdParameters.iNumberOfPerformanceLevels - 1);

  if (hm_ADL_Overdrive_ODPerformanceLevels_Get (hm_dll, iAdapterIndex, 0, lpOdPerformanceLevels) != ADL_OK) return -1;

  for (int j = 0; j < lpOdParameters.iNumberOfPerformanceLevels; j++)
    log_info ("[DEBUG] %s, adapter %d, level %d : engine %d, memory %d, voltage: %d",
    __func__, iAdapterIndex, j,
    lpOdPerformanceLevels->aLevels[j].iEngineClock / 100, lpOdPerformanceLevels->aLevels[j].iMemoryClock / 100, lpOdPerformanceLevels->aLevels[j].iVddc);

  myfree (lpOdPerformanceLevels);

  return 0;
}
*/

LPAdapterInfo hm_get_adapter_info_adl (void *adl, int iNumberAdapters)
{
  size_t AdapterInfoSize = iNumberAdapters * sizeof (AdapterInfo);

  LPAdapterInfo lpAdapterInfo = (LPAdapterInfo) mymalloc (AdapterInfoSize);

  if (hm_ADL_Adapter_AdapterInfo_Get ((ADL_PTR *) adl, lpAdapterInfo, AdapterInfoSize) != ADL_OK) return NULL;

  return lpAdapterInfo;
}

int hm_get_adapter_index_nvapi (HM_ADAPTER_NVAPI nvapiGPUHandle[DEVICES_MAX])
{
  NvU32 pGpuCount;

  if (hm_NvAPI_EnumPhysicalGPUs (data.hm_nvapi, nvapiGPUHandle, &pGpuCount) != NVAPI_OK) return 0;

  if (pGpuCount == 0)
  {
    log_info ("WARN: No NvAPI adapters found");

    return 0;
  }

  return (pGpuCount);
}

int hm_get_adapter_index_nvml (HM_ADAPTER_NVML nvmlGPUHandle[DEVICES_MAX])
{
  int pGpuCount = 0;

  for (uint i = 0; i < DEVICES_MAX; i++)
  {
    if (hm_NVML_nvmlDeviceGetHandleByIndex (data.hm_nvml, 1, i, &nvmlGPUHandle[i]) != NVML_SUCCESS) break;

    // can be used to determine if the device by index matches the cuda device by index
    // char name[100]; memset (name, 0, sizeof (name));
    // hm_NVML_nvmlDeviceGetName (data.hm_nvml, nvGPUHandle[i], name, sizeof (name) - 1);

    pGpuCount++;
  }

  if (pGpuCount == 0)
  {
    log_info ("WARN: No NVML adapters found");

    return 0;
  }

  return (pGpuCount);
}

/*
//
// does not help at all, since ADL does not assign different bus id, device id when we have multi GPU setups
//

int hm_get_opencl_device_index (hm_attrs_t *hm_device, uint num_adl_adapters, int bus_num, int dev_num)
{
  u32 idx = -1;

  for (uint i = 0; i < num_adl_adapters; i++)
  {
    int opencl_bus_num = hm_device[i].busid;
    int opencl_dev_num = hm_device[i].devid;

    if ((opencl_bus_num == bus_num) && (opencl_dev_num == dev_num))
    {
      idx = i;

      break;
    }
  }

  if (idx >= DEVICES_MAX) return -1;

  return idx;
}

void hm_get_opencl_busid_devid (hm_attrs_t *hm_device, uint opencl_num_devices, cl_device_id *devices)
{
  for (uint i = 0; i < opencl_num_devices; i++)
  {
    cl_device_topology_amd device_topology;

    hc_clGetDeviceInfo (devices[i], CL_DEVICE_TOPOLOGY_AMD, sizeof (device_topology), &device_topology, NULL);

    hm_device[i].busid = device_topology.pcie.bus;
    hm_device[i].devid = device_topology.pcie.device;
  }
}
*/

static void hm_sort_adl_adapters_by_busid_devid (u32 *valid_adl_device_list, int num_adl_adapters, LPAdapterInfo lpAdapterInfo)
{
  // basically bubble sort

  for (int i = 0; i < num_adl_adapters; i++)
  {
    for (int j = 0; j < num_adl_adapters - 1; j++)
    {
      // get info of adapter [x]

      u32 adapter_index_x = valid_adl_device_list[j];
      AdapterInfo info_x = lpAdapterInfo[adapter_index_x];

      u32 bus_num_x = info_x.iBusNumber;
      u32 dev_num_x = info_x.iDeviceNumber;

      // get info of adapter [y]

      u32 adapter_index_y = valid_adl_device_list[j + 1];
      AdapterInfo info_y = lpAdapterInfo[adapter_index_y];

      u32 bus_num_y = info_y.iBusNumber;
      u32 dev_num_y = info_y.iDeviceNumber;

      uint need_swap = 0;

      if (bus_num_y < bus_num_x)
      {
        need_swap = 1;
      }
      else if (bus_num_y == bus_num_x)
      {
        if (dev_num_y < dev_num_x)
        {
          need_swap = 1;
        }
      }

      if (need_swap == 1)
      {
        u32 temp = valid_adl_device_list[j + 1];

        valid_adl_device_list[j + 1] = valid_adl_device_list[j];
        valid_adl_device_list[j + 0] = temp;
      }
    }
  }
}

u32 *hm_get_list_valid_adl_adapters (int iNumberAdapters, int *num_adl_adapters, LPAdapterInfo lpAdapterInfo)
{
  *num_adl_adapters = 0;

  u32 *adl_adapters = NULL;

  int *bus_numbers    = NULL;
  int *device_numbers = NULL;

  for (int i = 0; i < iNumberAdapters; i++)
  {
    AdapterInfo info = lpAdapterInfo[i];

    if (strlen (info.strUDID) < 1) continue;

    #ifdef WIN
    if (info.iVendorID !=   1002) continue;
    #else
    if (info.iVendorID != 0x1002) continue;
    #endif

    if (info.iBusNumber    < 0) continue;
    if (info.iDeviceNumber < 0) continue;

    int found = 0;

    for (int pos = 0; pos < *num_adl_adapters; pos++)
    {
      if ((bus_numbers[pos] == info.iBusNumber) && (device_numbers[pos] == info.iDeviceNumber))
      {
        found = 1;
        break;
      }
    }

    if (found) continue;

    // add it to the list

    adl_adapters = (u32 *) myrealloc (adl_adapters, (*num_adl_adapters) * sizeof (int), sizeof (int));

    adl_adapters[*num_adl_adapters] = i;

    // rest is just bookkeeping

    bus_numbers    = (int*) myrealloc (bus_numbers,    (*num_adl_adapters) * sizeof (int), sizeof (int));
    device_numbers = (int*) myrealloc (device_numbers, (*num_adl_adapters) * sizeof (int), sizeof (int));

    bus_numbers[*num_adl_adapters]    = info.iBusNumber;
    device_numbers[*num_adl_adapters] = info.iDeviceNumber;

    (*num_adl_adapters)++;
  }

  myfree (bus_numbers);
  myfree (device_numbers);

  // sort the list by increasing bus id, device id number

  hm_sort_adl_adapters_by_busid_devid (adl_adapters, *num_adl_adapters, lpAdapterInfo);

  return adl_adapters;
}

int hm_check_fanspeed_control (void *adl, hm_attrs_t *hm_device, u32 *valid_adl_device_list, int num_adl_adapters, LPAdapterInfo lpAdapterInfo)
{
  // loop through all valid devices

  for (int i = 0; i < num_adl_adapters; i++)
  {
    u32 adapter_index = valid_adl_device_list[i];

    // get AdapterInfo

    AdapterInfo info = lpAdapterInfo[adapter_index];

    // unfortunately this doesn't work since bus id and dev id are not unique
    // int opencl_device_index = hm_get_opencl_device_index (hm_device, num_adl_adapters, info.iBusNumber, info.iDeviceNumber);
    // if (opencl_device_index == -1) continue;

    int opencl_device_index = i;

    // if (hm_show_performance_level (adl, info.iAdapterIndex) != 0) return -1;

    // get fanspeed info

    if (hm_device[opencl_device_index].od_version == 5)
    {
      ADLFanSpeedInfo FanSpeedInfo;

      memset (&FanSpeedInfo, 0, sizeof (ADLFanSpeedInfo));

      FanSpeedInfo.iSize = sizeof (ADLFanSpeedInfo);

      if (hm_ADL_Overdrive5_FanSpeedInfo_Get (adl, info.iAdapterIndex, 0, &FanSpeedInfo) != ADL_OK) return -1;

      // check read and write capability in fanspeedinfo

      if ((FanSpeedInfo.iFlags & ADL_DL_FANCTRL_SUPPORTS_PERCENT_READ) &&
          (FanSpeedInfo.iFlags & ADL_DL_FANCTRL_SUPPORTS_PERCENT_WRITE))
      {
        hm_device[opencl_device_index].fan_get_supported = 1;
      }
      else
      {
        hm_device[opencl_device_index].fan_get_supported = 0;
      }
    }
    else // od_version == 6
    {
      ADLOD6FanSpeedInfo faninfo;

      memset (&faninfo, 0, sizeof (faninfo));

      if (hm_ADL_Overdrive6_FanSpeed_Get (adl, info.iAdapterIndex, &faninfo) != ADL_OK) return -1;

      // check read capability in fanspeedinfo

      if (faninfo.iSpeedType & ADL_OD6_FANSPEED_TYPE_PERCENT)
      {
        hm_device[opencl_device_index].fan_get_supported = 1;
      }
      else
      {
        hm_device[opencl_device_index].fan_get_supported = 0;
      }
    }
  }

  return 0;
}

int hm_get_overdrive_version (void *adl, hm_attrs_t *hm_device, u32 *valid_adl_device_list, int num_adl_adapters, LPAdapterInfo lpAdapterInfo)
{
  for (int i = 0; i < num_adl_adapters; i++)
  {
    u32 adapter_index = valid_adl_device_list[i];

    // get AdapterInfo

    AdapterInfo info = lpAdapterInfo[adapter_index];

    // get overdrive version

    int od_supported = 0;
    int od_enabled   = 0;
    int od_version   = 0;

    if (hm_ADL_Overdrive_Caps (adl, info.iAdapterIndex, &od_supported, &od_enabled, &od_version) != ADL_OK) return -1;

    // store the overdrive version in hm_device

    // unfortunately this doesn't work since bus id and dev id are not unique
    // int opencl_device_index = hm_get_opencl_device_index (hm_device, num_adl_adapters, info.iBusNumber, info.iDeviceNumber);
    // if (opencl_device_index == -1) continue;

    int opencl_device_index = i;

    hm_device[opencl_device_index].od_version = od_version;
  }

  return 0;
}

int hm_get_adapter_index_adl (hm_attrs_t *hm_device, u32 *valid_adl_device_list, int num_adl_adapters, LPAdapterInfo lpAdapterInfo)
{
  for (int i = 0; i < num_adl_adapters; i++)
  {
    u32 adapter_index = valid_adl_device_list[i];

    // get AdapterInfo

    AdapterInfo info = lpAdapterInfo[adapter_index];

    // store the iAdapterIndex in hm_device

    // unfortunately this doesn't work since bus id and dev id are not unique
    // int opencl_device_index = hm_get_opencl_device_index (hm_device, num_adl_adapters, info.iBusNumber, info.iDeviceNumber);
    // if (opencl_device_index == -1) continue;

    int opencl_device_index = i;

    hm_device[opencl_device_index].adl = info.iAdapterIndex;
  }

  return num_adl_adapters;
}

int hm_get_threshold_slowdown_with_device_id (const uint device_id)
{
  if ((data.devices_param[device_id].device_type & CL_DEVICE_TYPE_GPU) == 0) return -1;

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_AMD)
  {
    if (data.hm_adl)
    {
      if (data.hm_device[device_id].od_version == 5)
      {

      }
      else if (data.hm_device[device_id].od_version == 6)
      {
        int CurrentValue = 0;
        int DefaultValue = 0;

        if (hm_ADL_Overdrive6_TargetTemperatureData_Get (data.hm_adl, data.hm_device[device_id].adl, &CurrentValue, &DefaultValue) != ADL_OK) return -1;

        // the return value has never been tested since hm_ADL_Overdrive6_TargetTemperatureData_Get() never worked on any system. expect problems.

        return DefaultValue;
      }
    }
  }

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_NV)
  {
    int target = 0;

    if (hm_NVML_nvmlDeviceGetTemperatureThreshold (data.hm_nvml, 1, data.hm_device[device_id].nvml, NVML_TEMPERATURE_THRESHOLD_SLOWDOWN, (unsigned int *) &target) != NVML_SUCCESS) return -1;

    return target;
  }

  return -1;
}

int hm_get_threshold_shutdown_with_device_id (const uint device_id)
{
  if ((data.devices_param[device_id].device_type & CL_DEVICE_TYPE_GPU) == 0) return -1;

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_AMD)
  {
    if (data.hm_adl)
    {
      if (data.hm_device[device_id].od_version == 5)
      {

      }
      else if (data.hm_device[device_id].od_version == 6)
      {

      }
    }
  }

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_NV)
  {
    int target = 0;

    if (hm_NVML_nvmlDeviceGetTemperatureThreshold (data.hm_nvml, 1, data.hm_device[device_id].nvml, NVML_TEMPERATURE_THRESHOLD_SHUTDOWN, (unsigned int *) &target) != NVML_SUCCESS) return -1;

    return target;
  }

  return -1;
}

int hm_get_temperature_with_device_id (const uint device_id)
{
  if ((data.devices_param[device_id].device_type & CL_DEVICE_TYPE_GPU) == 0) return -1;

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_AMD)
  {
    if (data.hm_adl)
    {
      if (data.hm_device[device_id].od_version == 5)
      {
        ADLTemperature Temperature;

        Temperature.iSize = sizeof (ADLTemperature);

        if (hm_ADL_Overdrive5_Temperature_Get (data.hm_adl, data.hm_device[device_id].adl, 0, &Temperature) != ADL_OK) return -1;

        return Temperature.iTemperature / 1000;
      }
      else if (data.hm_device[device_id].od_version == 6)
      {
        int Temperature = 0;

        if (hm_ADL_Overdrive6_Temperature_Get (data.hm_adl, data.hm_device[device_id].adl, &Temperature) != ADL_OK) return -1;

        return Temperature / 1000;
      }
    }
  }

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_NV)
  {
    int temperature = 0;

    if (hm_NVML_nvmlDeviceGetTemperature (data.hm_nvml, 1, data.hm_device[device_id].nvml, NVML_TEMPERATURE_GPU, (uint *) &temperature) != NVML_SUCCESS) return -1;

    return temperature;
  }

  return -1;
}

int hm_get_fanpolicy_with_device_id (const uint device_id)
{
  if ((data.devices_param[device_id].device_type & CL_DEVICE_TYPE_GPU) == 0) return -1;

  if (data.hm_device[device_id].fan_get_supported == 1)
  {
    if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_AMD)
    {
      if (data.hm_adl)
      {
        if (data.hm_device[device_id].od_version == 5)
        {
          ADLFanSpeedValue lpFanSpeedValue;

          memset (&lpFanSpeedValue, 0, sizeof (lpFanSpeedValue));

          lpFanSpeedValue.iSize      = sizeof (lpFanSpeedValue);
          lpFanSpeedValue.iSpeedType = ADL_DL_FANCTRL_SPEED_TYPE_PERCENT;

          if (hm_ADL_Overdrive5_FanSpeed_Get (data.hm_adl, data.hm_device[device_id].adl, 0, &lpFanSpeedValue) != ADL_OK) return -1;

          return (lpFanSpeedValue.iFanSpeed & ADL_DL_FANCTRL_FLAG_USER_DEFINED_SPEED) ? 0 : 1;
        }
        else // od_version == 6
        {
          return 1;
        }
      }
    }

    if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_NV)
    {
      return 1;
    }
  }

  return -1;
}

int hm_get_fanspeed_with_device_id (const uint device_id)
{
  if ((data.devices_param[device_id].device_type & CL_DEVICE_TYPE_GPU) == 0) return -1;

  if (data.hm_device[device_id].fan_get_supported == 1)
  {
    if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_AMD)
    {
      if (data.hm_adl)
      {
        if (data.hm_device[device_id].od_version == 5)
        {
          ADLFanSpeedValue lpFanSpeedValue;

          memset (&lpFanSpeedValue, 0, sizeof (lpFanSpeedValue));

          lpFanSpeedValue.iSize      = sizeof (lpFanSpeedValue);
          lpFanSpeedValue.iSpeedType = ADL_DL_FANCTRL_SPEED_TYPE_PERCENT;
          lpFanSpeedValue.iFlags     = ADL_DL_FANCTRL_FLAG_USER_DEFINED_SPEED;

          if (hm_ADL_Overdrive5_FanSpeed_Get (data.hm_adl, data.hm_device[device_id].adl, 0, &lpFanSpeedValue) != ADL_OK) return -1;

          return lpFanSpeedValue.iFanSpeed;
        }
        else // od_version == 6
        {
          ADLOD6FanSpeedInfo faninfo;

          memset (&faninfo, 0, sizeof (faninfo));

          if (hm_ADL_Overdrive6_FanSpeed_Get (data.hm_adl, data.hm_device[device_id].adl, &faninfo) != ADL_OK) return -1;

          return faninfo.iFanSpeedPercent;
        }
      }
    }

    if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_NV)
    {
      int speed = 0;

      if (hm_NVML_nvmlDeviceGetFanSpeed (data.hm_nvml, 0, data.hm_device[device_id].nvml, (uint *) &speed) != NVML_SUCCESS) return -1;

      return speed;
    }
  }

  return -1;
}

int hm_get_buslanes_with_device_id (const uint device_id)
{
  if ((data.devices_param[device_id].device_type & CL_DEVICE_TYPE_GPU) == 0) return -1;

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_AMD)
  {
    if (data.hm_adl)
    {
      ADLPMActivity PMActivity;

      PMActivity.iSize = sizeof (ADLPMActivity);

      if (hm_ADL_Overdrive_CurrentActivity_Get (data.hm_adl, data.hm_device[device_id].adl, &PMActivity) != ADL_OK) return -1;

      return PMActivity.iCurrentBusLanes;
    }
  }

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_NV)
  {
    unsigned int currLinkWidth;

    if (hm_NVML_nvmlDeviceGetCurrPcieLinkWidth (data.hm_nvml, 1, data.hm_device[device_id].nvml, &currLinkWidth) != NVML_SUCCESS) return -1;

    return currLinkWidth;
  }

  return -1;
}

int hm_get_utilization_with_device_id (const uint device_id)
{
  if ((data.devices_param[device_id].device_type & CL_DEVICE_TYPE_GPU) == 0) return -1;

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_AMD)
  {
    if (data.hm_adl)
    {
      ADLPMActivity PMActivity;

      PMActivity.iSize = sizeof (ADLPMActivity);

      if (hm_ADL_Overdrive_CurrentActivity_Get (data.hm_adl, data.hm_device[device_id].adl, &PMActivity) != ADL_OK) return -1;

      return PMActivity.iActivityPercent;
    }
  }

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_NV)
  {
    nvmlUtilization_t utilization;

    if (hm_NVML_nvmlDeviceGetUtilizationRates (data.hm_nvml, 1, data.hm_device[device_id].nvml, &utilization) != NVML_SUCCESS) return -1;

    return utilization.gpu;
  }

  return -1;
}

int hm_get_memoryspeed_with_device_id (const uint device_id)
{
  if ((data.devices_param[device_id].device_type & CL_DEVICE_TYPE_GPU) == 0) return -1;

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_AMD)
  {
    if (data.hm_adl)
    {
      ADLPMActivity PMActivity;

      PMActivity.iSize = sizeof (ADLPMActivity);

      if (hm_ADL_Overdrive_CurrentActivity_Get (data.hm_adl, data.hm_device[device_id].adl, &PMActivity) != ADL_OK) return -1;

      return PMActivity.iMemoryClock / 100;
    }
  }

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_NV)
  {
    unsigned int clock;

    if (hm_NVML_nvmlDeviceGetClockInfo (data.hm_nvml, 1, data.hm_device[device_id].nvml, NVML_CLOCK_MEM, &clock) != NVML_SUCCESS) return -1;

    return clock;
  }

  return -1;
}

int hm_get_corespeed_with_device_id (const uint device_id)
{
  if ((data.devices_param[device_id].device_type & CL_DEVICE_TYPE_GPU) == 0) return -1;

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_AMD)
  {
    if (data.hm_adl)
    {
      ADLPMActivity PMActivity;

      PMActivity.iSize = sizeof (ADLPMActivity);

      if (hm_ADL_Overdrive_CurrentActivity_Get (data.hm_adl, data.hm_device[device_id].adl, &PMActivity) != ADL_OK) return -1;

      return PMActivity.iEngineClock / 100;
    }
  }

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_NV)
  {
    unsigned int clock;

    if (hm_NVML_nvmlDeviceGetClockInfo (data.hm_nvml, 1, data.hm_device[device_id].nvml, NVML_CLOCK_SM, &clock) != NVML_SUCCESS) return -1;

    return clock;
  }

  return -1;
}

int hm_get_throttle_with_device_id (const uint device_id)
{
  if ((data.devices_param[device_id].device_type & CL_DEVICE_TYPE_GPU) == 0) return -1;

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_AMD)
  {

  }

  if (data.devices_param[device_id].device_vendor_id == VENDOR_ID_NV)
  {
    unsigned long long clocksThrottleReasons = 0;
    unsigned long long supportedThrottleReasons = 0;

    if (hm_NVML_nvmlDeviceGetCurrentClocksThrottleReasons   (data.hm_nvml, 1, data.hm_device[device_id].nvml, &clocksThrottleReasons)    != NVML_SUCCESS) return -1;
    if (hm_NVML_nvmlDeviceGetSupportedClocksThrottleReasons (data.hm_nvml, 1, data.hm_device[device_id].nvml, &supportedThrottleReasons) != NVML_SUCCESS) return -1;

    clocksThrottleReasons &=  supportedThrottleReasons;
    clocksThrottleReasons &= ~nvmlClocksThrottleReasonGpuIdle;
    clocksThrottleReasons &= ~nvmlClocksThrottleReasonApplicationsClocksSetting;
    clocksThrottleReasons &= ~nvmlClocksThrottleReasonUnknown;

    if (data.kernel_power_final)
    {
      clocksThrottleReasons &= ~nvmlClocksThrottleReasonHwSlowdown;
    }

    return (clocksThrottleReasons != nvmlClocksThrottleReasonNone);
  }

  return -1;
}

int hm_set_fanspeed_with_device_id_adl (const uint device_id, const int fanspeed, const int fanpolicy)
{
  if (data.hm_device[device_id].fan_set_supported == 1)
  {
    if (data.hm_adl)
    {
      if (fanpolicy == 1)
      {
        if (data.hm_device[device_id].od_version == 5)
        {
          ADLFanSpeedValue lpFanSpeedValue;

          memset (&lpFanSpeedValue, 0, sizeof (lpFanSpeedValue));

          lpFanSpeedValue.iSize      = sizeof (lpFanSpeedValue);
          lpFanSpeedValue.iSpeedType = ADL_DL_FANCTRL_SPEED_TYPE_PERCENT;
          lpFanSpeedValue.iFlags     = ADL_DL_FANCTRL_FLAG_USER_DEFINED_SPEED;
          lpFanSpeedValue.iFanSpeed  = fanspeed;

          if (hm_ADL_Overdrive5_FanSpeed_Set (data.hm_adl, data.hm_device[device_id].adl, 0, &lpFanSpeedValue) != ADL_OK) return -1;

          return 0;
        }
        else // od_version == 6
        {
          ADLOD6FanSpeedValue fan_speed_value;

          memset (&fan_speed_value, 0, sizeof (fan_speed_value));

          fan_speed_value.iSpeedType = ADL_OD6_FANSPEED_TYPE_PERCENT;
          fan_speed_value.iFanSpeed  = fanspeed;

          if (hm_ADL_Overdrive6_FanSpeed_Set (data.hm_adl, data.hm_device[device_id].adl, &fan_speed_value) != ADL_OK) return -1;

          return 0;
        }
      }
      else
      {
        if (data.hm_device[device_id].od_version == 5)
        {
          if (hm_ADL_Overdrive5_FanSpeedToDefault_Set (data.hm_adl, data.hm_device[device_id].adl, 0) != ADL_OK) return -1;

          return 0;
        }
        else // od_version == 6
        {
          if (hm_ADL_Overdrive6_FanSpeed_Reset (data.hm_adl, data.hm_device[device_id].adl) != ADL_OK) return -1;

          return 0;
        }
      }
    }
  }

  return -1;
}

int hm_set_fanspeed_with_device_id_nvapi (const uint device_id, const int fanspeed, const int fanpolicy)
{
  if (data.hm_device[device_id].fan_set_supported == 1)
  {
    if (data.hm_nvapi)
    {
      if (fanpolicy == 1)
      {
        NV_GPU_COOLER_LEVELS CoolerLevels;

        memset (&CoolerLevels, 0, sizeof (NV_GPU_COOLER_LEVELS));

        CoolerLevels.Version = GPU_COOLER_LEVELS_VER | sizeof (NV_GPU_COOLER_LEVELS);

        CoolerLevels.Levels[0].Level  = fanspeed;
        CoolerLevels.Levels[0].Policy = 1;

        if (hm_NvAPI_GPU_SetCoolerLevels (data.hm_nvapi, data.hm_device[device_id].nvapi, 0, &CoolerLevels) != NVAPI_OK) return -1;

        return 0;
      }
      else
      {
        if (hm_NvAPI_GPU_RestoreCoolerSettings (data.hm_nvapi, data.hm_device[device_id].nvapi, 0) != NVAPI_OK) return -1;

        return 0;
      }
    }
  }

  return -1;
}

int hm_set_fanspeed_with_device_id_xnvctrl (const uint device_id, const int fanspeed)
{
  if (data.hm_device[device_id].fan_set_supported == 1)
  {
    if (data.hm_xnvctrl)
    {
      if (set_fan_speed_target (data.hm_xnvctrl, data.hm_device[device_id].xnvctrl, fanspeed) != 0) return -1;

      return 0;
    }
  }

  return -1;
}

#endif // HAVE_HWMON

/**
 * maskprocessor
 */

void mp_css_to_uniq_tbl (uint css_cnt, cs_t *css, uint uniq_tbls[SP_PW_MAX][CHARSIZ])
{
  /* generates a lookup table where key is the char itself for fastest possible lookup performance */

  if (css_cnt > SP_PW_MAX)
  {
    log_error ("ERROR: Mask length is too long");

    exit (-1);
  }

  for (uint css_pos = 0; css_pos < css_cnt; css_pos++)
  {
    uint *uniq_tbl = uniq_tbls[css_pos];

    uint *cs_buf = css[css_pos].cs_buf;
    uint  cs_len = css[css_pos].cs_len;

    for (uint cs_pos = 0; cs_pos < cs_len; cs_pos++)
    {
      uint c = cs_buf[cs_pos] & 0xff;

      uniq_tbl[c] = 1;
    }
  }
}

static void mp_add_cs_buf (uint *in_buf, size_t in_len, cs_t *css, int css_cnt)
{
  cs_t *cs = &css[css_cnt];

  size_t css_uniq_sz = CHARSIZ * sizeof (uint);

  uint *css_uniq = (uint *) mymalloc (css_uniq_sz);

  size_t i;

  for (i = 0; i < cs->cs_len; i++)
  {
    const uint u = cs->cs_buf[i];

    css_uniq[u] = 1;
  }

  for (i = 0; i < in_len; i++)
  {
    uint u = in_buf[i] & 0xff;

    if (data.opts_type & OPTS_TYPE_PT_UPPER) u = (uint) toupper (u);

    if (css_uniq[u] == 1) continue;

    css_uniq[u] = 1;

    cs->cs_buf[cs->cs_len] = u;

    cs->cs_len++;
  }

  myfree (css_uniq);
}

static void mp_expand (char *in_buf, size_t in_len, cs_t *mp_sys, cs_t *mp_usr, int mp_usr_offset, int interpret)
{
  size_t in_pos;

  for (in_pos = 0; in_pos < in_len; in_pos++)
  {
    uint p0 = in_buf[in_pos] & 0xff;

    if (interpret == 1 && p0 == '?')
    {
      in_pos++;

      if (in_pos == in_len) break;

      uint p1 = in_buf[in_pos] & 0xff;

      switch (p1)
      {
        case 'l': mp_add_cs_buf (mp_sys[0].cs_buf, mp_sys[0].cs_len, mp_usr, mp_usr_offset);
                  break;
        case 'u': mp_add_cs_buf (mp_sys[1].cs_buf, mp_sys[1].cs_len, mp_usr, mp_usr_offset);
                  break;
        case 'd': mp_add_cs_buf (mp_sys[2].cs_buf, mp_sys[2].cs_len, mp_usr, mp_usr_offset);
                  break;
        case 's': mp_add_cs_buf (mp_sys[3].cs_buf, mp_sys[3].cs_len, mp_usr, mp_usr_offset);
                  break;
        case 'a': mp_add_cs_buf (mp_sys[4].cs_buf, mp_sys[4].cs_len, mp_usr, mp_usr_offset);
                  break;
        case 'b': mp_add_cs_buf (mp_sys[5].cs_buf, mp_sys[5].cs_len, mp_usr, mp_usr_offset);
                  break;
        case '1': if (mp_usr[0].cs_len == 0) { log_error ("ERROR: Custom-charset 1 is undefined\n"); exit (-1); }
                  mp_add_cs_buf (mp_usr[0].cs_buf, mp_usr[0].cs_len, mp_usr, mp_usr_offset);
                  break;
        case '2': if (mp_usr[1].cs_len == 0) { log_error ("ERROR: Custom-charset 2 is undefined\n"); exit (-1); }
                  mp_add_cs_buf (mp_usr[1].cs_buf, mp_usr[1].cs_len, mp_usr, mp_usr_offset);
                  break;
        case '3': if (mp_usr[2].cs_len == 0) { log_error ("ERROR: Custom-charset 3 is undefined\n"); exit (-1); }
                  mp_add_cs_buf (mp_usr[2].cs_buf, mp_usr[2].cs_len, mp_usr, mp_usr_offset);
                  break;
        case '4': if (mp_usr[3].cs_len == 0) { log_error ("ERROR: Custom-charset 4 is undefined\n"); exit (-1); }
                  mp_add_cs_buf (mp_usr[3].cs_buf, mp_usr[3].cs_len, mp_usr, mp_usr_offset);
                  break;
        case '?': mp_add_cs_buf (&p0, 1, mp_usr, mp_usr_offset);
                  break;
        default:  log_error ("Syntax error: %s", in_buf);
                  exit (-1);
      }
    }
    else
    {
      if (data.hex_charset)
      {
        in_pos++;

        if (in_pos == in_len)
        {
          log_error ("ERROR: The hex-charset option always expects couples of exactly 2 hexadecimal chars, failed mask: %s", in_buf);

          exit (-1);
        }

        uint p1 = in_buf[in_pos] & 0xff;

        if ((is_valid_hex_char (p0) == 0) || (is_valid_hex_char (p1) == 0))
        {
          log_error ("ERROR: Invalid hex character detected in mask %s", in_buf);

          exit (-1);
        }

        uint chr = 0;

        chr  = hex_convert (p1) << 0;
        chr |= hex_convert (p0) << 4;

        mp_add_cs_buf (&chr, 1, mp_usr, mp_usr_offset);
      }
      else
      {
        uint chr = p0;

        mp_add_cs_buf (&chr, 1, mp_usr, mp_usr_offset);
      }
    }
  }
}

u64 mp_get_sum (uint css_cnt, cs_t *css)
{
  u64 sum = 1;

  for (uint css_pos = 0; css_pos < css_cnt; css_pos++)
  {
    sum *= css[css_pos].cs_len;
  }

  return (sum);
}

cs_t *mp_gen_css (char *mask_buf, size_t mask_len, cs_t *mp_sys, cs_t *mp_usr, uint *css_cnt)
{
  cs_t *css = (cs_t *) mycalloc (256, sizeof (cs_t));

  uint mask_pos;
  uint css_pos;

  for (mask_pos = 0, css_pos = 0; mask_pos < mask_len; mask_pos++, css_pos++)
  {
    char p0 = mask_buf[mask_pos];

    if (p0 == '?')
    {
      mask_pos++;

      if (mask_pos == mask_len) break;

      char p1 = mask_buf[mask_pos];

      uint chr = p1;

      switch (p1)
      {
        case 'l': mp_add_cs_buf (mp_sys[0].cs_buf, mp_sys[0].cs_len, css, css_pos);
                  break;
        case 'u': mp_add_cs_buf (mp_sys[1].cs_buf, mp_sys[1].cs_len, css, css_pos);
                  break;
        case 'd': mp_add_cs_buf (mp_sys[2].cs_buf, mp_sys[2].cs_len, css, css_pos);
                  break;
        case 's': mp_add_cs_buf (mp_sys[3].cs_buf, mp_sys[3].cs_len, css, css_pos);
                  break;
        case 'a': mp_add_cs_buf (mp_sys[4].cs_buf, mp_sys[4].cs_len, css, css_pos);
                  break;
        case 'b': mp_add_cs_buf (mp_sys[5].cs_buf, mp_sys[5].cs_len, css, css_pos);
                  break;
        case '1': if (mp_usr[0].cs_len == 0) { log_error ("ERROR: Custom-charset 1 is undefined\n"); exit (-1); }
                  mp_add_cs_buf (mp_usr[0].cs_buf, mp_usr[0].cs_len, css, css_pos);
                  break;
        case '2': if (mp_usr[1].cs_len == 0) { log_error ("ERROR: Custom-charset 2 is undefined\n"); exit (-1); }
                  mp_add_cs_buf (mp_usr[1].cs_buf, mp_usr[1].cs_len, css, css_pos);
                  break;
        case '3': if (mp_usr[2].cs_len == 0) { log_error ("ERROR: Custom-charset 3 is undefined\n"); exit (-1); }
                  mp_add_cs_buf (mp_usr[2].cs_buf, mp_usr[2].cs_len, css, css_pos);
                  break;
        case '4': if (mp_usr[3].cs_len == 0) { log_error ("ERROR: Custom-charset 4 is undefined\n"); exit (-1); }
                  mp_add_cs_buf (mp_usr[3].cs_buf, mp_usr[3].cs_len, css, css_pos);
                  break;
        case '?': mp_add_cs_buf (&chr, 1, css, css_pos);
                  break;
        default:  log_error ("ERROR: Syntax error: %s", mask_buf);
                  exit (-1);
      }
    }
    else
    {
      if (data.hex_charset)
      {
        mask_pos++;

        // if there is no 2nd hex character, show an error:

        if (mask_pos == mask_len)
        {
          log_error ("ERROR: The hex-charset option always expects couples of exactly 2 hexadecimal chars, failed mask: %s", mask_buf);

          exit (-1);
        }

        char p1 = mask_buf[mask_pos];

        // if they are not valid hex character, show an error:

        if ((is_valid_hex_char (p0) == 0) || (is_valid_hex_char (p1) == 0))
        {
          log_error ("ERROR: Invalid hex character detected in mask %s", mask_buf);

          exit (-1);
        }

        uint chr = 0;

        chr |= hex_convert (p1) << 0;
        chr |= hex_convert (p0) << 4;

        mp_add_cs_buf (&chr, 1, css, css_pos);
      }
      else
      {
        uint chr = p0;

        mp_add_cs_buf (&chr, 1, css, css_pos);
      }
    }
  }

  if (css_pos == 0)
  {
    log_error ("ERROR: Invalid mask length (0)");

    exit (-1);
  }

  *css_cnt = css_pos;

  return (css);
}

void mp_exec (u64 val, char *buf, cs_t *css, int css_cnt)
{
  for (int i = 0; i < css_cnt; i++)
  {
    uint len  = css[i].cs_len;
    u64 next = val / len;
    uint pos  = val % len;
    buf[i] = (char) css[i].cs_buf[pos] & 0xff;
    val = next;
  }
}

void mp_cut_at (char *mask, uint max)
{
  uint i;
  uint j;
  uint mask_len = strlen (mask);

  for (i = 0, j = 0; i < mask_len && j < max; i++, j++)
  {
    if (mask[i] == '?') i++;
  }

  mask[i] = 0;
}

void mp_setup_sys (cs_t *mp_sys)
{
  uint pos;
  uint chr;
  uint donec[CHARSIZ] = { 0 };

  for (pos = 0, chr =  'a'; chr <=  'z'; chr++) { donec[chr] = 1;
                                                  mp_sys[0].cs_buf[pos++] = chr;
                                                  mp_sys[0].cs_len = pos; }

  for (pos = 0, chr =  'A'; chr <=  'Z'; chr++) { donec[chr] = 1;
                                                  mp_sys[1].cs_buf[pos++] = chr;
                                                  mp_sys[1].cs_len = pos; }

  for (pos = 0, chr =  '0'; chr <=  '9'; chr++) { donec[chr] = 1;
                                                  mp_sys[2].cs_buf[pos++] = chr;
                                                  mp_sys[2].cs_len = pos; }

  for (pos = 0, chr = 0x20; chr <= 0x7e; chr++) { if (donec[chr]) continue;
                                                  mp_sys[3].cs_buf[pos++] = chr;
                                                  mp_sys[3].cs_len = pos; }

  for (pos = 0, chr = 0x20; chr <= 0x7e; chr++) { mp_sys[4].cs_buf[pos++] = chr;
                                                  mp_sys[4].cs_len = pos; }

  for (pos = 0, chr = 0x00; chr <= 0xff; chr++) { mp_sys[5].cs_buf[pos++] = chr;
                                                  mp_sys[5].cs_len = pos; }
}

void mp_setup_usr (cs_t *mp_sys, cs_t *mp_usr, char *buf, uint index)
{
  FILE *fp = fopen (buf, "rb");

  if (fp == NULL || feof (fp)) // feof() in case if file is empty
  {
    mp_expand (buf, strlen (buf), mp_sys, mp_usr, index, 1);
  }
  else
  {
    char mp_file[1024] = { 0 };

    size_t len = fread (mp_file, 1, sizeof (mp_file) - 1, fp);

    fclose (fp);

    len = in_superchop (mp_file);

    if (len == 0)
    {
      log_info ("WARNING: Charset file corrupted");

      mp_expand (buf, strlen (buf), mp_sys, mp_usr, index, 1);
    }
    else
    {
      mp_expand (mp_file, len, mp_sys, mp_usr, index, 0);
    }
  }
}

void mp_reset_usr (cs_t *mp_usr, uint index)
{
  mp_usr[index].cs_len = 0;

  memset (mp_usr[index].cs_buf, 0, sizeof (mp_usr[index].cs_buf));
}

char *mp_get_truncated_mask (char *mask_buf, size_t mask_len, uint len)
{
  char *new_mask_buf = (char *) mymalloc (256);

  uint mask_pos;

  uint css_pos;

  for (mask_pos = 0, css_pos = 0; mask_pos < mask_len; mask_pos++, css_pos++)
  {
    if (css_pos == len) break;

    char p0 = mask_buf[mask_pos];

    new_mask_buf[mask_pos] = p0;

    if (p0 == '?')
    {
      mask_pos++;

      if (mask_pos == mask_len) break;

      new_mask_buf[mask_pos] = mask_buf[mask_pos];
    }
    else
    {
      if (data.hex_charset)
      {
        mask_pos++;

        if (mask_pos == mask_len)
        {
          log_error ("ERROR: The hex-charset option always expects couples of exactly 2 hexadecimal chars, failed mask: %s", mask_buf);

          exit (-1);
        }

        char p1 = mask_buf[mask_pos];

        // if they are not valid hex character, show an error:

        if ((is_valid_hex_char (p0) == 0) || (is_valid_hex_char (p1) == 0))
        {
          log_error ("ERROR: Invalid hex character detected in mask: %s", mask_buf);

          exit (-1);
        }

        new_mask_buf[mask_pos] = p1;
      }
    }
  }

  if (css_pos == len) return (new_mask_buf);

  myfree (new_mask_buf);

  return (NULL);
}

/**
 * statprocessor
 */

u64 sp_get_sum (uint start, uint stop, cs_t *root_css_buf)
{
  u64 sum = 1;

  uint i;

  for (i = start; i < stop; i++)
  {
    sum *= root_css_buf[i].cs_len;
  }

  return (sum);
}

void sp_exec (u64 ctx, char *pw_buf, cs_t *root_css_buf, cs_t *markov_css_buf, uint start, uint stop)
{
  u64 v = ctx;

  cs_t *cs = &root_css_buf[start];

  uint i;

  for (i = start; i < stop; i++)
  {
    const u64 m = v % cs->cs_len;
    const u64 d = v / cs->cs_len;

    v = d;

    const uint k = cs->cs_buf[m];

    pw_buf[i - start] = (char) k;

    cs = &markov_css_buf[(i * CHARSIZ) + k];
  }
}

int sp_comp_val (const void *p1, const void *p2)
{
  hcstat_table_t *b1 = (hcstat_table_t *) p1;
  hcstat_table_t *b2 = (hcstat_table_t *) p2;

  return b2->val - b1->val;
}

void sp_setup_tbl (const char *shared_dir, char *hcstat, uint disable, uint classic, hcstat_table_t *root_table_buf, hcstat_table_t *markov_table_buf)
{
  uint i;
  uint j;
  uint k;

  /**
   * Initialize hcstats
   */

  u64 *root_stats_buf = (u64 *) mycalloc (SP_ROOT_CNT, sizeof (u64));

  u64 *root_stats_ptr = root_stats_buf;

  u64 *root_stats_buf_by_pos[SP_PW_MAX];

  for (i = 0; i < SP_PW_MAX; i++)
  {
    root_stats_buf_by_pos[i] = root_stats_ptr;

    root_stats_ptr += CHARSIZ;
  }

  u64 *markov_stats_buf = (u64 *) mycalloc (SP_MARKOV_CNT, sizeof (u64));

  u64 *markov_stats_ptr = markov_stats_buf;

  u64 *markov_stats_buf_by_key[SP_PW_MAX][CHARSIZ];

  for (i = 0; i < SP_PW_MAX; i++)
  {
    for (j = 0; j < CHARSIZ; j++)
    {
      markov_stats_buf_by_key[i][j] = markov_stats_ptr;

      markov_stats_ptr += CHARSIZ;
    }
  }

  /**
   * Load hcstats File
   */

  if (hcstat == NULL)
  {
    char hcstat_tmp[256] = { 0 };

    snprintf (hcstat_tmp, sizeof (hcstat_tmp) - 1, "%s/%s", shared_dir, SP_HCSTAT);

    hcstat = hcstat_tmp;
  }

  FILE *fd = fopen (hcstat, "rb");

  if (fd == NULL)
  {
    log_error ("%s: %s", hcstat, strerror (errno));

    exit (-1);
  }

  if (fread (root_stats_buf, sizeof (u64), SP_ROOT_CNT, fd) != SP_ROOT_CNT)
  {
    log_error ("%s: Could not load data", hcstat);

    fclose (fd);

    exit (-1);
  }

  if (fread (markov_stats_buf, sizeof (u64), SP_MARKOV_CNT, fd) != SP_MARKOV_CNT)
  {
    log_error ("%s: Could not load data", hcstat);

    fclose (fd);

    exit (-1);
  }

  fclose (fd);

  /**
   * Markov modifier of hcstat_table on user request
   */

  if (disable)
  {
    memset (root_stats_buf,   0, SP_ROOT_CNT   * sizeof (u64));
    memset (markov_stats_buf, 0, SP_MARKOV_CNT * sizeof (u64));
  }

  if (classic)
  {
    /* Add all stats to first position */

    for (i = 1; i < SP_PW_MAX; i++)
    {
      u64 *out = root_stats_buf_by_pos[0];
      u64 *in  = root_stats_buf_by_pos[i];

      for (j = 0; j < CHARSIZ; j++)
      {
        *out++ += *in++;
      }
    }

    for (i = 1; i < SP_PW_MAX; i++)
    {
      u64 *out = markov_stats_buf_by_key[0][0];
      u64 *in  = markov_stats_buf_by_key[i][0];

      for (j = 0; j < CHARSIZ; j++)
      {
        for (k = 0; k < CHARSIZ; k++)
        {
          *out++ += *in++;
        }
      }
    }

    /* copy them to all pw_positions */

    for (i = 1; i < SP_PW_MAX; i++)
    {
      memcpy (root_stats_buf_by_pos[i], root_stats_buf_by_pos[0], CHARSIZ * sizeof (u64));
    }

    for (i = 1; i < SP_PW_MAX; i++)
    {
      memcpy (markov_stats_buf_by_key[i][0], markov_stats_buf_by_key[0][0], CHARSIZ * CHARSIZ * sizeof (u64));
    }
  }

  /**
   * Initialize tables
   */

  hcstat_table_t *root_table_ptr = root_table_buf;

  hcstat_table_t *root_table_buf_by_pos[SP_PW_MAX];

  for (i = 0; i < SP_PW_MAX; i++)
  {
    root_table_buf_by_pos[i] = root_table_ptr;

    root_table_ptr += CHARSIZ;
  }

  hcstat_table_t *markov_table_ptr = markov_table_buf;

  hcstat_table_t *markov_table_buf_by_key[SP_PW_MAX][CHARSIZ];

  for (i = 0; i < SP_PW_MAX; i++)
  {
    for (j = 0; j < CHARSIZ; j++)
    {
      markov_table_buf_by_key[i][j] = markov_table_ptr;

      markov_table_ptr += CHARSIZ;
    }
  }

  /**
   * Convert hcstat to tables
   */

  for (i = 0; i < SP_ROOT_CNT; i++)
  {
    uint key = i % CHARSIZ;

    root_table_buf[i].key = key;
    root_table_buf[i].val = root_stats_buf[i];
  }

  for (i = 0; i < SP_MARKOV_CNT; i++)
  {
    uint key = i % CHARSIZ;

    markov_table_buf[i].key = key;
    markov_table_buf[i].val = markov_stats_buf[i];
  }

  myfree (root_stats_buf);
  myfree (markov_stats_buf);

  /**
   * Finally sort them
   */

  for (i = 0; i < SP_PW_MAX; i++)
  {
    qsort (root_table_buf_by_pos[i], CHARSIZ, sizeof (hcstat_table_t), sp_comp_val);
  }

  for (i = 0; i < SP_PW_MAX; i++)
  {
    for (j = 0; j < CHARSIZ; j++)
    {
      qsort (markov_table_buf_by_key[i][j], CHARSIZ, sizeof (hcstat_table_t), sp_comp_val);
    }
  }
}

void sp_tbl_to_css (hcstat_table_t *root_table_buf, hcstat_table_t *markov_table_buf, cs_t *root_css_buf, cs_t *markov_css_buf, uint threshold, uint uniq_tbls[SP_PW_MAX][CHARSIZ])
{
  /**
   * Convert tables to css
   */

  for (uint i = 0; i < SP_ROOT_CNT; i++)
  {
    uint pw_pos = i / CHARSIZ;

    cs_t *cs = &root_css_buf[pw_pos];

    if (cs->cs_len == threshold) continue;

    uint key = root_table_buf[i].key;

    if (uniq_tbls[pw_pos][key] == 0) continue;

    cs->cs_buf[cs->cs_len] = key;

    cs->cs_len++;
  }

  /**
   * Convert table to css
   */

  for (uint i = 0; i < SP_MARKOV_CNT; i++)
  {
    uint c = i / CHARSIZ;

    cs_t *cs = &markov_css_buf[c];

    if (cs->cs_len == threshold) continue;

    uint pw_pos = c / CHARSIZ;

    uint key = markov_table_buf[i].key;

    if ((pw_pos + 1) < SP_PW_MAX) if (uniq_tbls[pw_pos + 1][key] == 0) continue;

    cs->cs_buf[cs->cs_len] = key;

    cs->cs_len++;
  }

  /*
  for (uint i = 0; i < 8; i++)
  {
    for (uint j = 0x20; j < 0x80; j++)
    {
      cs_t *ptr = &markov_css_buf[(i * CHARSIZ) + j];

      printf ("pos:%u key:%u len:%u\n", i, j, ptr->cs_len);

      for (uint k = 0; k < 10; k++)
      {
        printf ("  %u\n",  ptr->cs_buf[k]);
      }
    }
  }
  */
}

void sp_stretch_root (hcstat_table_t *in, hcstat_table_t *out)
{
  for (uint i = 0; i < SP_PW_MAX; i += 2)
  {
    memcpy (out, in, CHARSIZ * sizeof (hcstat_table_t));

    out += CHARSIZ;
    in  += CHARSIZ;

    out->key = 0;
    out->val = 1;

    out++;

    for (uint j = 1; j < CHARSIZ; j++)
    {
      out->key = j;
      out->val = 0;

      out++;
    }
  }
}

void sp_stretch_markov (hcstat_table_t *in, hcstat_table_t *out)
{
  for (uint i = 0; i < SP_PW_MAX; i += 2)
  {
    memcpy (out, in, CHARSIZ * CHARSIZ * sizeof (hcstat_table_t));

    out += CHARSIZ * CHARSIZ;
    in  += CHARSIZ * CHARSIZ;

    for (uint j = 0; j < CHARSIZ; j++)
    {
      out->key = 0;
      out->val = 1;

      out++;

      for (uint k = 1; k < CHARSIZ; k++)
      {
        out->key = k;
        out->val = 0;

        out++;
      }
    }
  }
}

/**
 * mixed shared functions
 */

void dump_hex (const u8 *s, const int sz)
{
  for (int i = 0; i < sz; i++)
  {
    log_info_nn ("%02x ", s[i]);
  }

  log_info ("");
}

void usage_mini_print (const char *progname)
{
  for (uint i = 0; USAGE_MINI[i] != NULL; i++) log_info (USAGE_MINI[i], progname);
}

void usage_big_print (const char *progname)
{
  for (uint i = 0; USAGE_BIG[i] != NULL; i++) log_info (USAGE_BIG[i], progname);
}

char *get_exec_path ()
{
  int exec_path_len = 1024;

  char *exec_path = (char *) mymalloc (exec_path_len);

  #ifdef __linux__

  char tmp[32] = { 0 };

  snprintf (tmp, sizeof (tmp) - 1, "/proc/%d/exe", getpid ());

  const int len = readlink (tmp, exec_path, exec_path_len - 1);

  #elif WIN

  const int len = GetModuleFileName (NULL, exec_path, exec_path_len - 1);

  #elif __APPLE__

  uint size = exec_path_len;

  if (_NSGetExecutablePath (exec_path, &size) != 0)
  {
    log_error("! executable path buffer too small\n");

    exit (-1);
  }

  const int len = strlen (exec_path);

  #elif __FreeBSD__

  int mib[4];
  mib[0] = CTL_KERN;
  mib[1] = KERN_PROC;
  mib[2] = KERN_PROC_PATHNAME;
  mib[3] = -1;

  char tmp[32] = { 0 };

  size_t size = exec_path_len;
  sysctl(mib, 4, exec_path, &size, NULL, 0);

  const int len = readlink (tmp, exec_path, exec_path_len - 1);

  #else
  #error Your Operating System is not supported or detected
  #endif

  exec_path[len] = 0;

  return exec_path;
}

char *get_install_dir (const char *progname)
{
  char *install_dir = mystrdup (progname);
  char *last_slash  = NULL;

  if ((last_slash = strrchr (install_dir, '/')) != NULL)
  {
    *last_slash = 0;
  }
  else if ((last_slash = strrchr (install_dir, '\\')) != NULL)
  {
    *last_slash = 0;
  }
  else
  {
    install_dir[0] = '.';
    install_dir[1] = 0;
  }

  return (install_dir);
}

char *get_profile_dir (const char *homedir)
{
  #define DOT_HASHCAT ".hashcat"

  size_t len = strlen (homedir) + 1 + strlen (DOT_HASHCAT) + 1;

  char *profile_dir = (char *) mymalloc (len + 1);

  snprintf (profile_dir, len, "%s/%s", homedir, DOT_HASHCAT);

  return profile_dir;
}

char *get_session_dir (const char *profile_dir)
{
  #define SESSIONS_FOLDER "sessions"

  size_t len = strlen (profile_dir) + 1 + strlen (SESSIONS_FOLDER) + 1;

  char *session_dir = (char *) mymalloc (len + 1);

  snprintf (session_dir, len, "%s/%s", profile_dir, SESSIONS_FOLDER);

  return session_dir;
}

uint count_lines (FILE *fd)
{
  uint cnt = 0;

  char *buf = (char *) mymalloc (HCBUFSIZ + 1);

  char prev = '\n';

  while (!feof (fd))
  {
    size_t nread = fread (buf, sizeof (char), HCBUFSIZ, fd);

    if (nread < 1) continue;

    size_t i;

    for (i = 0; i < nread; i++)
    {
      if (prev == '\n') cnt++;

      prev = buf[i];
    }
  }

  myfree (buf);

  return cnt;
}

#ifdef __APPLE__
int pthread_setaffinity_np (pthread_t thread, size_t cpu_size, cpu_set_t *cpu_set)
{
  int core;

  for (core = 0; core < (8 * (int)cpu_size); core++)
    if (CPU_ISSET(core, cpu_set)) break;

  thread_affinity_policy_data_t policy = { core };

  const int rc = thread_policy_set (pthread_mach_thread_np (thread), THREAD_AFFINITY_POLICY, (thread_policy_t) &policy, 1);

  if (data.quiet == 0)
  {
    if (rc != KERN_SUCCESS)
    {
      log_error ("ERROR: %s : %d", "thread_policy_set()", rc);
    }
  }

  return rc;
}
#endif

void set_cpu_affinity (char *cpu_affinity)
{
  #if   defined(_WIN)
  DWORD_PTR aff_mask = 0;
  #elif defined(__FreeBSD__)
  cpuset_t cpuset;
  CPU_ZERO (&cpuset);
  #elif defined(_POSIX)
  cpu_set_t cpuset;
  CPU_ZERO (&cpuset);
  #endif

  if (cpu_affinity)
  {
    char *devices = mystrdup (cpu_affinity);

    char *next = strtok (devices, ",");

    do
    {
      uint cpu_id = atoi (next);

      if (cpu_id == 0)
      {
        #ifdef _WIN
        aff_mask = 0;
        #elif _POSIX
        CPU_ZERO (&cpuset);
        #endif

        break;
      }

      if (cpu_id > 32)
      {
        log_error ("ERROR: Invalid cpu_id %u specified", cpu_id);

        exit (-1);
      }

      #ifdef _WIN
      aff_mask |= 1u << (cpu_id - 1);
      #elif _POSIX
      CPU_SET ((cpu_id - 1), &cpuset);
      #endif

    } while ((next = strtok (NULL, ",")) != NULL);

    myfree (devices);
  }

  #if   defined( _WIN)
  SetProcessAffinityMask (GetCurrentProcess (), aff_mask);
  SetThreadAffinityMask (GetCurrentThread (), aff_mask);
  #elif defined(__FreeBSD__)
  pthread_t thread = pthread_self ();
  pthread_setaffinity_np (thread, sizeof (cpuset_t), &cpuset);
  #elif defined(_POSIX)
  pthread_t thread = pthread_self ();
  pthread_setaffinity_np (thread, sizeof (cpu_set_t), &cpuset);
  #endif
}

void *rulefind (const void *key, void *base, int nmemb, size_t size, int (*compar) (const void *, const void *))
{
  char *element, *end;

  end = (char *) base + nmemb * size;

  for (element = (char *) base; element < end; element += size)
    if (!compar (element, key))
      return element;

  return NULL;
}

int sort_by_u32 (const void *v1, const void *v2)
{
  const u32 *s1 = (const u32 *) v1;
  const u32 *s2 = (const u32 *) v2;

  return *s1 - *s2;
}

int sort_by_salt (const void *v1, const void *v2)
{
  const salt_t *s1 = (const salt_t *) v1;
  const salt_t *s2 = (const salt_t *) v2;

  const int res1 = s1->salt_len - s2->salt_len;

  if (res1 != 0) return (res1);

  const int res2 = s1->salt_iter - s2->salt_iter;

  if (res2 != 0) return (res2);

  uint n;

  n = 16;

  while (n--)
  {
    if (s1->salt_buf[n] > s2->salt_buf[n]) return ( 1);
    if (s1->salt_buf[n] < s2->salt_buf[n]) return -1;
  }

  n = 8;

  while (n--)
  {
    if (s1->salt_buf_pc[n] > s2->salt_buf_pc[n]) return ( 1);
    if (s1->salt_buf_pc[n] < s2->salt_buf_pc[n]) return -1;
  }

  return 0;
}

int sort_by_salt_buf (const void *v1, const void *v2)
{
  const pot_t *p1 = (const pot_t *) v1;
  const pot_t *p2 = (const pot_t *) v2;

  const hash_t *h1 = &p1->hash;
  const hash_t *h2 = &p2->hash;

  const salt_t *s1 = h1->salt;
  const salt_t *s2 = h2->salt;

  uint n = 16;

  while (n--)
  {
    if (s1->salt_buf[n] > s2->salt_buf[n]) return ( 1);
    if (s1->salt_buf[n] < s2->salt_buf[n]) return -1;
  }

  return 0;
}

int sort_by_hash_t_salt (const void *v1, const void *v2)
{
  const hash_t *h1 = (const hash_t *) v1;
  const hash_t *h2 = (const hash_t *) v2;

  const salt_t *s1 = h1->salt;
  const salt_t *s2 = h2->salt;

  // testphase: this should work
  uint n = 16;

  while (n--)
  {
    if (s1->salt_buf[n] > s2->salt_buf[n]) return ( 1);
    if (s1->salt_buf[n] < s2->salt_buf[n]) return -1;
  }

  /* original code, seems buggy since salt_len can be very big (had a case with 131 len)
     also it thinks salt_buf[x] is a char but its a uint so salt_len should be / 4
  if (s1->salt_len > s2->salt_len) return ( 1);
  if (s1->salt_len < s2->salt_len) return -1;

  uint n = s1->salt_len;

  while (n--)
  {
    if (s1->salt_buf[n] > s2->salt_buf[n]) return ( 1);
    if (s1->salt_buf[n] < s2->salt_buf[n]) return -1;
  }
  */

  return 0;
}

int sort_by_hash_t_salt_hccap (const void *v1, const void *v2)
{
  const hash_t *h1 = (const hash_t *) v1;
  const hash_t *h2 = (const hash_t *) v2;

  const salt_t *s1 = h1->salt;
  const salt_t *s2 = h2->salt;

  // last 2: salt_buf[10] and salt_buf[11] contain the digest (skip them)

  uint n = 9; // 9 * 4 = 36 bytes (max length of ESSID)

  while (n--)
  {
    if (s1->salt_buf[n] > s2->salt_buf[n]) return ( 1);
    if (s1->salt_buf[n] < s2->salt_buf[n]) return -1;
  }

  return 0;
}

int sort_by_hash_no_salt (const void *v1, const void *v2)
{
  const hash_t *h1 = (const hash_t *) v1;
  const hash_t *h2 = (const hash_t *) v2;

  const void *d1 = h1->digest;
  const void *d2 = h2->digest;

  return data.sort_by_digest (d1, d2);
}

int sort_by_hash (const void *v1, const void *v2)
{
  const hash_t *h1 = (const hash_t *) v1;
  const hash_t *h2 = (const hash_t *) v2;

  if (data.isSalted)
  {
    const salt_t *s1 = h1->salt;
    const salt_t *s2 = h2->salt;

    int res = sort_by_salt (s1, s2);

    if (res != 0) return (res);
  }

  const void *d1 = h1->digest;
  const void *d2 = h2->digest;

  return data.sort_by_digest (d1, d2);
}

int sort_by_pot (const void *v1, const void *v2)
{
  const pot_t *p1 = (const pot_t *) v1;
  const pot_t *p2 = (const pot_t *) v2;

  const hash_t *h1 = &p1->hash;
  const hash_t *h2 = &p2->hash;

  return sort_by_hash (h1, h2);
}

int sort_by_mtime (const void *p1, const void *p2)
{
  const char **f1 = (const char **) p1;
  const char **f2 = (const char **) p2;

  struct stat s1; stat (*f1, &s1);
  struct stat s2; stat (*f2, &s2);

  return s2.st_mtime - s1.st_mtime;
}

int sort_by_cpu_rule (const void *p1, const void *p2)
{
  const cpu_rule_t *r1 = (const cpu_rule_t *) p1;
  const cpu_rule_t *r2 = (const cpu_rule_t *) p2;

  return memcmp (r1, r2, sizeof (cpu_rule_t));
}

int sort_by_kernel_rule (const void *p1, const void *p2)
{
  const kernel_rule_t *r1 = (const kernel_rule_t *) p1;
  const kernel_rule_t *r2 = (const kernel_rule_t *) p2;

  return memcmp (r1, r2, sizeof (kernel_rule_t));
}

int sort_by_stringptr (const void *p1, const void *p2)
{
  const char **s1 = (const char **) p1;
  const char **s2 = (const char **) p2;

  return strcmp (*s1, *s2);
}

int sort_by_dictstat (const void *s1, const void *s2)
{
  dictstat_t *d1 = (dictstat_t *) s1;
  dictstat_t *d2 = (dictstat_t *) s2;

  #ifdef __linux__
  d2->stat.st_atim = d1->stat.st_atim;
  #else
  d2->stat.st_atime = d1->stat.st_atime;
  #endif

  return memcmp (&d1->stat, &d2->stat, sizeof (struct stat));
}

int sort_by_bitmap (const void *p1, const void *p2)
{
  const bitmap_result_t *b1 = (const bitmap_result_t *) p1;
  const bitmap_result_t *b2 = (const bitmap_result_t *) p2;

  return b1->collisions - b2->collisions;
}

int sort_by_digest_4_2 (const void *v1, const void *v2)
{
  const u32 *d1 = (const u32 *) v1;
  const u32 *d2 = (const u32 *) v2;

  uint n = 2;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_4_4 (const void *v1, const void *v2)
{
  const u32 *d1 = (const u32 *) v1;
  const u32 *d2 = (const u32 *) v2;

  uint n = 4;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_4_5 (const void *v1, const void *v2)
{
  const u32 *d1 = (const u32 *) v1;
  const u32 *d2 = (const u32 *) v2;

  uint n = 5;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_4_6 (const void *v1, const void *v2)
{
  const u32 *d1 = (const u32 *) v1;
  const u32 *d2 = (const u32 *) v2;

  uint n = 6;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_4_8 (const void *v1, const void *v2)
{
  const u32 *d1 = (const u32 *) v1;
  const u32 *d2 = (const u32 *) v2;

  uint n = 8;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_4_16 (const void *v1, const void *v2)
{
  const u32 *d1 = (const u32 *) v1;
  const u32 *d2 = (const u32 *) v2;

  uint n = 16;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_4_32 (const void *v1, const void *v2)
{
  const u32 *d1 = (const u32 *) v1;
  const u32 *d2 = (const u32 *) v2;

  uint n = 32;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_4_64 (const void *v1, const void *v2)
{
  const u32 *d1 = (const u32 *) v1;
  const u32 *d2 = (const u32 *) v2;

  uint n = 64;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_8_8 (const void *v1, const void *v2)
{
  const u64 *d1 = (const u64 *) v1;
  const u64 *d2 = (const u64 *) v2;

  uint n = 8;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_8_16 (const void *v1, const void *v2)
{
  const u64 *d1 = (const u64 *) v1;
  const u64 *d2 = (const u64 *) v2;

  uint n = 16;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_8_25 (const void *v1, const void *v2)
{
  const u64 *d1 = (const u64 *) v1;
  const u64 *d2 = (const u64 *) v2;

  uint n = 25;

  while (n--)
  {
    if (d1[n] > d2[n]) return ( 1);
    if (d1[n] < d2[n]) return -1;
  }

  return 0;
}

int sort_by_digest_p0p1 (const void *v1, const void *v2)
{
  const u32 *d1 = (const u32 *) v1;
  const u32 *d2 = (const u32 *) v2;

  const uint dgst_pos0 = data.dgst_pos0;
  const uint dgst_pos1 = data.dgst_pos1;
  const uint dgst_pos2 = data.dgst_pos2;
  const uint dgst_pos3 = data.dgst_pos3;

  if (d1[dgst_pos3] > d2[dgst_pos3]) return ( 1);
  if (d1[dgst_pos3] < d2[dgst_pos3]) return -1;
  if (d1[dgst_pos2] > d2[dgst_pos2]) return ( 1);
  if (d1[dgst_pos2] < d2[dgst_pos2]) return -1;
  if (d1[dgst_pos1] > d2[dgst_pos1]) return ( 1);
  if (d1[dgst_pos1] < d2[dgst_pos1]) return -1;
  if (d1[dgst_pos0] > d2[dgst_pos0]) return ( 1);
  if (d1[dgst_pos0] < d2[dgst_pos0]) return -1;

  return 0;
}

static int sort_by_tuning_db_alias (const void *v1, const void *v2)
{
  const tuning_db_alias_t *t1 = (const tuning_db_alias_t *) v1;
  const tuning_db_alias_t *t2 = (const tuning_db_alias_t *) v2;

  const int res1 = strcmp (t1->device_name, t2->device_name);

  if (res1 != 0) return (res1);

  return 0;
}

static int sort_by_tuning_db_entry (const void *v1, const void *v2)
{
  const tuning_db_entry_t *t1 = (const tuning_db_entry_t *) v1;
  const tuning_db_entry_t *t2 = (const tuning_db_entry_t *) v2;

  const int res1 = strcmp (t1->device_name, t2->device_name);

  if (res1 != 0) return (res1);

  const int res2 = t1->attack_mode
                 - t2->attack_mode;

  if (res2 != 0) return (res2);

  const int res3 = t1->hash_type
                 - t2->hash_type;

  if (res3 != 0) return (res3);

  return 0;
}

void format_debug (char *debug_file, uint debug_mode, unsigned char *orig_plain_ptr, uint orig_plain_len, unsigned char *mod_plain_ptr, uint mod_plain_len, char *rule_buf, int rule_len)
{
  uint outfile_autohex = data.outfile_autohex;

  unsigned char *rule_ptr = (unsigned char *) rule_buf;

  FILE *debug_fp = NULL;

  if (debug_file != NULL)
  {
    debug_fp = fopen (debug_file, "ab");

    lock_file (debug_fp);
  }
  else
  {
    debug_fp = stderr;
  }

  if (debug_fp == NULL)
  {
    log_info ("WARNING: Could not open debug-file for writing");
  }
  else
  {
    if ((debug_mode == 2) || (debug_mode == 3) || (debug_mode == 4))
    {
      format_plain (debug_fp, orig_plain_ptr, orig_plain_len, outfile_autohex);

      if ((debug_mode == 3) || (debug_mode == 4)) fputc (':', debug_fp);
    }

    fwrite (rule_ptr, rule_len, 1, debug_fp);

    if (debug_mode == 4)
    {
      fputc (':', debug_fp);

      format_plain (debug_fp, mod_plain_ptr, mod_plain_len, outfile_autohex);
    }

    fputc  ('\n', debug_fp);

    if (debug_file != NULL) fclose (debug_fp);
  }
}

void format_plain (FILE *fp, unsigned char *plain_ptr, uint plain_len, uint outfile_autohex)
{
  int needs_hexify = 0;

  if (outfile_autohex == 1)
  {
    for (uint i = 0; i < plain_len; i++)
    {
      if (plain_ptr[i] < 0x20)
      {
        needs_hexify = 1;

        break;
      }

      if (plain_ptr[i] > 0x7f)
      {
        needs_hexify = 1;

        break;
      }
    }
  }

  if (needs_hexify == 1)
  {
    fprintf (fp, "$HEX[");

    for (uint i = 0; i < plain_len; i++)
    {
      fprintf (fp, "%02x", plain_ptr[i]);
    }

    fprintf (fp, "]");
  }
  else
  {
    fwrite (plain_ptr, plain_len, 1, fp);
  }
}

void format_output (FILE *out_fp, char *out_buf, unsigned char *plain_ptr, const uint plain_len, const u64 crackpos, unsigned char *username, const uint user_len)
{
  uint outfile_format = data.outfile_format;

  char separator = data.separator;

  if (outfile_format & OUTFILE_FMT_HASH)
  {
    fprintf (out_fp, "%s", out_buf);

    if (outfile_format & (OUTFILE_FMT_PLAIN | OUTFILE_FMT_HEXPLAIN | OUTFILE_FMT_CRACKPOS))
    {
      fputc (separator, out_fp);
    }
  }
  else if (data.username)
  {
    if (username != NULL)
    {
      for (uint i = 0; i < user_len; i++)
      {
        fprintf (out_fp, "%c", username[i]);
      }

      if (outfile_format & (OUTFILE_FMT_PLAIN | OUTFILE_FMT_HEXPLAIN | OUTFILE_FMT_CRACKPOS))
      {
        fputc (separator, out_fp);
      }
    }
  }

  if (outfile_format & OUTFILE_FMT_PLAIN)
  {
    format_plain (out_fp, plain_ptr, plain_len, data.outfile_autohex);

    if (outfile_format & (OUTFILE_FMT_HEXPLAIN | OUTFILE_FMT_CRACKPOS))
    {
      fputc (separator, out_fp);
    }
  }

  if (outfile_format & OUTFILE_FMT_HEXPLAIN)
  {
    for (uint i = 0; i < plain_len; i++)
    {
      fprintf (out_fp, "%02x", plain_ptr[i]);
    }

    if (outfile_format & (OUTFILE_FMT_CRACKPOS))
    {
      fputc (separator, out_fp);
    }
  }

  if (outfile_format & OUTFILE_FMT_CRACKPOS)
  {
    #ifdef _WIN
    __mingw_fprintf (out_fp, "%llu", crackpos);
    #endif

    #ifdef _POSIX
    #ifdef __x86_64__
    fprintf (out_fp, "%lu", (unsigned long) crackpos);
    #else
    fprintf (out_fp, "%llu", crackpos);
    #endif
    #endif
  }

  fputs (EOL, out_fp);
}

void handle_show_request (pot_t *pot, uint pot_cnt, char *input_buf, int input_len, hash_t *hashes_buf, int (*sort_by_pot) (const void *, const void *), FILE *out_fp)
{
  pot_t pot_key;

  pot_key.hash.salt   = hashes_buf->salt;
  pot_key.hash.digest = hashes_buf->digest;

  pot_t *pot_ptr = (pot_t *) bsearch (&pot_key, pot, pot_cnt, sizeof (pot_t), sort_by_pot);

  if (pot_ptr)
  {
    log_info_nn ("");

    input_buf[input_len] = 0;

    // user
    unsigned char *username = NULL;
    uint user_len = 0;

    if (data.username)
    {
      user_t *user = hashes_buf->hash_info->user;

      if (user)
      {
        username = (unsigned char *) (user->user_name);

        user_len = user->user_len;
      }
    }

    // do output the line
    format_output (out_fp, input_buf, (unsigned char *) pot_ptr->plain_buf, pot_ptr->plain_len, 0, username, user_len);
  }
}

#define LM_WEAK_HASH    "\x4e\xcf\x0d\x0c\x0a\xe2\xfb\xc1"
#define LM_MASKED_PLAIN "[notfound]"

void handle_show_request_lm (pot_t *pot, uint pot_cnt, char *input_buf, int input_len, hash_t *hash_left, hash_t *hash_right, int (*sort_by_pot) (const void *, const void *), FILE *out_fp)
{
  // left

  pot_t pot_left_key;

  pot_left_key.hash.salt   = hash_left->salt;
  pot_left_key.hash.digest = hash_left->digest;

  pot_t *pot_left_ptr = (pot_t *) bsearch (&pot_left_key, pot, pot_cnt, sizeof (pot_t), sort_by_pot);

  // right

  uint weak_hash_found = 0;

  pot_t pot_right_key;

  pot_right_key.hash.salt   = hash_right->salt;
  pot_right_key.hash.digest = hash_right->digest;

  pot_t *pot_right_ptr = (pot_t *) bsearch (&pot_right_key, pot, pot_cnt, sizeof (pot_t), sort_by_pot);

  if (pot_right_ptr == NULL)
  {
    // special case, if "weak hash"

    if (memcmp (hash_right->digest, LM_WEAK_HASH, 8) == 0)
    {
      weak_hash_found = 1;

      pot_right_ptr = (pot_t *) mycalloc (1, sizeof (pot_t));

      // in theory this is not needed, but we are paranoia:

      memset (pot_right_ptr->plain_buf, 0, sizeof (pot_right_ptr->plain_buf));
      pot_right_ptr->plain_len = 0;
    }
  }

  if ((pot_left_ptr == NULL) && (pot_right_ptr == NULL))
  {
    if (weak_hash_found == 1) myfree (pot_right_ptr); // this shouldn't happen at all: if weak_hash_found == 1, than pot_right_ptr is not NULL for sure

    return;
  }

  // at least one half was found:

  log_info_nn ("");

  input_buf[input_len] = 0;

  // user

  unsigned char *username = NULL;
  uint user_len = 0;

  if (data.username)
  {
    user_t *user = hash_left->hash_info->user;

    if (user)
    {
      username = (unsigned char *) (user->user_name);

      user_len = user->user_len;
    }
  }

  // mask the part which was not found

  uint left_part_masked  = 0;
  uint right_part_masked = 0;

  uint mask_plain_len = strlen (LM_MASKED_PLAIN);

  if (pot_left_ptr == NULL)
  {
    left_part_masked = 1;

    pot_left_ptr = (pot_t *) mycalloc (1, sizeof (pot_t));

    memset (pot_left_ptr->plain_buf, 0, sizeof (pot_left_ptr->plain_buf));

    memcpy (pot_left_ptr->plain_buf, LM_MASKED_PLAIN, mask_plain_len);
    pot_left_ptr->plain_len = mask_plain_len;
  }

  if (pot_right_ptr == NULL)
  {
    right_part_masked = 1;

    pot_right_ptr = (pot_t *) mycalloc (1, sizeof (pot_t));

    memset (pot_right_ptr->plain_buf, 0, sizeof (pot_right_ptr->plain_buf));

    memcpy (pot_right_ptr->plain_buf, LM_MASKED_PLAIN, mask_plain_len);
    pot_right_ptr->plain_len = mask_plain_len;
  }

  // create the pot_ptr out of pot_left_ptr and pot_right_ptr

  pot_t pot_ptr;

  pot_ptr.plain_len = pot_left_ptr->plain_len + pot_right_ptr->plain_len;

  memcpy (pot_ptr.plain_buf, pot_left_ptr->plain_buf, pot_left_ptr->plain_len);

  memcpy (pot_ptr.plain_buf + pot_left_ptr->plain_len, pot_right_ptr->plain_buf, pot_right_ptr->plain_len);

  // do output the line

  format_output (out_fp, input_buf, (unsigned char *) pot_ptr.plain_buf, pot_ptr.plain_len, 0, username, user_len);

  if (weak_hash_found == 1) myfree (pot_right_ptr);

  if (left_part_masked  == 1) myfree (pot_left_ptr);
  if (right_part_masked == 1) myfree (pot_right_ptr);
}

void handle_left_request (pot_t *pot, uint pot_cnt, char *input_buf, int input_len, hash_t *hashes_buf, int (*sort_by_pot) (const void *, const void *), FILE *out_fp)
{
  pot_t pot_key;

  memcpy (&pot_key.hash, hashes_buf, sizeof (hash_t));

  pot_t *pot_ptr = (pot_t *) bsearch (&pot_key, pot, pot_cnt, sizeof (pot_t), sort_by_pot);

  if (pot_ptr == NULL)
  {
    log_info_nn ("");

    input_buf[input_len] = 0;

    format_output (out_fp, input_buf, NULL, 0, 0, NULL, 0);
  }
}

void handle_left_request_lm (pot_t *pot, uint pot_cnt, char *input_buf, int input_len, hash_t *hash_left, hash_t *hash_right, int (*sort_by_pot) (const void *, const void *), FILE *out_fp)
{
  // left

  pot_t pot_left_key;

  memcpy (&pot_left_key.hash, hash_left, sizeof (hash_t));

  pot_t *pot_left_ptr = (pot_t *) bsearch (&pot_left_key, pot, pot_cnt, sizeof (pot_t), sort_by_pot);

  // right

  pot_t pot_right_key;

  memcpy (&pot_right_key.hash, hash_right, sizeof (hash_t));

  pot_t *pot_right_ptr = (pot_t *) bsearch (&pot_right_key, pot, pot_cnt, sizeof (pot_t), sort_by_pot);

  uint weak_hash_found = 0;

  if (pot_right_ptr == NULL)
  {
    // special case, if "weak hash"

    if (memcmp (hash_right->digest, LM_WEAK_HASH, 8) == 0)
    {
      weak_hash_found = 1;

      // we just need that pot_right_ptr is not a NULL pointer

      pot_right_ptr = (pot_t *) mycalloc (1, sizeof (pot_t));
    }
  }

  if ((pot_left_ptr != NULL) && (pot_right_ptr != NULL))
  {
    if (weak_hash_found == 1) myfree (pot_right_ptr);

    return;
  }

  // ... at least one part was not cracked

  log_info_nn ("");

  input_buf[input_len] = 0;

  // only show the hash part which is still not cracked

  uint user_len = (uint)input_len - 32u;

  char *hash_output = (char *) mymalloc (33);

  memcpy (hash_output, input_buf, input_len);

  if (pot_left_ptr != NULL)
  {
    // only show right part (because left part was already found)

    memcpy (hash_output + user_len, input_buf + user_len + 16, 16);

    hash_output[user_len + 16] = 0;
  }

  if (pot_right_ptr != NULL)
  {
    // only show left part (because right part was already found)

    memcpy (hash_output + user_len, input_buf + user_len, 16);

    hash_output[user_len + 16] = 0;
  }

  format_output (out_fp, hash_output, NULL, 0, 0, NULL, 0);

  myfree (hash_output);

  if (weak_hash_found == 1) myfree (pot_right_ptr);
}

uint setup_opencl_platforms_filter (char *opencl_platforms)
{
  uint opencl_platforms_filter = 0;

  if (opencl_platforms)
  {
    char *platforms = mystrdup (opencl_platforms);

    char *next = strtok (platforms, ",");

    do
    {
      int platform = atoi (next);

      if (platform < 1 || platform > 32)
      {
        log_error ("ERROR: Invalid OpenCL platform %u specified", platform);

        exit (-1);
      }

      opencl_platforms_filter |= 1u << (platform - 1);

    } while ((next = strtok (NULL, ",")) != NULL);

    myfree (platforms);
  }
  else
  {
    opencl_platforms_filter = -1u;
  }

  return opencl_platforms_filter;
}

u32 setup_devices_filter (char *opencl_devices)
{
  u32 devices_filter = 0;

  if (opencl_devices)
  {
    char *devices = mystrdup (opencl_devices);

    char *next = strtok (devices, ",");

    do
    {
      int device_id = atoi (next);

      if (device_id < 1 || device_id > 32)
      {
        log_error ("ERROR: Invalid device_id %u specified", device_id);

        exit (-1);
      }

      devices_filter |= 1u << (device_id - 1);

    } while ((next = strtok (NULL, ",")) != NULL);

    myfree (devices);
  }
  else
  {
    devices_filter = -1u;
  }

  return devices_filter;
}

cl_device_type setup_device_types_filter (char *opencl_device_types)
{
  cl_device_type device_types_filter = 0;

  if (opencl_device_types)
  {
    char *device_types = mystrdup (opencl_device_types);

    char *next = strtok (device_types, ",");

    do
    {
      int device_type = atoi (next);

      if (device_type < 1 || device_type > 3)
      {
        log_error ("ERROR: Invalid device_type %u specified", device_type);

        exit (-1);
      }

      device_types_filter |= 1u << device_type;

    } while ((next = strtok (NULL, ",")) != NULL);

    myfree (device_types);
  }
  else
  {
    // Do not use CPU by default, this often reduces GPU performance because
    // the CPU is too busy to handle GPU synchronization

    device_types_filter = CL_DEVICE_TYPE_ALL & ~CL_DEVICE_TYPE_CPU;
  }

  return device_types_filter;
}

u32 get_random_num (const u32 min, const u32 max)
{
  if (min == max) return (min);

  return ((rand () % (max - min)) + min);
}

u32 mydivc32 (const u32 dividend, const u32 divisor)
{
  u32 quotient = dividend / divisor;

  if (dividend % divisor) quotient++;

  return quotient;
}

u64 mydivc64 (const u64 dividend, const u64 divisor)
{
  u64 quotient = dividend / divisor;

  if (dividend % divisor) quotient++;

  return quotient;
}

void format_timer_display (struct tm *tm, char *buf, size_t len)
{
  const char *time_entities_s[] = { "year",  "day",  "hour",  "min",  "sec"  };
  const char *time_entities_m[] = { "years", "days", "hours", "mins", "secs" };

  if (tm->tm_year - 70)
  {
    char *time_entity1 = ((tm->tm_year - 70) == 1) ? (char *) time_entities_s[0] : (char *) time_entities_m[0];
    char *time_entity2 = ( tm->tm_yday       == 1) ? (char *) time_entities_s[1] : (char *) time_entities_m[1];

    snprintf (buf, len - 1, "%d %s, %d %s", tm->tm_year - 70, time_entity1, tm->tm_yday, time_entity2);
  }
  else if (tm->tm_yday)
  {
    char *time_entity1 = (tm->tm_yday == 1) ? (char *) time_entities_s[1] : (char *) time_entities_m[1];
    char *time_entity2 = (tm->tm_hour == 1) ? (char *) time_entities_s[2] : (char *) time_entities_m[2];

    snprintf (buf, len - 1, "%d %s, %d %s", tm->tm_yday, time_entity1, tm->tm_hour, time_entity2);
  }
  else if (tm->tm_hour)
  {
    char *time_entity1 = (tm->tm_hour == 1) ? (char *) time_entities_s[2] : (char *) time_entities_m[2];
    char *time_entity2 = (tm->tm_min  == 1) ? (char *) time_entities_s[3] : (char *) time_entities_m[3];

    snprintf (buf, len - 1, "%d %s, %d %s", tm->tm_hour, time_entity1, tm->tm_min, time_entity2);
  }
  else if (tm->tm_min)
  {
    char *time_entity1 = (tm->tm_min == 1) ? (char *) time_entities_s[3] : (char *) time_entities_m[3];
    char *time_entity2 = (tm->tm_sec == 1) ? (char *) time_entities_s[4] : (char *) time_entities_m[4];

    snprintf (buf, len - 1, "%d %s, %d %s", tm->tm_min, time_entity1, tm->tm_sec, time_entity2);
  }
  else
  {
    char *time_entity1 = (tm->tm_sec == 1) ? (char *) time_entities_s[4] : (char *) time_entities_m[4];

    snprintf (buf, len - 1, "%d %s", tm->tm_sec, time_entity1);
  }
}

void format_speed_display (double val, char *buf, size_t len)
{
  if (val <= 0)
  {
    buf[0] = '0';
    buf[1] = ' ';
    buf[2] = 0;

    return;
  }

  char units[7] = { ' ', 'k', 'M', 'G', 'T', 'P', 'E' };

  uint level = 0;

  while (val > 99999)
  {
    val /= 1000;

    level++;
  }

  /* generate output */

  if (level == 0)
  {
    snprintf (buf, len - 1, "%.0f ", val);
  }
  else
  {
    snprintf (buf, len - 1, "%.1f %c", val, units[level]);
  }
}

int fgetl (FILE *fp, char *line_buf)
{
  int line_len = 0;

  while (!feof (fp))
  {
    const int c = fgetc (fp);

    if (c == EOF) break;

    line_buf[line_len] = (char) c;

    line_len++;

    if (line_len == HCBUFSIZ) line_len--;

    if (c == '\n') break;
  }

  if (line_len == 0) return 0;

  if (line_buf[line_len - 1] == '\n')
  {
    line_len--;

    line_buf[line_len] = 0;
  }

  if (line_len == 0) return 0;

  if (line_buf[line_len - 1] == '\r')
  {
    line_len--;

    line_buf[line_len] = 0;
  }

  return (line_len);
}

int in_superchop (char *buf)
{
  int len = strlen (buf);

  while (len)
  {
    if (buf[len - 1] == '\n')
    {
      len--;

      continue;
    }

    if (buf[len - 1] == '\r')
    {
      len--;

      continue;
    }

    break;
  }

  buf[len] = 0;

  return len;
}

char **scan_directory (const char *path)
{
  char *tmp_path = mystrdup (path);

  size_t tmp_path_len = strlen (tmp_path);

  while (tmp_path[tmp_path_len - 1] == '/' || tmp_path[tmp_path_len - 1] == '\\')
  {
    tmp_path[tmp_path_len - 1] = 0;

    tmp_path_len = strlen (tmp_path);
  }

  char **files = NULL;

  int num_files = 0;

  DIR *d = NULL;

  if ((d = opendir (tmp_path)) != NULL)
  {
    #ifdef __APPLE__

    struct dirent e;

    for (;;)
    {
      memset (&e, 0, sizeof (e));

      struct dirent *de = NULL;

      if (readdir_r (d, &e, &de) != 0)
      {
        log_error ("ERROR: readdir_r() failed");

        break;
      }

      if (de == NULL) break;

    #else

    struct dirent *de;

    while ((de = readdir (d)) != NULL)
    {

    #endif

      if ((strcmp (de->d_name, ".") == 0) || (strcmp (de->d_name, "..") == 0)) continue;

      int path_size = strlen (tmp_path) + 1 + strlen (de->d_name);

      char *path_file = (char *) mymalloc (path_size + 1);

      snprintf (path_file, path_size + 1, "%s/%s", tmp_path, de->d_name);

      path_file[path_size] = 0;

      DIR *d_test;

      if ((d_test = opendir (path_file)) != NULL)
      {
        closedir (d_test);

        myfree (path_file);
      }
      else
      {
        files = (char **) myrealloc (files, num_files * sizeof (char *), sizeof (char *));

        num_files++;

        files[num_files - 1] = path_file;
      }
    }

    closedir (d);
  }
  else if (errno == ENOTDIR)
  {
    files = (char **) myrealloc (files, num_files * sizeof (char *), sizeof (char *));

    num_files++;

    files[num_files - 1] = mystrdup (path);
  }

  files = (char **) myrealloc (files, num_files * sizeof (char *), sizeof (char *));

  num_files++;

  files[num_files - 1] = NULL;

  myfree (tmp_path);

  return (files);
}

int count_dictionaries (char **dictionary_files)
{
  if (dictionary_files == NULL) return 0;

  int cnt = 0;

  for (int d = 0; dictionary_files[d] != NULL; d++)
  {
    cnt++;
  }

  return (cnt);
}

char *stroptitype (const uint opti_type)
{
  switch (opti_type)
  {
    case OPTI_TYPE_ZERO_BYTE:         return ((char *) OPTI_STR_ZERO_BYTE);
    case OPTI_TYPE_PRECOMPUTE_INIT:   return ((char *) OPTI_STR_PRECOMPUTE_INIT);
    case OPTI_TYPE_PRECOMPUTE_MERKLE: return ((char *) OPTI_STR_PRECOMPUTE_MERKLE);
    case OPTI_TYPE_PRECOMPUTE_PERMUT: return ((char *) OPTI_STR_PRECOMPUTE_PERMUT);
    case OPTI_TYPE_MEET_IN_MIDDLE:    return ((char *) OPTI_STR_MEET_IN_MIDDLE);
    case OPTI_TYPE_EARLY_SKIP:        return ((char *) OPTI_STR_EARLY_SKIP);
    case OPTI_TYPE_NOT_SALTED:        return ((char *) OPTI_STR_NOT_SALTED);
    case OPTI_TYPE_NOT_ITERATED:      return ((char *) OPTI_STR_NOT_ITERATED);
    case OPTI_TYPE_PREPENDED_SALT:    return ((char *) OPTI_STR_PREPENDED_SALT);
    case OPTI_TYPE_APPENDED_SALT:     return ((char *) OPTI_STR_APPENDED_SALT);
    case OPTI_TYPE_SINGLE_HASH:       return ((char *) OPTI_STR_SINGLE_HASH);
    case OPTI_TYPE_SINGLE_SALT:       return ((char *) OPTI_STR_SINGLE_SALT);
    case OPTI_TYPE_BRUTE_FORCE:       return ((char *) OPTI_STR_BRUTE_FORCE);
    case OPTI_TYPE_RAW_HASH:          return ((char *) OPTI_STR_RAW_HASH);
    case OPTI_TYPE_SLOW_HASH_SIMD:    return ((char *) OPTI_STR_SLOW_HASH_SIMD);
    case OPTI_TYPE_USES_BITS_8:       return ((char *) OPTI_STR_USES_BITS_8);
    case OPTI_TYPE_USES_BITS_16:      return ((char *) OPTI_STR_USES_BITS_16);
    case OPTI_TYPE_USES_BITS_32:      return ((char *) OPTI_STR_USES_BITS_32);
    case OPTI_TYPE_USES_BITS_64:      return ((char *) OPTI_STR_USES_BITS_64);
  }

  return (NULL);
}

char *strstatus (const uint devices_status)
{
  switch (devices_status)
  {
    case  STATUS_INIT:               return ((char *) ST_0000);
    case  STATUS_STARTING:           return ((char *) ST_0001);
    case  STATUS_RUNNING:            return ((char *) ST_0002);
    case  STATUS_PAUSED:             return ((char *) ST_0003);
    case  STATUS_EXHAUSTED:          return ((char *) ST_0004);
    case  STATUS_CRACKED:            return ((char *) ST_0005);
    case  STATUS_ABORTED:            return ((char *) ST_0006);
    case  STATUS_QUIT:               return ((char *) ST_0007);
    case  STATUS_BYPASS:             return ((char *) ST_0008);
    case  STATUS_STOP_AT_CHECKPOINT: return ((char *) ST_0009);
    case  STATUS_AUTOTUNE:           return ((char *) ST_0010);
  }

  return ((char *) "Unknown");
}

static void SuspendThreads ()
{
  if (data.devices_status != STATUS_RUNNING) return;

  hc_timer_set (&data.timer_paused);

  data.devices_status = STATUS_PAUSED;

  log_info ("Paused");
}

static void ResumeThreads ()
{
  if (data.devices_status != STATUS_PAUSED) return;

  double ms_paused;

  hc_timer_get (data.timer_paused, ms_paused);

  data.ms_paused += ms_paused;

  data.devices_status = STATUS_RUNNING;

  log_info ("Resumed");
}

static void bypass ()
{
  data.devices_status = STATUS_BYPASS;

  log_info ("Next dictionary / mask in queue selected, bypassing current one");
}

static void stop_at_checkpoint ()
{
  if (data.devices_status != STATUS_STOP_AT_CHECKPOINT)
  {
    if (data.devices_status != STATUS_RUNNING) return;
  }

  // this feature only makes sense if --restore-disable was not specified

  if (data.restore_disable == 1)
  {
    log_info ("WARNING: This feature is disabled when --restore-disable is specified");

    return;
  }

  // check if monitoring of Restore Point updates should be enabled or disabled

  if (data.devices_status != STATUS_STOP_AT_CHECKPOINT)
  {
    data.devices_status = STATUS_STOP_AT_CHECKPOINT;

    // save the current restore point value

    data.checkpoint_cur_words = get_lowest_words_done ();

    log_info ("Checkpoint enabled: Will quit at next Restore Point update");
  }
  else
  {
    data.devices_status = STATUS_RUNNING;

    // reset the global value for checkpoint checks

    data.checkpoint_cur_words = 0;

    log_info ("Checkpoint disabled: Restore Point updates will no longer be monitored");
  }
}

void myabort ()
{
  data.devices_status = STATUS_ABORTED;
}

void myquit ()
{
  data.devices_status = STATUS_QUIT;
}

void naive_replace (char *s, const u8 key_char, const u8 replace_char)
{
  const size_t len = strlen (s);

  for (size_t in = 0; in < len; in++)
  {
    const u8 c = s[in];

    if (c == key_char)
    {
      s[in] = replace_char;
    }
  }
}

void naive_escape (char *s, size_t s_max, const u8 key_char, const u8 escape_char)
{
  char s_escaped[1024] = { 0 };

  size_t s_escaped_max = sizeof (s_escaped);

  const size_t len = strlen (s);

  for (size_t in = 0, out = 0; in < len; in++, out++)
  {
    const u8 c = s[in];

    if (c == key_char)
    {
      s_escaped[out] = escape_char;

      out++;
    }

    if (out == s_escaped_max - 2) break;

    s_escaped[out] = c;
  }

  strncpy (s, s_escaped, s_max - 1);
}

void load_kernel (const char *kernel_file, int num_devices, size_t *kernel_lengths, const u8 **kernel_sources)
{
  FILE *fp = fopen (kernel_file, "rb");

  if (fp != NULL)
  {
    struct stat st;

    memset (&st, 0, sizeof (st));

    stat (kernel_file, &st);

    u8 *buf = (u8 *) mymalloc (st.st_size + 1);

    size_t num_read = fread (buf, sizeof (u8), st.st_size, fp);

    if (num_read != (size_t) st.st_size)
    {
      log_error ("ERROR: %s: %s", kernel_file, strerror (errno));

      exit (-1);
    }

    fclose (fp);

    buf[st.st_size] = 0;

    for (int i = 0; i < num_devices; i++)
    {
      kernel_lengths[i] = (size_t) st.st_size;

      kernel_sources[i] = buf;
    }
  }
  else
  {
    log_error ("ERROR: %s: %s", kernel_file, strerror (errno));

    exit (-1);
  }

  return;
}

void writeProgramBin (char *dst, u8 *binary, size_t binary_size)
{
  if (binary_size > 0)
  {
    FILE *fp = fopen (dst, "wb");

    lock_file (fp);
    fwrite (binary, sizeof (u8), binary_size, fp);

    fflush (fp);
    fclose (fp);
  }
}

/**
 * restore
 */

restore_data_t *init_restore (int argc, char **argv)
{
  restore_data_t *rd = (restore_data_t *) mymalloc (sizeof (restore_data_t));

  if (data.restore_disable == 0)
  {
    FILE *fp = fopen (data.eff_restore_file, "rb");

    if (fp)
    {
      size_t nread = fread (rd, sizeof (restore_data_t), 1, fp);

      if (nread != 1)
      {
        log_error ("ERROR: Cannot read %s", data.eff_restore_file);

        exit (-1);
      }

      fclose (fp);

      if (rd->pid)
      {
        char *pidbin = (char *) mymalloc (HCBUFSIZ);

        int pidbin_len = -1;

        #ifdef _POSIX
        snprintf (pidbin, HCBUFSIZ - 1, "/proc/%d/cmdline", rd->pid);

        FILE *fd = fopen (pidbin, "rb");

        if (fd)
        {
          pidbin_len = fread (pidbin, 1, HCBUFSIZ, fd);

          pidbin[pidbin_len] = 0;

          fclose (fd);

          char *argv0_r = strrchr (argv[0], '/');

          char *pidbin_r = strrchr (pidbin, '/');

          if (argv0_r == NULL) argv0_r = argv[0];

          if (pidbin_r == NULL) pidbin_r = pidbin;

          if (strcmp (argv0_r, pidbin_r) == 0)
          {
            log_error ("ERROR: Already an instance %s running on pid %d", pidbin, rd->pid);

            exit (-1);
          }
        }

        #elif _WIN
        HANDLE hProcess = OpenProcess (PROCESS_ALL_ACCESS, FALSE, rd->pid);

        char *pidbin2 = (char *) mymalloc (HCBUFSIZ);

        int pidbin2_len = -1;

        pidbin_len = GetModuleFileName (NULL, pidbin, HCBUFSIZ);
        pidbin2_len = GetModuleFileNameEx (hProcess, NULL, pidbin2, HCBUFSIZ);

        pidbin[pidbin_len] = 0;
        pidbin2[pidbin2_len] = 0;

        if (pidbin2_len)
        {
          if (strcmp (pidbin, pidbin2) == 0)
          {
            log_error ("ERROR: Already an instance %s running on pid %d", pidbin2, rd->pid);

            exit (-1);
          }
        }

        myfree (pidbin2);

        #endif

        myfree (pidbin);
      }

      if (rd->version_bin < RESTORE_MIN)
      {
        log_error ("ERROR: Cannot use outdated %s. Please remove it.", data.eff_restore_file);

        exit (-1);
      }
    }
  }

  memset (rd, 0, sizeof (restore_data_t));

  rd->version_bin = VERSION_BIN;

  #ifdef _POSIX
  rd->pid = getpid ();
  #elif _WIN
  rd->pid = GetCurrentProcessId ();
  #endif

  if (getcwd (rd->cwd, 255) == NULL)
  {
    myfree (rd);

    return (NULL);
  }

  rd->argc = argc;
  rd->argv = argv;

  return (rd);
}

void read_restore (const char *eff_restore_file, restore_data_t *rd)
{
  FILE *fp = fopen (eff_restore_file, "rb");

  if (fp == NULL)
  {
    log_error ("ERROR: Restore file '%s': %s", eff_restore_file, strerror (errno));

    exit (-1);
  }

  if (fread (rd, sizeof (restore_data_t), 1, fp) != 1)
  {
    log_error ("ERROR: Can't read %s", eff_restore_file);

    exit (-1);
  }

  rd->argv = (char **) mycalloc (rd->argc, sizeof (char *));

  char *buf = (char *) mymalloc (HCBUFSIZ);

  for (uint i = 0; i < rd->argc; i++)
  {
    if (fgets (buf, HCBUFSIZ - 1, fp) == NULL)
    {
      log_error ("ERROR: Can't read %s", eff_restore_file);

      exit (-1);
    }

    size_t len = strlen (buf);

    if (len) buf[len - 1] = 0;

    rd->argv[i] = mystrdup (buf);
  }

  myfree (buf);

  fclose (fp);

  log_info ("INFO: Changing current working directory to the path found within the .restore file: '%s'", rd->cwd);

  if (chdir (rd->cwd))
  {
    log_error ("ERROR: The directory '%s' does not exist. It is needed to restore (--restore) the session.\n"
               "       You could either create this directory (or link it) or update the .restore file using e.g. the analyze_hc_restore.pl tool:\n"
               "       https://github.com/philsmd/analyze_hc_restore\n"
               "       The directory must be relative to (or contain) all files/folders mentioned within the command line.", rd->cwd);

    exit (-1);
  }
}

u64 get_lowest_words_done ()
{
  u64 words_cur = -1llu;

  for (uint device_id = 0; device_id < data.devices_cnt; device_id++)
  {
    hc_device_param_t *device_param = &data.devices_param[device_id];

    if (device_param->skipped) continue;

    const u64 words_done = device_param->words_done;

    if (words_done < words_cur) words_cur = words_done;
  }

  // It's possible that a device's workload isn't finished right after a restore-case.
  // In that case, this function would return 0 and overwrite the real restore point
  // There's also data.words_cur which is set to rd->words_cur but it changes while
  // the attack is running therefore we should stick to rd->words_cur.
  // Note that -s influences rd->words_cur we should keep a close look on that.

  if (words_cur < data.rd->words_cur) words_cur = data.rd->words_cur;

  return words_cur;
}

void write_restore (const char *new_restore_file, restore_data_t *rd)
{
  u64 words_cur = get_lowest_words_done ();

  rd->words_cur = words_cur;

  FILE *fp = fopen (new_restore_file, "wb");

  if (fp == NULL)
  {
    log_error ("ERROR: %s: %s", new_restore_file, strerror (errno));

    exit (-1);
  }

  if (setvbuf (fp, NULL, _IONBF, 0))
  {
    log_error ("ERROR: setvbuf file '%s': %s", new_restore_file, strerror (errno));

    exit (-1);
  }

  fwrite (rd, sizeof (restore_data_t), 1, fp);

  for (uint i = 0; i < rd->argc; i++)
  {
    fprintf (fp, "%s", rd->argv[i]);
    fputc ('\n', fp);
  }

  fflush (fp);

  fsync (fileno (fp));

  fclose (fp);
}

void cycle_restore ()
{
  const char *eff_restore_file = data.eff_restore_file;
  const char *new_restore_file = data.new_restore_file;

  restore_data_t *rd = data.rd;

  write_restore (new_restore_file, rd);

  struct stat st;

  memset (&st, 0, sizeof(st));

  if (stat (eff_restore_file, &st) == 0)
  {
    if (unlink (eff_restore_file))
    {
      log_info ("WARN: Unlink file '%s': %s", eff_restore_file, strerror (errno));
    }
  }

  if (rename (new_restore_file, eff_restore_file))
  {
    log_info ("WARN: Rename file '%s' to '%s': %s", new_restore_file, eff_restore_file, strerror (errno));
  }
}

void check_checkpoint ()
{
  // if (data.restore_disable == 1) break;  (this is already implied by previous checks)

  u64 words_cur = get_lowest_words_done ();

  if (words_cur != data.checkpoint_cur_words)
  {
    myabort ();
  }
}

/**
 * tuning db
 */

void tuning_db_destroy (tuning_db_t *tuning_db)
{
  int i;

  for (i = 0; i < tuning_db->alias_cnt; i++)
  {
    tuning_db_alias_t *alias = &tuning_db->alias_buf[i];

    myfree (alias->device_name);
    myfree (alias->alias_name);
  }

  for (i = 0; i < tuning_db->entry_cnt; i++)
  {
    tuning_db_entry_t *entry = &tuning_db->entry_buf[i];

    myfree (entry->device_name);
  }

  myfree (tuning_db->alias_buf);
  myfree (tuning_db->entry_buf);

  myfree (tuning_db);
}

tuning_db_t *tuning_db_alloc (FILE *fp)
{
  tuning_db_t *tuning_db = (tuning_db_t *) mymalloc (sizeof (tuning_db_t));

  int num_lines = count_lines (fp);

  // a bit over-allocated

  tuning_db->alias_buf = (tuning_db_alias_t *) mycalloc (num_lines + 1, sizeof (tuning_db_alias_t));
  tuning_db->alias_cnt = 0;

  tuning_db->entry_buf = (tuning_db_entry_t *) mycalloc (num_lines + 1, sizeof (tuning_db_entry_t));
  tuning_db->entry_cnt = 0;

  return tuning_db;
}

tuning_db_t *tuning_db_init (const char *tuning_db_file)
{
  FILE *fp = fopen (tuning_db_file, "rb");

  if (fp == NULL)
  {
    log_error ("%s: %s", tuning_db_file, strerror (errno));

    exit (-1);
  }

  tuning_db_t *tuning_db = tuning_db_alloc (fp);

  rewind (fp);

  int line_num = 0;

  char *buf = (char *) mymalloc (HCBUFSIZ);

  while (!feof (fp))
  {
    char *line_buf = fgets (buf, HCBUFSIZ - 1, fp);

    if (line_buf == NULL) break;

    line_num++;

    const int line_len = in_superchop (line_buf);

    if (line_len == 0) continue;

    if (line_buf[0] == '#') continue;

    // start processing

    char *token_ptr[7] = { NULL };

    int token_cnt = 0;

    char *next = strtok (line_buf, "\t ");

    token_ptr[token_cnt] = next;

    token_cnt++;

    while ((next = strtok (NULL, "\t ")) != NULL)
    {
      token_ptr[token_cnt] = next;

      token_cnt++;
    }

    if (token_cnt == 2)
    {
      char *device_name = token_ptr[0];
      char *alias_name  = token_ptr[1];

      tuning_db_alias_t *alias = &tuning_db->alias_buf[tuning_db->alias_cnt];

      alias->device_name = mystrdup (device_name);
      alias->alias_name  = mystrdup (alias_name);

      tuning_db->alias_cnt++;
    }
    else if (token_cnt == 6)
    {
      if ((token_ptr[1][0] != '0') &&
          (token_ptr[1][0] != '1') &&
          (token_ptr[1][0] != '3') &&
          (token_ptr[1][0] != '*'))
      {
        log_info ("WARNING: Tuning-db: Invalid attack_mode '%c' in Line '%u'", token_ptr[1][0], line_num);

        continue;
      }

      if ((token_ptr[3][0] != '1') &&
          (token_ptr[3][0] != '2') &&
          (token_ptr[3][0] != '4') &&
          (token_ptr[3][0] != '8') &&
          (token_ptr[3][0] != 'N'))
      {
        log_info ("WARNING: Tuning-db: Invalid vector_width '%c' in Line '%u'", token_ptr[3][0], line_num);

        continue;
      }

      char *device_name = token_ptr[0];

      int attack_mode      = -1;
      int hash_type        = -1;
      int vector_width     = -1;
      int kernel_accel     = -1;
      int kernel_loops     = -1;

      if (token_ptr[1][0] != '*') attack_mode      = atoi (token_ptr[1]);
      if (token_ptr[2][0] != '*') hash_type        = atoi (token_ptr[2]);
      if (token_ptr[3][0] != 'N') vector_width     = atoi (token_ptr[3]);

      if (token_ptr[4][0] != 'A')
      {
        kernel_accel = atoi (token_ptr[4]);

        if ((kernel_accel < 1) || (kernel_accel > 1024))
        {
          log_info ("WARNING: Tuning-db: Invalid kernel_accel '%d' in Line '%u'", kernel_accel, line_num);

          continue;
        }
      }
      else
      {
        kernel_accel = 0;
      }

      if (token_ptr[5][0] != 'A')
      {
        kernel_loops = atoi (token_ptr[5]);

        if ((kernel_loops < 1) || (kernel_loops > 1024))
        {
          log_info ("WARNING: Tuning-db: Invalid kernel_loops '%d' in Line '%u'", kernel_loops, line_num);

          continue;
        }
      }
      else
      {
        kernel_loops = 0;
      }

      tuning_db_entry_t *entry = &tuning_db->entry_buf[tuning_db->entry_cnt];

      entry->device_name  = mystrdup (device_name);
      entry->attack_mode  = attack_mode;
      entry->hash_type    = hash_type;
      entry->vector_width = vector_width;
      entry->kernel_accel = kernel_accel;
      entry->kernel_loops = kernel_loops;

      tuning_db->entry_cnt++;
    }
    else
    {
      log_info ("WARNING: Tuning-db: Invalid number of token in Line '%u'", line_num);

      continue;
    }
  }

  myfree (buf);

  fclose (fp);

  // todo: print loaded 'cnt' message

  // sort the database

  qsort (tuning_db->alias_buf, tuning_db->alias_cnt, sizeof (tuning_db_alias_t), sort_by_tuning_db_alias);
  qsort (tuning_db->entry_buf, tuning_db->entry_cnt, sizeof (tuning_db_entry_t), sort_by_tuning_db_entry);

  return tuning_db;
}

tuning_db_entry_t *tuning_db_search (tuning_db_t *tuning_db, hc_device_param_t *device_param, int attack_mode, int hash_type)
{
  static tuning_db_entry_t s;

  // first we need to convert all spaces in the device_name to underscore

  char *device_name_nospace = mystrdup (device_param->device_name);

  int device_name_length = strlen (device_name_nospace);

  int i;

  for (i = 0; i < device_name_length; i++)
  {
    if (device_name_nospace[i] == ' ') device_name_nospace[i] = '_';
  }

  // find out if there's an alias configured

  tuning_db_alias_t a;

  a.device_name = device_name_nospace;

  tuning_db_alias_t *alias = bsearch (&a, tuning_db->alias_buf, tuning_db->alias_cnt, sizeof (tuning_db_alias_t), sort_by_tuning_db_alias);

  char *alias_name = (alias == NULL) ? NULL : alias->alias_name;

  // attack-mode 6 and 7 are attack-mode 1 basically

  if (attack_mode == 6) attack_mode = 1;
  if (attack_mode == 7) attack_mode = 1;

  // bsearch is not ideal but fast enough

  s.device_name = device_name_nospace;
  s.attack_mode = attack_mode;
  s.hash_type   = hash_type;

  tuning_db_entry_t *entry = NULL;

  // this will produce all 2^3 combinations required

  for (i = 0; i < 8; i++)
  {
    s.device_name = (i & 1) ? "*" : device_name_nospace;
    s.attack_mode = (i & 2) ?  -1 : attack_mode;
    s.hash_type   = (i & 4) ?  -1 : hash_type;

    entry = bsearch (&s, tuning_db->entry_buf, tuning_db->entry_cnt, sizeof (tuning_db_entry_t), sort_by_tuning_db_entry);

    if (entry != NULL) break;

    // in non-wildcard mode do some additional checks:

    if ((i & 1) == 0)
    {
      // in case we have an alias-name

      if (alias_name != NULL)
      {
        s.device_name = alias_name;

        entry = bsearch (&s, tuning_db->entry_buf, tuning_db->entry_cnt, sizeof (tuning_db_entry_t), sort_by_tuning_db_entry);

        if (entry != NULL) break;
      }

      // or by device type

      if (device_param->device_type & CL_DEVICE_TYPE_CPU)
      {
        s.device_name = "DEVICE_TYPE_CPU";
      }
      else if (device_param->device_type & CL_DEVICE_TYPE_GPU)
      {
        s.device_name = "DEVICE_TYPE_GPU";
      }
      else if (device_param->device_type & CL_DEVICE_TYPE_ACCELERATOR)
      {
        s.device_name = "DEVICE_TYPE_ACCELERATOR";
      }

      entry = bsearch (&s, tuning_db->entry_buf, tuning_db->entry_cnt, sizeof (tuning_db_entry_t), sort_by_tuning_db_entry);

      if (entry != NULL) break;
    }
  }

  // free converted device_name

  myfree (device_name_nospace);

  return entry;
}

/**
 * parallel running threads
 */

#ifdef WIN

BOOL WINAPI sigHandler_default (DWORD sig)
{
  switch (sig)
  {
    case CTRL_CLOSE_EVENT:

      /*
       * special case see: https://stackoverflow.com/questions/3640633/c-setconsolectrlhandler-routine-issue/5610042#5610042
       * if the user interacts w/ the user-interface (GUI/cmd), we need to do the finalization job within this signal handler
       * function otherwise it is too late (e.g. after returning from this function)
       */

      myabort ();

      SetConsoleCtrlHandler (NULL, TRUE);

      hc_sleep (10);

      return TRUE;

    case CTRL_C_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:

      myabort ();

      SetConsoleCtrlHandler (NULL, TRUE);

      return TRUE;
  }

  return FALSE;
}

BOOL WINAPI sigHandler_benchmark (DWORD sig)
{
  switch (sig)
  {
    case CTRL_CLOSE_EVENT:

      myquit ();

      SetConsoleCtrlHandler (NULL, TRUE);

      hc_sleep (10);

      return TRUE;

    case CTRL_C_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:

      myquit ();

      SetConsoleCtrlHandler (NULL, TRUE);

      return TRUE;
  }

  return FALSE;
}

void hc_signal (BOOL WINAPI (callback) (DWORD))
{
  if (callback == NULL)
  {
    SetConsoleCtrlHandler ((PHANDLER_ROUTINE) callback, FALSE);
  }
  else
  {
    SetConsoleCtrlHandler ((PHANDLER_ROUTINE) callback, TRUE);
  }
}

#else

void sigHandler_default (int sig)
{
  myabort ();

  signal (sig, NULL);
}

void sigHandler_benchmark (int sig)
{
  myquit ();

  signal (sig, NULL);
}

void hc_signal (void (callback) (int))
{
  if (callback == NULL) callback = SIG_DFL;

  signal (SIGINT,  callback);
  signal (SIGTERM, callback);
  signal (SIGABRT, callback);
}

#endif

void status_display ();

void *thread_keypress (void *p)
{
  uint quiet = data.quiet;

  tty_break();

  while (data.shutdown_outer == 0)
  {
    int ch = tty_getchar();

    if (ch == -1) break;

    if (ch ==  0) continue;

    //https://github.com/hashcat/hashcat/issues/302
    //#ifdef _POSIX
    //if (ch != '\n')
    //#endif

    hc_thread_mutex_lock (mux_display);

    log_info ("");

    switch (ch)
    {
      case 's':
      case '\r':
      case '\n':

        log_info ("");

        status_display ();

        log_info ("");

        if (quiet == 0) fprintf (stdout, "%s", PROMPT);
        if (quiet == 0) fflush (stdout);

        break;

      case 'b':

        log_info ("");

        bypass ();

        log_info ("");

        if (quiet == 0) fprintf (stdout, "%s", PROMPT);
        if (quiet == 0) fflush (stdout);

        break;

      case 'p':

        log_info ("");

        SuspendThreads ();

        log_info ("");

        if (quiet == 0) fprintf (stdout, "%s", PROMPT);
        if (quiet == 0) fflush (stdout);

        break;

      case 'r':

        log_info ("");

        ResumeThreads ();

        log_info ("");

        if (quiet == 0) fprintf (stdout, "%s", PROMPT);
        if (quiet == 0) fflush (stdout);

        break;

      case 'c':

        log_info ("");

        stop_at_checkpoint ();

        log_info ("");

        if (quiet == 0) fprintf (stdout, "%s", PROMPT);
        if (quiet == 0) fflush (stdout);

        break;

      case 'q':

        log_info ("");

        myabort ();

        break;
    }

    //https://github.com/hashcat/hashcat/issues/302
    //#ifdef _POSIX
    //if (ch != '\n')
    //#endif

    hc_thread_mutex_unlock (mux_display);
  }

  tty_fix();

  return (p);
}

/**
 * rules common
 */

bool class_num (const u8 c)
{
  return ((c >= '0') && (c <= '9'));
}

bool class_lower (const u8 c)
{
  return ((c >= 'a') && (c <= 'z'));
}

bool class_upper (const u8 c)
{
  return ((c >= 'A') && (c <= 'Z'));
}

bool class_alpha (const u8 c)
{
  return (class_lower (c) || class_upper (c));
}

static int conv_ctoi (const u8 c)
{
  if (class_num (c))
  {
    return c - '0';
  }
  else if (class_upper (c))
  {
    return c - 'A' + 10;
  }

  return -1;
}

static int conv_itoc (const u8 c)
{
  if (c < 10)
  {
    return c + '0';
  }
  else if (c < 37)
  {
    return c + 'A' - 10;
  }

  return -1;
}

/**
 * device rules
 */

#define INCR_POS           if (++rule_pos == rule_len) return (-1)
#define SET_NAME(rule,val) (rule)->cmds[rule_cnt]  = ((val) & 0xff) <<  0
#define SET_P0(rule,val)   INCR_POS; (rule)->cmds[rule_cnt] |= ((val) & 0xff) <<  8
#define SET_P1(rule,val)   INCR_POS; (rule)->cmds[rule_cnt] |= ((val) & 0xff) << 16
#define MAX_KERNEL_RULES   255
#define GET_NAME(rule)     rule_cmd = (((rule)->cmds[rule_cnt] >>  0) & 0xff)
#define GET_P0(rule)       INCR_POS; rule_buf[rule_pos] = (((rule)->cmds[rule_cnt] >>  8) & 0xff)
#define GET_P1(rule)       INCR_POS; rule_buf[rule_pos] = (((rule)->cmds[rule_cnt] >> 16) & 0xff)

#define SET_P0_CONV(rule,val)  INCR_POS; (rule)->cmds[rule_cnt] |= ((conv_ctoi (val)) & 0xff) <<  8
#define SET_P1_CONV(rule,val)  INCR_POS; (rule)->cmds[rule_cnt] |= ((conv_ctoi (val)) & 0xff) << 16
#define GET_P0_CONV(rule)      INCR_POS; rule_buf[rule_pos] = conv_itoc (((rule)->cmds[rule_cnt] >>  8) & 0xff)
#define GET_P1_CONV(rule)      INCR_POS; rule_buf[rule_pos] = conv_itoc (((rule)->cmds[rule_cnt] >> 16) & 0xff)

int cpu_rule_to_kernel_rule (char *rule_buf, uint rule_len, kernel_rule_t *rule)
{
  uint rule_pos;
  uint rule_cnt;

  for (rule_pos = 0, rule_cnt = 0; rule_pos < rule_len && rule_cnt < MAX_KERNEL_RULES; rule_pos++, rule_cnt++)
  {
    switch (rule_buf[rule_pos])
    {
      case ' ':
        rule_cnt--;
        break;

      case RULE_OP_MANGLE_NOOP:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_LREST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_UREST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_LREST_UFIRST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_UREST_LFIRST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_TREST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_TOGGLE_AT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_REVERSE:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPEWORD:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPEWORD_TIMES:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_REFLECT:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_ROTATE_LEFT:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_ROTATE_RIGHT:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_APPEND:
        SET_NAME (rule, rule_buf[rule_pos]);
        SET_P0   (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_PREPEND:
        SET_NAME (rule, rule_buf[rule_pos]);
        SET_P0   (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DELETE_FIRST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DELETE_LAST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DELETE_AT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_EXTRACT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        SET_P1_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_OMIT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        SET_P1_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_INSERT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        SET_P1      (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_OVERSTRIKE:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        SET_P1      (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_TRUNCATE_AT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_REPLACE:
        SET_NAME (rule, rule_buf[rule_pos]);
        SET_P0   (rule, rule_buf[rule_pos]);
        SET_P1   (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_PURGECHAR:
        SET_NAME (rule, rule_buf[rule_pos]);
        SET_P0   (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_TOGGLECASE_REC:
        return -1;

      case RULE_OP_MANGLE_DUPECHAR_FIRST:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPECHAR_LAST:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPECHAR_ALL:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_SWITCH_FIRST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_SWITCH_LAST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_SWITCH_AT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        SET_P1_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTL:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTR:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_CHR_INCR:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_CHR_DECR:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_REPLACE_NP1:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_REPLACE_NM1:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_FIRST:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_LAST:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_TITLE:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      default:
        return -1;
    }
  }

  if (rule_pos < rule_len) return -1;

  return 0;
}

int kernel_rule_to_cpu_rule (char *rule_buf, kernel_rule_t *rule)
{
  uint rule_cnt;
  uint rule_pos;
  uint rule_len = HCBUFSIZ - 1; // maximum possible len

  char rule_cmd;

  for (rule_cnt = 0, rule_pos = 0; rule_pos < rule_len && rule_cnt < MAX_KERNEL_RULES; rule_pos++, rule_cnt++)
  {
    GET_NAME (rule);

    if (rule_cnt > 0) rule_buf[rule_pos++] = ' ';

    switch (rule_cmd)
    {
      case RULE_OP_MANGLE_NOOP:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_LREST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_UREST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_LREST_UFIRST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_UREST_LFIRST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_TREST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_TOGGLE_AT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_REVERSE:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_DUPEWORD:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_DUPEWORD_TIMES:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_REFLECT:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_ROTATE_LEFT:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_ROTATE_RIGHT:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_APPEND:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0 (rule);
        break;

      case RULE_OP_MANGLE_PREPEND:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0 (rule);
        break;

      case RULE_OP_MANGLE_DELETE_FIRST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_DELETE_LAST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_DELETE_AT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_EXTRACT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        GET_P1_CONV (rule);
        break;

      case RULE_OP_MANGLE_OMIT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        GET_P1_CONV (rule);
        break;

      case RULE_OP_MANGLE_INSERT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        GET_P1      (rule);
        break;

      case RULE_OP_MANGLE_OVERSTRIKE:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        GET_P1      (rule);
        break;

      case RULE_OP_MANGLE_TRUNCATE_AT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_REPLACE:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0 (rule);
        GET_P1 (rule);
        break;

      case RULE_OP_MANGLE_PURGECHAR:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0 (rule);
        break;

      case RULE_OP_MANGLE_TOGGLECASE_REC:
        return -1;

      case RULE_OP_MANGLE_DUPECHAR_FIRST:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_DUPECHAR_LAST:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_DUPECHAR_ALL:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_SWITCH_FIRST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_SWITCH_LAST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_SWITCH_AT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        GET_P1_CONV (rule);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTL:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTR:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_CHR_INCR:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_CHR_DECR:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_REPLACE_NP1:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_REPLACE_NM1:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_FIRST:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_LAST:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_TITLE:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case 0:
        return rule_pos - 1;

      default:
        return -1;
    }
  }

  if (rule_cnt > 0)
  {
    return rule_pos;
  }

  return -1;
}

/**
 * CPU rules : this is from hashcat sources, cpu based rules
 */

#define NEXT_RULEPOS(rp)      if (++(rp) == rule_len) return (RULE_RC_SYNTAX_ERROR)
#define NEXT_RPTOI(r,rp,up)   if (((up) = conv_ctoi ((r)[(rp)])) == -1) return (RULE_RC_SYNTAX_ERROR)

#define MANGLE_TOGGLE_AT(a,p) if (class_alpha ((a)[(p)])) (a)[(p)] ^= 0x20
#define MANGLE_LOWER_AT(a,p)  if (class_upper ((a)[(p)])) (a)[(p)] ^= 0x20
#define MANGLE_UPPER_AT(a,p)  if (class_lower ((a)[(p)])) (a)[(p)] ^= 0x20

/* #define MANGLE_SWITCH(a,l,r)  { char c = (l); arr[(r)] = arr[(l)]; arr[(l)] = c; } */
/* #define MANGLE_SWITCH(a,l,r)  { char c = (l); (a)[(r)] = (a)[(l)]; (a)[(l)] = c; } */
#define MANGLE_SWITCH(a,l,r)  { char c = (a)[(r)]; (a)[(r)] = (a)[(l)]; (a)[(l)] = c; }

int mangle_lrest (char arr[BLOCK_SIZE], int arr_len)
{
  int pos;

  for (pos = 0; pos < arr_len; pos++) MANGLE_LOWER_AT (arr, pos);

  return (arr_len);
}

int mangle_urest (char arr[BLOCK_SIZE], int arr_len)
{
  int pos;

  for (pos = 0; pos < arr_len; pos++) MANGLE_UPPER_AT (arr, pos);

  return (arr_len);
}

int mangle_trest (char arr[BLOCK_SIZE], int arr_len)
{
  int pos;

  for (pos = 0; pos < arr_len; pos++) MANGLE_TOGGLE_AT (arr, pos);

  return (arr_len);
}

int mangle_reverse (char arr[BLOCK_SIZE], int arr_len)
{
  int l;
  int r;

  for (l = 0; l < arr_len; l++)
  {
    r = arr_len - 1 - l;

    if (l >= r) break;

    MANGLE_SWITCH (arr, l, r);
  }

  return (arr_len);
}

int mangle_double (char arr[BLOCK_SIZE], int arr_len)
{
  if ((arr_len * 2) >= BLOCK_SIZE) return (arr_len);

  memcpy (&arr[arr_len], arr, (size_t) arr_len);

  return (arr_len * 2);
}

int mangle_double_times (char arr[BLOCK_SIZE], int arr_len, int times)
{
  if (((arr_len * times) + arr_len) >= BLOCK_SIZE) return (arr_len);

  int orig_len = arr_len;

  int i;

  for (i = 0; i < times; i++)
  {
    memcpy (&arr[arr_len], arr, orig_len);

    arr_len += orig_len;
  }

  return (arr_len);
}

int mangle_reflect (char arr[BLOCK_SIZE], int arr_len)
{
  if ((arr_len * 2) >= BLOCK_SIZE) return (arr_len);

  mangle_double (arr, arr_len);

  mangle_reverse (arr + arr_len, arr_len);

  return (arr_len * 2);
}

int mangle_rotate_left (char arr[BLOCK_SIZE], int arr_len)
{
  int l;
  int r;

  for (l = 0, r = arr_len - 1; r > 0; r--)
  {
    MANGLE_SWITCH (arr, l, r);
  }

  return (arr_len);
}

int mangle_rotate_right (char arr[BLOCK_SIZE], int arr_len)
{
  int l;
  int r;

  for (l = 0, r = arr_len - 1; l < r; l++)
  {
    MANGLE_SWITCH (arr, l, r);
  }

  return (arr_len);
}

int mangle_append (char arr[BLOCK_SIZE], int arr_len, char c)
{
  if ((arr_len + 1) >= BLOCK_SIZE) return (arr_len);

  arr[arr_len] = c;

  return (arr_len + 1);
}

int mangle_prepend (char arr[BLOCK_SIZE], int arr_len, char c)
{
  if ((arr_len + 1) >= BLOCK_SIZE) return (arr_len);

  int arr_pos;

  for (arr_pos = arr_len - 1; arr_pos > -1; arr_pos--)
  {
    arr[arr_pos + 1] = arr[arr_pos];
  }

  arr[0] = c;

  return (arr_len + 1);
}

int mangle_delete_at (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  int arr_pos;

  for (arr_pos = upos; arr_pos < arr_len - 1; arr_pos++)
  {
    arr[arr_pos] = arr[arr_pos + 1];
  }

  return (arr_len - 1);
}

int mangle_extract (char arr[BLOCK_SIZE], int arr_len, int upos, int ulen)
{
  if (upos >= arr_len) return (arr_len);

  if ((upos + ulen) > arr_len) return (arr_len);

  int arr_pos;

  for (arr_pos = 0; arr_pos < ulen; arr_pos++)
  {
    arr[arr_pos] = arr[upos + arr_pos];
  }

  return (ulen);
}

int mangle_omit (char arr[BLOCK_SIZE], int arr_len, int upos, int ulen)
{
  if (upos >= arr_len) return (arr_len);

  if ((upos + ulen) >= arr_len) return (arr_len);

  int arr_pos;

  for (arr_pos = upos; arr_pos < arr_len - ulen; arr_pos++)
  {
    arr[arr_pos] = arr[arr_pos + ulen];
  }

  return (arr_len - ulen);
}

int mangle_insert (char arr[BLOCK_SIZE], int arr_len, int upos, char c)
{
  if (upos >= arr_len) return (arr_len);

  if ((arr_len + 1) >= BLOCK_SIZE) return (arr_len);

  int arr_pos;

  for (arr_pos = arr_len - 1; arr_pos > upos - 1; arr_pos--)
  {
    arr[arr_pos + 1] = arr[arr_pos];
  }

  arr[upos] = c;

  return (arr_len + 1);
}

static int mangle_insert_multi (char arr[BLOCK_SIZE], int arr_len, int arr_pos, char arr2[BLOCK_SIZE], int arr2_len, int arr2_pos, int arr2_cpy)
{
  if ((arr_len + arr2_cpy) > BLOCK_SIZE) return (RULE_RC_REJECT_ERROR);

  if (arr_pos > arr_len) return (RULE_RC_REJECT_ERROR);

  if (arr2_pos > arr2_len) return (RULE_RC_REJECT_ERROR);

  if ((arr2_pos + arr2_cpy) > arr2_len) return (RULE_RC_REJECT_ERROR);

  if (arr2_cpy < 1) return (RULE_RC_SYNTAX_ERROR);

  memcpy (arr2, arr2 + arr2_pos, arr2_len - arr2_pos);

  memcpy (arr2 + arr2_cpy, arr + arr_pos, arr_len - arr_pos);

  memcpy (arr + arr_pos, arr2, arr_len - arr_pos + arr2_cpy);

  return (arr_len + arr2_cpy);
}

int mangle_overstrike (char arr[BLOCK_SIZE], int arr_len, int upos, char c)
{
  if (upos >= arr_len) return (arr_len);

  arr[upos] = c;

  return (arr_len);
}

int mangle_truncate_at (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  memset (arr + upos, 0, arr_len - upos);

  return (upos);
}

int mangle_replace (char arr[BLOCK_SIZE], int arr_len, char oldc, char newc)
{
  int arr_pos;

  for (arr_pos = 0; arr_pos < arr_len; arr_pos++)
  {
    if (arr[arr_pos] != oldc) continue;

    arr[arr_pos] = newc;
  }

  return (arr_len);
}

int mangle_purgechar (char arr[BLOCK_SIZE], int arr_len, char c)
{
  int arr_pos;

  int ret_len;

  for (ret_len = 0, arr_pos = 0; arr_pos < arr_len; arr_pos++)
  {
    if (arr[arr_pos] == c) continue;

    arr[ret_len] = arr[arr_pos];

    ret_len++;
  }

  return (ret_len);
}

int mangle_dupeblock_prepend (char arr[BLOCK_SIZE], int arr_len, int ulen)
{
  if (ulen > arr_len) return (arr_len);

  if ((arr_len + ulen) >= BLOCK_SIZE) return (arr_len);

  char cs[100] = { 0 };

  memcpy (cs, arr, ulen);

  int i;

  for (i = 0; i < ulen; i++)
  {
    char c = cs[i];

    arr_len = mangle_insert (arr, arr_len, i, c);
  }

  return (arr_len);
}

int mangle_dupeblock_append (char arr[BLOCK_SIZE], int arr_len, int ulen)
{
  if (ulen > arr_len) return (arr_len);

  if ((arr_len + ulen) >= BLOCK_SIZE) return (arr_len);

  int upos = arr_len - ulen;

  int i;

  for (i = 0; i < ulen; i++)
  {
    char c = arr[upos + i];

    arr_len = mangle_append (arr, arr_len, c);
  }

  return (arr_len);
}

int mangle_dupechar_at (char arr[BLOCK_SIZE], int arr_len, int upos, int ulen)
{
  if ( arr_len         ==  0) return (arr_len);
  if ((arr_len + ulen) >= BLOCK_SIZE) return (arr_len);

  char c = arr[upos];

  int i;

  for (i = 0; i < ulen; i++)
  {
    arr_len = mangle_insert (arr, arr_len, upos, c);
  }

  return (arr_len);
}

int mangle_dupechar (char arr[BLOCK_SIZE], int arr_len)
{
  if ( arr_len            ==  0) return (arr_len);
  if ((arr_len + arr_len) >= BLOCK_SIZE) return (arr_len);

  int arr_pos;

  for (arr_pos = arr_len - 1; arr_pos > -1; arr_pos--)
  {
    int new_pos = arr_pos * 2;

    arr[new_pos] = arr[arr_pos];

    arr[new_pos + 1] = arr[arr_pos];
  }

  return (arr_len * 2);
}

int mangle_switch_at_check (char arr[BLOCK_SIZE], int arr_len, int upos, int upos2)
{
  if (upos  >= arr_len) return (arr_len);
  if (upos2 >= arr_len) return (arr_len);

  MANGLE_SWITCH (arr, upos, upos2);

  return (arr_len);
}

int mangle_switch_at (char arr[BLOCK_SIZE], int arr_len, int upos, int upos2)
{
  MANGLE_SWITCH (arr, upos, upos2);

  return (arr_len);
}

int mangle_chr_shiftl (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  arr[upos] <<= 1;

  return (arr_len);
}

int mangle_chr_shiftr (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  arr[upos] >>= 1;

  return (arr_len);
}

int mangle_chr_incr (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  arr[upos] += 1;

  return (arr_len);
}

int mangle_chr_decr (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  arr[upos] -= 1;

  return (arr_len);
}

int mangle_title (char arr[BLOCK_SIZE], int arr_len)
{
  int upper_next = 1;

  int pos;

  for (pos = 0; pos < arr_len; pos++)
  {
    if (arr[pos] == ' ')
    {
      upper_next = 1;

      continue;
    }

    if (upper_next)
    {
      upper_next = 0;

      MANGLE_UPPER_AT (arr, pos);
    }
    else
    {
      MANGLE_LOWER_AT (arr, pos);
    }
  }

  return (arr_len);
}

int generate_random_rule (char rule_buf[RP_RULE_BUFSIZ], u32 rp_gen_func_min, u32 rp_gen_func_max)
{
  u32 rp_gen_num = get_random_num (rp_gen_func_min, rp_gen_func_max);

  u32 j;

  u32 rule_pos = 0;

  for (j = 0; j < rp_gen_num; j++)
  {
    u32 r  = 0;
    u32 p1 = 0;
    u32 p2 = 0;
    u32 p3 = 0;

    switch ((char) get_random_num (0, 9))
    {
      case 0:
        r = get_random_num (0, sizeof (grp_op_nop));
        rule_buf[rule_pos++] = grp_op_nop[r];
        break;

      case 1:
        r = get_random_num (0, sizeof (grp_op_pos_p0));
        rule_buf[rule_pos++] = grp_op_pos_p0[r];
        p1 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        break;

      case 2:
        r = get_random_num (0, sizeof (grp_op_pos_p1));
        rule_buf[rule_pos++] = grp_op_pos_p1[r];
        p1 = get_random_num (1, 6);
        rule_buf[rule_pos++] = grp_pos[p1];
        break;

      case 3:
        r = get_random_num (0, sizeof (grp_op_chr));
        rule_buf[rule_pos++] = grp_op_chr[r];
        p1 = get_random_num (0x20, 0x7e);
        rule_buf[rule_pos++] = (char) p1;
        break;

      case 4:
        r = get_random_num (0, sizeof (grp_op_chr_chr));
        rule_buf[rule_pos++] = grp_op_chr_chr[r];
        p1 = get_random_num (0x20, 0x7e);
        rule_buf[rule_pos++] = (char) p1;
        p2 = get_random_num (0x20, 0x7e);
        while (p1 == p2)
        p2 = get_random_num (0x20, 0x7e);
        rule_buf[rule_pos++] = (char) p2;
        break;

      case 5:
        r = get_random_num (0, sizeof (grp_op_pos_chr));
        rule_buf[rule_pos++] = grp_op_pos_chr[r];
        p1 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        p2 = get_random_num (0x20, 0x7e);
        rule_buf[rule_pos++] = (char) p2;
        break;

      case 6:
        r = get_random_num (0, sizeof (grp_op_pos_pos0));
        rule_buf[rule_pos++] = grp_op_pos_pos0[r];
        p1 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        p2 = get_random_num (0, sizeof (grp_pos));
        while (p1 == p2)
        p2 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p2];
        break;

      case 7:
        r = get_random_num (0, sizeof (grp_op_pos_pos1));
        rule_buf[rule_pos++] = grp_op_pos_pos1[r];
        p1 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        p2 = get_random_num (1, sizeof (grp_pos));
        while (p1 == p2)
        p2 = get_random_num (1, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p2];
        break;

      case 8:
        r = get_random_num (0, sizeof (grp_op_pos1_pos2_pos3));
        rule_buf[rule_pos++] = grp_op_pos1_pos2_pos3[r];
        p1 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        p2 = get_random_num (1, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        p3 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p3];
        break;
    }
  }

  return (rule_pos);
}

int _old_apply_rule (char *rule, int rule_len, char in[BLOCK_SIZE], int in_len, char out[BLOCK_SIZE])
{
  char mem[BLOCK_SIZE] = { 0 };

  if (in == NULL) return (RULE_RC_REJECT_ERROR);

  if (out == NULL) return (RULE_RC_REJECT_ERROR);

  if (in_len < 1 || in_len > BLOCK_SIZE) return (RULE_RC_REJECT_ERROR);

  if (rule_len < 1) return (RULE_RC_REJECT_ERROR);

  int out_len = in_len;
  int mem_len = in_len;

  memcpy (out, in, out_len);

  int rule_pos;

  for (rule_pos = 0; rule_pos < rule_len; rule_pos++)
  {
    int upos, upos2;
    int ulen;

    switch (rule[rule_pos])
    {
      case ' ':
        break;

      case RULE_OP_MANGLE_NOOP:
        break;

      case RULE_OP_MANGLE_LREST:
        out_len = mangle_lrest (out, out_len);
        break;

      case RULE_OP_MANGLE_UREST:
        out_len = mangle_urest (out, out_len);
        break;

      case RULE_OP_MANGLE_LREST_UFIRST:
        out_len = mangle_lrest (out, out_len);
        if (out_len) MANGLE_UPPER_AT (out, 0);
        break;

      case RULE_OP_MANGLE_UREST_LFIRST:
        out_len = mangle_urest (out, out_len);
        if (out_len) MANGLE_LOWER_AT (out, 0);
        break;

      case RULE_OP_MANGLE_TREST:
        out_len = mangle_trest (out, out_len);
        break;

      case RULE_OP_MANGLE_TOGGLE_AT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if (upos < out_len) MANGLE_TOGGLE_AT (out, upos);
        break;

      case RULE_OP_MANGLE_REVERSE:
        out_len = mangle_reverse (out, out_len);
        break;

      case RULE_OP_MANGLE_DUPEWORD:
        out_len = mangle_double (out, out_len);
        break;

      case RULE_OP_MANGLE_DUPEWORD_TIMES:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_double_times (out, out_len, ulen);
        break;

      case RULE_OP_MANGLE_REFLECT:
        out_len = mangle_reflect (out, out_len);
        break;

      case RULE_OP_MANGLE_ROTATE_LEFT:
        mangle_rotate_left (out, out_len);
        break;

      case RULE_OP_MANGLE_ROTATE_RIGHT:
        mangle_rotate_right (out, out_len);
        break;

      case RULE_OP_MANGLE_APPEND:
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_append (out, out_len, rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_PREPEND:
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_prepend (out, out_len, rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_DELETE_FIRST:
        out_len = mangle_delete_at (out, out_len, 0);
        break;

      case RULE_OP_MANGLE_DELETE_LAST:
        out_len = mangle_delete_at (out, out_len, (out_len) ? out_len - 1 : 0);
        break;

      case RULE_OP_MANGLE_DELETE_AT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        out_len = mangle_delete_at (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_EXTRACT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_extract (out, out_len, upos, ulen);
        break;

      case RULE_OP_MANGLE_OMIT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_omit (out, out_len, upos, ulen);
        break;

      case RULE_OP_MANGLE_INSERT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_insert (out, out_len, upos, rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_OVERSTRIKE:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_overstrike (out, out_len, upos, rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_TRUNCATE_AT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        out_len = mangle_truncate_at (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_REPLACE:
        NEXT_RULEPOS (rule_pos);
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_replace (out, out_len, rule[rule_pos - 1], rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_PURGECHAR:
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_purgechar (out, out_len, rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_TOGGLECASE_REC:
        /* todo */
        break;

      case RULE_OP_MANGLE_DUPECHAR_FIRST:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_dupechar_at (out, out_len, 0, ulen);
        break;

      case RULE_OP_MANGLE_DUPECHAR_LAST:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_dupechar_at (out, out_len, out_len - 1, ulen);
        break;

      case RULE_OP_MANGLE_DUPECHAR_ALL:
        out_len = mangle_dupechar (out, out_len);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_FIRST:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_dupeblock_prepend (out, out_len, ulen);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_LAST:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_dupeblock_append (out, out_len, ulen);
        break;

      case RULE_OP_MANGLE_SWITCH_FIRST:
        if (out_len >= 2) mangle_switch_at (out, out_len, 0, 1);
        break;

      case RULE_OP_MANGLE_SWITCH_LAST:
        if (out_len >= 2) mangle_switch_at (out, out_len, out_len - 1, out_len - 2);
        break;

      case RULE_OP_MANGLE_SWITCH_AT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos2);
        out_len = mangle_switch_at_check (out, out_len, upos, upos2);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTL:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        mangle_chr_shiftl (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTR:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        mangle_chr_shiftr (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_CHR_INCR:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        mangle_chr_incr (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_CHR_DECR:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        mangle_chr_decr (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_REPLACE_NP1:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if ((upos >= 0) && ((upos + 1) < out_len)) mangle_overstrike (out, out_len, upos, out[upos + 1]);
        break;

      case RULE_OP_MANGLE_REPLACE_NM1:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if ((upos >= 1) && ((upos + 0) < out_len)) mangle_overstrike (out, out_len, upos, out[upos - 1]);
        break;

      case RULE_OP_MANGLE_TITLE:
        out_len = mangle_title (out, out_len);
        break;

      case RULE_OP_MANGLE_EXTRACT_MEMORY:
        if (mem_len < 1) return (RULE_RC_REJECT_ERROR);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos2);
        if ((out_len = mangle_insert_multi (out, out_len, upos2, mem, mem_len, upos, ulen)) < 1) return (out_len);
        break;

      case RULE_OP_MANGLE_APPEND_MEMORY:
        if (mem_len < 1) return (RULE_RC_REJECT_ERROR);
        if ((out_len + mem_len) > BLOCK_SIZE) return (RULE_RC_REJECT_ERROR);
        memcpy (out + out_len, mem, mem_len);
        out_len += mem_len;
        break;

      case RULE_OP_MANGLE_PREPEND_MEMORY:
        if (mem_len < 1) return (RULE_RC_REJECT_ERROR);
        if ((mem_len + out_len) > BLOCK_SIZE) return (RULE_RC_REJECT_ERROR);
        memcpy (mem + mem_len, out, out_len);
        out_len += mem_len;
        memcpy (out, mem, out_len);
        break;

      case RULE_OP_MEMORIZE_WORD:
        memcpy (mem, out, out_len);
        mem_len = out_len;
        break;

      case RULE_OP_REJECT_LESS:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if (out_len > upos) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_GREATER:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if (out_len < upos) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_CONTAIN:
        NEXT_RULEPOS (rule_pos);
        if (strchr (out, rule[rule_pos]) != NULL) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_NOT_CONTAIN:
        NEXT_RULEPOS (rule_pos);
        if (strchr (out, rule[rule_pos]) == NULL) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_EQUAL_FIRST:
        NEXT_RULEPOS (rule_pos);
        if (out[0] != rule[rule_pos]) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_EQUAL_LAST:
        NEXT_RULEPOS (rule_pos);
        if (out[out_len - 1] != rule[rule_pos]) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_EQUAL_AT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if ((upos + 1) > out_len) return (RULE_RC_REJECT_ERROR);
        NEXT_RULEPOS (rule_pos);
        if (out[upos] != rule[rule_pos]) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_CONTAINS:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if ((upos + 1) > out_len) return (RULE_RC_REJECT_ERROR);
        NEXT_RULEPOS (rule_pos);
        int c; int cnt; for (c = 0, cnt = 0; c < out_len; c++) if (out[c] == rule[rule_pos]) cnt++;
        if (cnt < upos) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_MEMORY:
        if ((out_len == mem_len) && (memcmp (out, mem, out_len) == 0)) return (RULE_RC_REJECT_ERROR);
        break;

      default:
        return (RULE_RC_SYNTAX_ERROR);
    }
  }

  memset (out + out_len, 0, BLOCK_SIZE - out_len);

  return (out_len);
}
