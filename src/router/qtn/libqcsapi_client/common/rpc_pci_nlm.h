#ifndef __PCI_NLM_H__

#define __PCI_NLM_H__

/*
 * We seperate the netlink type for client and server here.
 * If the netlink type is conflicted with customers', they just need to modify
 * NETLINK_RPC_PCI_CLNT and the type define in the PCIe RC driver and the netlink
 * type in the rpc server and PCIe EP driver will not be affected.
 */
#define NETLINK_RPC_PCI_CLNT 31
#define NETLINK_RPC_PCI_SVC 31
#define PCIMSGSIZE 2048

/*
 * Nelink Message types.
 */

#define NETLINK_TYPE_SVC_REGISTER 10
#define NETLINK_TYPE_SVC_RESPONSE 11

#define NETLINK_TYPE_CLNT_REGISTER 10
#define NETLINK_TYPE_CLNT_REQUEST 11

#endif
