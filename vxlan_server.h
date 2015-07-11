#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/rculist.h>
#include <linux/netdevice.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/igmp.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/hash.h>
#include <linux/ethtool.h>
#include <net/arp.h>
#include <net/ndisc.h>
#include <net/ip.h>
#include <net/ip_tunnels.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/rtnetlink.h>
#include <net/route.h>
#include <net/dsfield.h>
#include <net/inet_ecn.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <linux/jhash.h>

#define FALSE 0
#define TRUE (!FALSE)

#define CLIENT_HASH_SIZE 4096   /*must be power of 2*/
#define CLIENT_HASH_MASK (CLIENT_HASH_SIZE-1)

#define ENDPOINT_HASH_SIZE 256
#define ENDPOINT_HASH_MASK (ENDPOINT_HASH_SIZE-1)

#define DEFAULT_CLIENT_AGE_TIME (HZ*120)
#define DEFAULT_ENDPOINT_AGE_TIME (HZ*300)

#define VXLAN_HEADER_ROOM (14+20+8+8)
//struct vxlan_client;

struct vxlan_server_dev{
	struct net_device *ndev;
	int dev_ok;
	struct vxlan_client*vc_local;
};

#define TBYTE_SORDER(a) ((((a)<<16)&0xff0000)|(((a)>>16)&0xff)|((a)&0xff00))
struct vxlan_server_net{
	struct list_head list;
	uint32_t vni;
	struct socket* socket;
	struct vxlan_server_dev vsd;

	uint32_t default_ip;
	uint16_t default_port;
	
	spinlock_t client_hash_lock;
	spinlock_t endpoint_hash_lock;
	struct hlist_head __rcu client_hhead[CLIENT_HASH_SIZE];
	struct hlist_head __rcu endpoint_hhead[ENDPOINT_HASH_SIZE]; //I suppose endpoint would be as that much as clients,this is why I choose linked list
	
};


#define DEFAULT_BINDING_PORT 4789
#define DEFAULT_VNI 41

struct vxlan_hdr{
	uint32_t flag:8;
	uint32_t reserved1:24;
	uint32_t vni:24;
	uint32_t reserved2:8;
}__attribute__((packed));


/***********
*the vxlan endpoint may be mapped by several vxlan clients
*and this endpoint will decide which host this packet will be directed to 
************/
struct vxlan_endpoint{
	struct hlist_node hnode;
	struct rcu_head rcu;
	uint32_t src_ip;
	uint16_t src_port;
	
	int is_local_port;
	uint64_t jiffie_cnt;
};
struct vxlan_client{
	struct hlist_node hnode;
	struct rcu_head rcu;
	/*why we still keep src_ip and src_port here at the premise that endpoint hash enough information already*/
	uint8_t mac[6];
	uint32_t src_ip;
	uint16_t src_port;
	
	int is_local_port;
	uint64_t jiffie_cnt;
	
};
int is_mac_addr_equal(uint8_t mac1[],uint8_t mac2[]);
int copy_mac_addr(uint8_t mac1[],const uint8_t mac2[]);


