/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_os.h>
#include <node_api.h>
#include <uv.h>

#include <climits>
#include <cstdio>
#include <cstring>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Set error info on a ctx object (for the getCheckedFunction pattern in
/// os.js). The JS side checks if the return value is undefined and throws
/// ERR_SYSTEM_ERROR with the ctx.
static void
setCtxError(napi_env env, napi_value ctx, int errorno, const char *syscall) {
  napi_value val;

  napi_create_int32(env, errorno, &val);
  napi_set_named_property(env, ctx, "errno", val);

  napi_create_string_utf8(env, uv_strerror(errorno), NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, ctx, "message", val);

  napi_create_string_utf8(env, syscall, NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, ctx, "syscall", val);

  // Also set 'code' — Node's ERR_SYSTEM_ERROR uses ctx.code if present.
  napi_create_string_utf8(env, uv_err_name(errorno), NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, ctx, "code", val);
}

// ---------------------------------------------------------------------------
// getHostname(ctx) -> string | undefined
// ---------------------------------------------------------------------------

static napi_value getHostname(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  char buf[UV_MAXHOSTNAMESIZE];
  size_t size = sizeof(buf);
  int r = uv_os_gethostname(buf, &size);

  if (r != 0) {
    if (argc >= 1)
      setCtxError(env, argv[0], r, "uv_os_gethostname");
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  napi_value result;
  napi_create_string_utf8(env, buf, size, &result);
  return result;
}

// ---------------------------------------------------------------------------
// getOSInformation(ctx) -> [sysname, version, release, machine] | undefined
// ---------------------------------------------------------------------------

static napi_value getOSInformation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  uv_utsname_t utsInfo;
  int err = uv_os_uname(&utsInfo);

  if (err != 0) {
    if (argc >= 1)
      setCtxError(env, argv[0], err, "uv_os_uname");
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Return [sysname, version, release, machine]
  napi_value elements[4];
  napi_create_string_utf8(env, utsInfo.sysname, NAPI_AUTO_LENGTH, &elements[0]);
  napi_create_string_utf8(env, utsInfo.version, NAPI_AUTO_LENGTH, &elements[1]);
  napi_create_string_utf8(env, utsInfo.release, NAPI_AUTO_LENGTH, &elements[2]);
  napi_create_string_utf8(env, utsInfo.machine, NAPI_AUTO_LENGTH, &elements[3]);

  napi_value result;
  napi_create_array_with_length(env, 4, &result);
  for (uint32_t i = 0; i < 4; ++i)
    napi_set_element(env, result, i, elements[i]);

  return result;
}

// ---------------------------------------------------------------------------
// getLoadAvg(Float64Array) -> void
// ---------------------------------------------------------------------------

static napi_value getLoadAvg(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // argv[0] should be a Float64Array of length 3.
  void *data = nullptr;
  size_t length = 0;
  napi_get_typedarray_info(
      env, argv[0], nullptr, &length, &data, nullptr, nullptr);

  if (data && length >= 3) {
    uv_loadavg(static_cast<double *>(data));
  }

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// getUptime(ctx) -> number | undefined
// ---------------------------------------------------------------------------

static napi_value getUptime(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  double uptime;
  int err = uv_uptime(&uptime);

  if (err != 0) {
    if (argc >= 1)
      setCtxError(env, argv[0], err, "uv_uptime");
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  napi_value result;
  napi_create_double(env, uptime, &result);
  return result;
}

// ---------------------------------------------------------------------------
// getCPUs() -> flat array [model, speed, user, nice, sys, idle, irq, ...]
// ---------------------------------------------------------------------------

static napi_value getCPUs(napi_env env, napi_callback_info /*info*/) {
  uv_cpu_info_t *cpuInfos;
  int count;

  int err = uv_cpu_info(&cpuInfos, &count);
  if (err) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Build flat array: [model, speed, user, nice, sys, idle, irq, ...]
  napi_value result;
  napi_create_array_with_length(env, static_cast<size_t>(count) * 7, &result);

  uint32_t idx = 0;
  for (int i = 0; i < count; i++) {
    uv_cpu_info_t *ci = &cpuInfos[i];

    napi_value val;
    napi_create_string_utf8(env, ci->model, NAPI_AUTO_LENGTH, &val);
    napi_set_element(env, result, idx++, val);

    napi_create_double(env, static_cast<double>(ci->speed), &val);
    napi_set_element(env, result, idx++, val);

    napi_create_double(env, static_cast<double>(ci->cpu_times.user), &val);
    napi_set_element(env, result, idx++, val);

    napi_create_double(env, static_cast<double>(ci->cpu_times.nice), &val);
    napi_set_element(env, result, idx++, val);

    napi_create_double(env, static_cast<double>(ci->cpu_times.sys), &val);
    napi_set_element(env, result, idx++, val);

    napi_create_double(env, static_cast<double>(ci->cpu_times.idle), &val);
    napi_set_element(env, result, idx++, val);

    napi_create_double(env, static_cast<double>(ci->cpu_times.irq), &val);
    napi_set_element(env, result, idx++, val);
  }

  uv_free_cpu_info(cpuInfos, count);
  return result;
}

// ---------------------------------------------------------------------------
// getFreeMem() -> number
// ---------------------------------------------------------------------------

static napi_value getFreeMem(napi_env env, napi_callback_info /*info*/) {
  double amount = static_cast<double>(uv_get_free_memory());
  napi_value result;
  napi_create_double(env, amount, &result);
  return result;
}

// ---------------------------------------------------------------------------
// getTotalMem() -> number
// ---------------------------------------------------------------------------

static napi_value getTotalMem(napi_env env, napi_callback_info /*info*/) {
  double amount = static_cast<double>(uv_get_total_memory());
  napi_value result;
  napi_create_double(env, amount, &result);
  return result;
}

// ---------------------------------------------------------------------------
// getHomeDirectory(ctx) -> string | undefined
// ---------------------------------------------------------------------------

static napi_value getHomeDirectory(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  char buf[PATH_MAX];
  size_t len = sizeof(buf);
  int err = uv_os_homedir(buf, &len);

  if (err) {
    if (argc >= 1)
      setCtxError(env, argv[0], err, "uv_os_homedir");
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  napi_value result;
  napi_create_string_utf8(env, buf, len, &result);
  return result;
}

// ---------------------------------------------------------------------------
// getUserInfo(options, ctx) -> {uid, gid, username, homedir, shell} | undefined
// ---------------------------------------------------------------------------

static napi_value getUserInfo(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  uv_passwd_t pwd;
  int err = uv_os_get_passwd(&pwd);

  if (err) {
    // ctx is the last argument.
    if (argc >= 2)
      setCtxError(env, argv[argc - 1], err, "uv_os_get_passwd");
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Build result object: {uid, gid, username, homedir, shell}
  napi_value result;
  napi_create_object(env, &result);

  napi_value val;

  napi_create_double(env, static_cast<double>(pwd.uid), &val);
  napi_set_named_property(env, result, "uid", val);

  napi_create_double(env, static_cast<double>(pwd.gid), &val);
  napi_set_named_property(env, result, "gid", val);

  if (pwd.username) {
    napi_create_string_utf8(env, pwd.username, NAPI_AUTO_LENGTH, &val);
  } else {
    napi_get_undefined(env, &val);
  }
  napi_set_named_property(env, result, "username", val);

  if (pwd.homedir) {
    napi_create_string_utf8(env, pwd.homedir, NAPI_AUTO_LENGTH, &val);
  } else {
    napi_get_undefined(env, &val);
  }
  napi_set_named_property(env, result, "homedir", val);

  if (pwd.shell) {
    napi_create_string_utf8(env, pwd.shell, NAPI_AUTO_LENGTH, &val);
  } else {
    napi_get_null(env, &val);
  }
  napi_set_named_property(env, result, "shell", val);

  uv_os_free_passwd(&pwd);
  return result;
}

// ---------------------------------------------------------------------------
// getInterfaceAddresses(ctx) -> flat array | undefined
// [name, address, netmask, family, mac, internal, scopeid, ...]
// ---------------------------------------------------------------------------

static napi_value getInterfaceAddresses(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  uv_interface_address_t *interfaces;
  int count;

  int err = uv_interface_addresses(&interfaces, &count);

  if (err == UV_ENOSYS) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  if (err) {
    if (argc >= 1)
      setCtxError(env, argv[0], err, "uv_interface_addresses");
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Build flat array: [name, address, netmask, family, mac, internal, scopeid,
  // ...]
  napi_value result;
  napi_create_array_with_length(env, static_cast<size_t>(count) * 7, &result);

  uint32_t idx = 0;
  for (int i = 0; i < count; i++) {
    char ip[INET6_ADDRSTRLEN];
    char netmask[INET6_ADDRSTRLEN];
    char mac[18];

    std::snprintf(
        mac,
        sizeof(mac),
        "%02x:%02x:%02x:%02x:%02x:%02x",
        static_cast<unsigned char>(interfaces[i].phys_addr[0]),
        static_cast<unsigned char>(interfaces[i].phys_addr[1]),
        static_cast<unsigned char>(interfaces[i].phys_addr[2]),
        static_cast<unsigned char>(interfaces[i].phys_addr[3]),
        static_cast<unsigned char>(interfaces[i].phys_addr[4]),
        static_cast<unsigned char>(interfaces[i].phys_addr[5]));

    const char *family;
    int scopeid = -1;

    if (interfaces[i].address.address4.sin_family == AF_INET) {
      uv_ip4_name(&interfaces[i].address.address4, ip, sizeof(ip));
      uv_ip4_name(&interfaces[i].netmask.netmask4, netmask, sizeof(netmask));
      family = "IPv4";
    } else if (interfaces[i].address.address4.sin_family == AF_INET6) {
      uv_ip6_name(&interfaces[i].address.address6, ip, sizeof(ip));
      uv_ip6_name(&interfaces[i].netmask.netmask6, netmask, sizeof(netmask));
      family = "IPv6";
      scopeid = static_cast<int>(interfaces[i].address.address6.sin6_scope_id);
    } else {
      std::strncpy(ip, "<unknown sa family>", INET6_ADDRSTRLEN);
      ip[INET6_ADDRSTRLEN - 1] = '\0';
      netmask[0] = '\0';
      family = "Unknown";
    }

    napi_value val;

    // name
    napi_create_string_utf8(env, interfaces[i].name, NAPI_AUTO_LENGTH, &val);
    napi_set_element(env, result, idx++, val);

    // address
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &val);
    napi_set_element(env, result, idx++, val);

    // netmask
    napi_create_string_utf8(env, netmask, NAPI_AUTO_LENGTH, &val);
    napi_set_element(env, result, idx++, val);

    // family
    napi_create_string_utf8(env, family, NAPI_AUTO_LENGTH, &val);
    napi_set_element(env, result, idx++, val);

    // mac
    napi_create_string_utf8(env, mac, 17, &val);
    napi_set_element(env, result, idx++, val);

    // internal
    napi_get_boolean(env, interfaces[i].is_internal, &val);
    napi_set_element(env, result, idx++, val);

    // scopeid (-1 for IPv4)
    napi_create_int32(env, scopeid, &val);
    napi_set_element(env, result, idx++, val);
  }

  uv_free_interface_addresses(interfaces, count);
  return result;
}

// ---------------------------------------------------------------------------
// setPriority(pid, priority, ctx) -> int (0 on success)
// ---------------------------------------------------------------------------

static napi_value setPriority(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t pid = 0;
  int32_t priority = 0;
  napi_get_value_int32(env, argv[0], &pid);
  napi_get_value_int32(env, argv[1], &priority);

  int err = uv_os_setpriority(pid, priority);

  if (err) {
    if (argc >= 3)
      setCtxError(env, argv[2], err, "uv_os_setpriority");
  }

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// getPriority(pid, ctx) -> int | undefined
// ---------------------------------------------------------------------------

static napi_value getPriority(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t pid = 0;
  napi_get_value_int32(env, argv[0], &pid);

  int priority;
  int err = uv_os_getpriority(pid, &priority);

  if (err) {
    if (argc >= 2)
      setCtxError(env, argv[1], err, "uv_os_getpriority");
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  napi_value result;
  napi_create_int32(env, priority, &result);
  return result;
}

// ---------------------------------------------------------------------------
// getAvailableParallelism() -> uint32
// ---------------------------------------------------------------------------

static napi_value getAvailableParallelism(
    napi_env env,
    napi_callback_info /*info*/) {
  unsigned int parallelism = uv_available_parallelism();
  napi_value result;
  napi_create_uint32(env, parallelism, &result);
  return result;
}

// ---------------------------------------------------------------------------
// initOsBinding
// ---------------------------------------------------------------------------

napi_value initOsBinding(napi_env env, napi_value exports) {
  napi_property_descriptor props[] = {
      {"getHostname",
       nullptr,
       getHostname,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getOSInformation",
       nullptr,
       getOSInformation,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getLoadAvg",
       nullptr,
       getLoadAvg,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getUptime",
       nullptr,
       getUptime,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getCPUs",
       nullptr,
       getCPUs,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getFreeMem",
       nullptr,
       getFreeMem,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getTotalMem",
       nullptr,
       getTotalMem,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getHomeDirectory",
       nullptr,
       getHomeDirectory,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getUserInfo",
       nullptr,
       getUserInfo,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getInterfaceAddresses",
       nullptr,
       getInterfaceAddresses,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"setPriority",
       nullptr,
       setPriority,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getPriority",
       nullptr,
       getPriority,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getAvailableParallelism",
       nullptr,
       getAvailableParallelism,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };

  napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);

  // isBigEndian boolean property.
  napi_value isBigEndian;
  // Check endianness at runtime.
  union {
    uint32_t i;
    char c[4];
  } check = {0x01020304};
  napi_get_boolean(env, check.c[0] == 1, &isBigEndian);
  napi_set_named_property(env, exports, "isBigEndian", isBigEndian);

  return exports;
}

} // namespace node_compat
} // namespace hermes
