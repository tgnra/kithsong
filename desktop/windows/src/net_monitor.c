#include <winsock2.h> /* before windows.h, and ws2ipdef.h for netioapi.h */
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "net_monitor.h"

static struct {
  HANDLE handle;
  net_monitor_fn fn;
  void *user;
} g_net;

static VOID NETIOAPI_API_ on_change(PVOID context, PMIB_IPINTERFACE_ROW row, MIB_NOTIFICATION_TYPE type) {
  (void)context, (void)row;
  if (type == MibInitialNotification) return;
  if (g_net.fn) g_net.fn(g_net.user);
}

BOOL net_monitor_start(net_monitor_fn fn, void *user) {
  net_monitor_stop();
  g_net.fn = fn;
  g_net.user = user;
  HANDLE h = NULL;
  if (NotifyIpInterfaceChange(AF_UNSPEC, on_change, NULL, FALSE, &h) != NO_ERROR) return FALSE;
  g_net.handle = h;
  return TRUE;
}

void net_monitor_stop(void) {
  if (g_net.handle) CancelMibChangeNotify2(g_net.handle);
  g_net.handle = NULL;
  g_net.fn = NULL;
  g_net.user = NULL;
}
