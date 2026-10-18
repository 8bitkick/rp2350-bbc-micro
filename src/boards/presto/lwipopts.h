#ifndef PRESTO_LWIPOPTS_H
#define PRESTO_LWIPOPTS_H

// lwIP for the web control panel (presto_net.c): no OS, polled from core 0 between frames and
// while it waits for core 1, so nothing here runs in an interrupt. Kept small: the panel only
// ever has a browser or two talking to it.

// the board header would put lwIP's memory in a PSRAM section our linker script doesn't have
#undef LWIP_DECLARE_MEMORY_ALIGNED

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (8 * 1024)      // outgoing data (the page itself goes out from flash, uncopied)
#define MEMP_NUM_TCP_PCB            12
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_PBUF               32              // the page goes out from flash, a pbuf per piece; several at once
#define MEMP_NUM_ARP_QUEUE          10
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)
#define PBUF_POOL_SIZE              8               // incoming packets, ~1.5KB each
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)   // uploads are copied out as they arrive
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define SO_REUSE                    1

// beeb.local
#define LWIP_IGMP                   1
#define LWIP_MDNS_RESPONDER         1
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#define MDNS_MAX_SERVICES           1

#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_STATS                  0

#endif
