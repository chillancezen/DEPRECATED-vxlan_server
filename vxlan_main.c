#include "vxlan_server.h"
int vxlan_server_id;

struct list_head gnet_list;

int glocal_port=DEFAULT_BINDING_PORT;
int gvni=DEFAULT_VNI;


void vxlan_server_client_free(struct rcu_head*rcu)
{
	struct vxlan_client *vc=container_of(rcu,struct vxlan_client,rcu);
	//printk("free:%02x,%02x\n",vc->mac[0],vc->mac[1]);
	kfree(vc);
}
void vxlan_server_endpoint_free(struct rcu_head* rcu)
{
	struct vxlan_endpoint *ve=container_of(rcu,struct vxlan_endpoint,rcu);
	kfree(ve);
	
}
/*
* try to find the mac related vxlan client
*if b_rcu_protect is True,then this routine will accquire RCU lock,and traverse the entire list in RCU context,otherwise not.
*and we set b_rcu_protect not NULL,as long as we make it clear that,this function is called with rcu_read_lock() invoked before somewhere
* the most important point is if we set b_age_check to TRUE,then we must be make sure that the spin-lock is accquired somewhere else before,
*/
struct vxlan_client * vxlan_server_find_client(struct net*net,uint8_t mac[],int b_rcu_protect,int b_age_check)
{
	uint64_t diff_time;
	struct vxlan_client *cl=NULL,*cl_tmp=NULL;
	struct vxlan_server_net* vsn=net_generic(net,vxlan_server_id);
	struct hlist_head *hhead;
	int hash_idx=jhash(mac,6,0x12345678);
	hash_idx&=CLIENT_HASH_MASK;
	hhead=&vsn->client_hhead[hash_idx];

	if(b_rcu_protect){
		hlist_for_each_entry_rcu(cl_tmp,hhead,hnode){
			if(compare_ether_addr(cl_tmp->mac,mac)==0){
				cl=cl_tmp;
				cl->jiffie_cnt=jiffies_64;
				break;
			}
			if(!b_age_check)
				continue;
			/*here we check age expiry timer */
			if(cl_tmp->is_local_port)
				continue;//local port must be skipped and remain here for a long time
			diff_time=jiffies_64-cl_tmp->jiffie_cnt;
			if(diff_time>DEFAULT_CLIENT_AGE_TIME){
				hlist_del_rcu(&cl_tmp->hnode);
				call_rcu(&cl_tmp->rcu,vxlan_server_client_free);
			}
		}
	}else{
		hlist_for_each_entry(cl_tmp,hhead,hnode){
			if(compare_ether_addr(cl_tmp->mac,mac)==0){
				cl=cl_tmp;
				cl->jiffie_cnt=jiffies_64;
				break;
			}
			if(!b_age_check)
				continue;
			if(cl_tmp->is_local_port)
				continue;
			/*here we check age expiry timer */
			diff_time=jiffies_64-cl_tmp->jiffie_cnt;
			if(diff_time>DEFAULT_CLIENT_AGE_TIME){
				hlist_del_rcu(&cl_tmp->hnode);
				call_rcu(&cl_tmp->rcu,vxlan_server_client_free);
			}
		}
	}
	return cl;
}
/*
*try to create a vxlan_client ,since there are multiple updater which are all trying to create the same data entry,we must acquire the per-device 
*spinlock,and check the hash table again if the entry already exists 
*/
struct vxlan_client *vxlan_server_create_client(struct net*net,uint8_t mac[],uint32_t src_ip,uint16_t src_port,int b_local,int b_rcu_protect)
{
	struct vxlan_client*vc=NULL;
	struct vxlan_server_net *vsn=net_generic(net,vxlan_server_id);
	struct hlist_head *hhead;
	int hash_idx=jhash(mac,6,0x12345678);
	hash_idx&=CLIENT_HASH_MASK;
	hhead=&vsn->client_hhead[hash_idx];
	
	
	spin_lock(&vsn->client_hash_lock);
	vc=vxlan_server_find_client(net,mac,b_rcu_protect,TRUE);//since we may have several update,we must check it agin befor we really allocate a new sructure
	if(vc)
		goto ret_flag;
	vc=kzalloc(sizeof(struct vxlan_client),GFP_ATOMIC);
	if(!vc)
		goto ret_flag;
	copy_mac_addr(vc->mac,mac);
	vc->src_ip=src_ip;
	vc->src_port=src_port;
	vc->jiffie_cnt=jiffies_64;
	vc->is_local_port=b_local?TRUE:FALSE;
	hlist_add_head_rcu(&vc->hnode,hhead);
	
	#if 0
	rcu_read_lock();
	hlist_for_each_entry_rcu(vc_tmp,hhead,hnode){
		printk("ele:%d\n",mac[0]);
	}
	rcu_read_unlock();
	#endif
	ret_flag:
	spin_unlock(&vsn->client_hash_lock);	
	return vc;
}
/*
*try to find an endpoint in the list,
*/
struct vxlan_endpoint* vxlan_server_find_endpoint(struct net*net,uint32_t src_ip,uint16_t src_port,int b_rcu_protect,int b_age_check)
{	
	uint64_t diff_time;
	struct vxlan_server_net *vsn=net_generic(net,vxlan_server_id);
	struct vxlan_endpoint *ve=NULL,*ve_tmp;
	struct hlist_head*hhead;
	int hash_idx=jhash_2words(src_ip,src_port,0x12345678);
	hash_idx&=ENDPOINT_HASH_MASK;
	hhead=&vsn->endpoint_hhead[hash_idx];
	
	if(b_rcu_protect){
		hlist_for_each_entry_rcu(ve_tmp,hhead,hnode){
			if(ve_tmp->src_ip==src_ip&&ve_tmp->src_port==src_port){
				ve=ve_tmp;
				break;
			}
			if(!b_age_check)
				continue;
			diff_time=jiffies_64-ve_tmp->jiffie_cnt;
			if(diff_time>DEFAULT_ENDPOINT_AGE_TIME){
				hlist_del_rcu(&ve_tmp->hnode);
				call_rcu(&ve_tmp->rcu,vxlan_server_endpoint_free);
			}
		}
	}else{
		hlist_for_each_entry(ve_tmp,hhead,hnode){
			if(ve_tmp->src_ip==src_ip&&ve_tmp->src_port==src_port){
				ve=ve_tmp;
				break;
			}
			if(!b_age_check)
				continue;
			diff_time=jiffies_64-ve_tmp->jiffie_cnt;
			if(diff_time>DEFAULT_ENDPOINT_AGE_TIME){
				hlist_del_rcu(&ve_tmp->hnode);
				call_rcu(&ve_tmp->rcu,vxlan_server_endpoint_free);
			}
		}
	}
	
	return ve;
}
/*
* hold rcu_read lock or not ,it will  make any sense
*/
struct vxlan_endpoint*vxlan_server_create_endpoint(struct net*net,uint32_t src_ip,uint16_t src_port,int b_local,int b_rcu_protect)
{
	struct vxlan_server_net *vsn=net_generic(net,vxlan_server_id);
	struct vxlan_endpoint *ve=NULL;
	struct hlist_head*hhead;
	int hash_idx=jhash_2words(src_ip,src_port,0x12345678);
	hash_idx&=ENDPOINT_HASH_MASK;
	hhead=&vsn->endpoint_hhead[hash_idx];

	spin_lock(&vsn->endpoint_hash_lock);
	ve=vxlan_server_find_endpoint(net,src_ip,src_port,b_rcu_protect,TRUE);
	if(ve)
		goto ret_flag;
	ve=kzalloc(sizeof(struct vxlan_endpoint),GFP_ATOMIC);
	if(!ve)
		goto ret_flag;
	ve->src_ip=src_ip;
	ve->src_port=src_port;
	ve->jiffie_cnt=jiffies_64;
	ve->is_local_port=b_local?TRUE:FALSE;
	hlist_add_head_rcu(&ve->hnode,hhead);
	ret_flag:
	spin_unlock(&vsn->endpoint_hash_lock);
	return ve;
}

/*
* given a sk_buff,this function will forward it no matter unicast to any remote client or broadcast or deliever it to local stack
* make sure this would function is call with rcu_read_lock() protected
*/
int vxlan_server_forward_one(struct net*net,struct sk_buff*skb,int b_local,uint32_t dst_ip,uint16_t dst_port)
{
	struct flowi4 fl4;
	struct rtable*rt;
	struct vxlan_hdr*vxh;
	struct udphdr *udpp;
	struct iphdr *ipp;
	struct vxlan_server_net *vsn=net_generic(net,vxlan_server_id);
	printk("send one:%08x,%04x,%d\n",dst_ip,dst_port,b_local);
	//return -1;
	
	if(b_local){
		skb->protocol=eth_type_trans(skb,vsn->vsd.ndev);
		__skb_tunnel_rx(skb,vsn->vsd.ndev);
		skb_reset_network_header(skb);
		
		skb->encapsulation=0;
		skb->ip_summed=CHECKSUM_UNNECESSARY;
		skb->pkt_type=PACKET_HOST;
		netif_rx(skb);
	}else{
		
		skb_reset_inner_headers(skb);
		skb->encapsulation=1;
		if(skb_headroom(skb)<VXLAN_HEADER_ROOM)
			goto drop_flag;
		memset(&fl4,0x0,sizeof(struct flowi4));
		fl4.daddr=dst_ip;
		rt=ip_route_output_key(net,&fl4);
		if(IS_ERR(rt))
			goto drop_flag;
		if(rt->dst.dev==vsn->vsd.ndev)
			goto drop_flag;
		skb_dst_drop(skb);
		skb_dst_set(skb,&rt->dst);
		skb->dev=rt->dst.dev;
		skb->sk=vsn->socket->sk;
		
		/*try to stuff vxlan header*/
		vxh=(struct vxlan_hdr*)__skb_push(skb,sizeof(struct vxlan_hdr));
		vxh->flag=0x08;
		vxh->reserved1=0;
		vxh->reserved2=0;
		vxh->vni=TBYTE_SORDER(vsn->vni);
		/*try to stuff UDP header*/
		udpp=(struct udphdr*)__skb_push(skb,sizeof(struct udphdr));
		skb_reset_transport_header(skb);
		udpp->check=0;
		udpp->dest=dst_port;
		udpp->source=htons(glocal_port);
		udpp->len=htons(skb->len);
		
		/*try to stuff ip(IPv4 only) header*/
		ipp=(struct iphdr*)__skb_push(skb,sizeof(struct iphdr));
		memset(ipp,0x0,sizeof(struct iphdr));
		skb_reset_network_header(skb);
		

		ipp->version=4;
		ipp->ihl=5;
		ipp->tos=0;
		ipp->tot_len=htons(skb->len);
		__ip_select_ident(ipp,0);
		ipp->frag_off=0;
		ipp->ttl=0xff;
		ipp->protocol=IPPROTO_UDP;
		ipp->daddr=dst_ip;
		ipp->saddr=fl4.saddr;
		ipp->check=ip_fast_csum(ipp,ipp->ihl);

		skb->pkt_type=PACKET_OTHERHOST;
		
		nf_reset(skb);

		iptunnel_xmit(skb,rt->dst.dev);
		
	}

	
	return 0;

	drop_flag:
		kfree_skb(skb);
		return -1;
}
int vxlan_server_forward(struct net* net,struct sk_buff* skb,struct vxlan_endpoint*ve_src)
{

	struct vxlan_server_net *vsn=net_generic(net,vxlan_server_id);
	struct ethhdr *eh=eth_hdr(skb);
	int pkt_sent=FALSE;
	int idx=0;
	struct vxlan_endpoint *ve_dst;
	struct vxlan_client *vc_dst;
	struct sk_buff*skb_tmp;
	
	if(is_multicast_ether_addr(eh->h_dest)||is_broadcast_ether_addr(eh->h_dest)){
		/*populate all the client and send this packet to it*/
		
		for(idx=0;idx<ENDPOINT_HASH_SIZE;idx++)
			hlist_for_each_entry_rcu(ve_dst,&vsn->endpoint_hhead[idx],hnode){
			if(ve_dst==ve_src)
				continue;
			skb_tmp=skb_clone(skb,GFP_ATOMIC);
			if(!skb_tmp)/*not enough memory*/
				continue;
			/*forward it two the one endpoint*/
			
			vxlan_server_forward_one(net,skb_tmp,FALSE,ve_dst->src_ip,ve_dst->src_port);
			pkt_sent=TRUE;
		}
		
		/*additionally ,we could throw it to the  local client the source address is not equal to the dev_addr*/
		/*and this time ,we clearly know what,,,the packet will be delievered to kernel upper stack through netif_rx()*/
		
		if(compare_ether_addr(eh->h_source,vsn->vsd.ndev->dev_addr)){//notice here we will consume the skb here
			
			vxlan_server_forward_one(net,skb,TRUE,0,0);
			
			pkt_sent=TRUE;
		}else if(pkt_sent==TRUE){//discard the packet,because we do not need it any more
			kfree_skb(skb);
		}

		if(pkt_sent==FALSE){//send it with default ip and port in vsn
			
			vxlan_server_forward_one(net,skb,FALSE,vsn->default_ip,vsn->default_port);
		}

	}else{


		vc_dst=vxlan_server_find_client(net,eh->h_dest,TRUE,FALSE);
		if(vc_dst){//unicast to the dest found(even it's a local iface destination,the b_local flag will be set)
			
			vxlan_server_forward_one(net,skb,FALSE,vc_dst->src_ip,vc_dst->src_port);
		}else {//send it to default dst
			
			vxlan_server_forward_one(net,skb,FALSE,vsn->default_ip,vsn->default_port);
		}
	}	
	return 0;
}
/*
*make sure this function is protected by rcu_read_lock()
*or it will not work properly,maybe
*/

int vxlan_server_recv_snoop(struct net*net,uint8_t *mac,uint32_t src_ip,uint16_t src_port,struct vxlan_client**pvc,struct vxlan_endpoint**pve)
{
	*pvc=NULL;
	*pve=NULL;

	*pvc=vxlan_server_find_client(net,mac,TRUE,FALSE);
	if(!*pvc){
		*pvc=vxlan_server_create_client(net,mac,src_ip,src_port,FALSE,TRUE);
	}

	*pve=vxlan_server_find_endpoint(net,src_ip,src_port,TRUE,FALSE);
	if(!*pve){
		*pve=vxlan_server_create_endpoint(net,src_ip,src_port,FALSE,TRUE);
	}
	
	return 0;
}
/*annotation from net/ipv4/udp.c
1456                  * This is an encapsulation socket so pass the skb to
1457                  * the socket's udp_encap_rcv() hook. Otherwise, just
1458                  * fall through and pass this up the UDP socket.
1459                  * up->encap_rcv() returns the following value:
1460                  * =0 if skb was successfully passed to the encap
1461                  *    handler or was discarded by it.
1462                  * >0 if skb should be passed on to UDP.
1463                  * <0 if skb should be resubmitted as proto -N
1464                  */

int vxlan_server_udp_encap_recv(struct sock*sk,struct sk_buff *skb)
{
	/*even here,the outer UDP hdr is still inclued in data*/
	struct vxlan_server_net *vsn=net_generic(sk->__sk_common.skc_net,vxlan_server_id);
	struct vxlan_hdr *vxh;
	struct ethhdr *inner_eh;
	struct iphdr *outer_ipp;
	struct udphdr *outer_udpp;
	struct net*net;
	/*********************************/
	struct vxlan_client* vc;
	struct vxlan_endpoint *ve;
	/*******************************/
	int vni;
	net=sk->__sk_common.skc_net;

	
	__skb_pull(skb,sizeof(struct udphdr));
	
	if(!pskb_may_pull(skb,sizeof(struct vxlan_hdr)))
		goto up_flag;
	vxh=(struct vxlan_hdr*)skb->data;
	if(vxh->flag!=0x08||vxh->reserved1||vxh->reserved2)
		goto up_flag;
	vni=vxh->vni;
	vni=TBYTE_SORDER(vni);
	if(vni!=vsn->vni)
		goto up_flag;
	/*here we make sure this is a legal vxlan packet*/
	__skb_pull(skb,sizeof(struct vxlan_hdr));
	
	if(!pskb_may_pull(skb,sizeof(struct ethhdr)))
		goto drop_flag;

	/*reset mac header ,even this value is much bigger than ip and udp header*/
	skb_reset_mac_header(skb);
	

	inner_eh=eth_hdr(skb);
	outer_ipp=ip_hdr(skb);
	outer_udpp=udp_hdr(skb);
	printk("src mac:%02x:%02x:%02x:%02x:%02x:%02x\n",
		inner_eh->h_source[0],
		inner_eh->h_source[1],
		inner_eh->h_source[2],
		inner_eh->h_source[3],
		inner_eh->h_source[4],
		inner_eh->h_source[5]
		);
	
	/*filter illegal packet with multi or broad cast source address*/
	if(is_multicast_ether_addr(inner_eh->h_source)||is_broadcast_ether_addr(inner_eh->h_source))
		goto drop_flag;

	
	rcu_read_lock();
	
	vxlan_server_recv_snoop(net,inner_eh->h_source,outer_ipp->saddr,outer_udpp->source,&vc,&ve);
	
	vxlan_server_forward(net,skb,ve);
	rcu_read_unlock();
	
	//goto drop_flag;
	
	return 0;

	up_flag:
		__skb_push(skb,sizeof(struct udphdr));
		return 1;
	drop_flag:
		kfree_skb(skb);
		return 0;
}


int vxlan_dummy_dev_open(struct net_device*ndev)
{
	
	netif_start_queue(ndev);
	return 0;
}
int vxlan_dummy_dev_stop(struct net_device*ndev)
{
	netif_stop_queue(ndev);
	return 0;
}
netdev_tx_t vxlan_server_do_xmit(struct sk_buff*skb,struct net_device*ndev)
{
	struct net*net=skb->sk->__sk_common.skc_net;

	rcu_read_lock();
	vxlan_server_forward(net,skb,NULL);
	rcu_read_unlock();

	return NETDEV_TX_OK;
}
int vxlan_server_dev_set_mac (struct net_device *ndev,void*p)
{
	
	struct net* net=dev_net(ndev);
	struct vxlan_server_net*vsn=net_generic(net,vxlan_server_id);
	struct sockaddr* addr=p;
	if(netif_running(ndev))
		return -EBUSY;
	if(!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;
	/*try to find previous client ,and release it*/
	spin_lock(&vsn->client_hash_lock);
	//vc=vxlan_server_find_client(net,ndev->dev_addr,FALSE,FALSE);
	if(vsn->vsd.vc_local){
		hlist_del_rcu(&vsn->vsd.vc_local->hnode);
		call_rcu(&vsn->vsd.vc_local->rcu,vxlan_server_client_free);
	}
	spin_unlock(&vsn->client_hash_lock);
	vsn->vsd.vc_local=vxlan_server_create_client(net,addr->sa_data,0,0,TRUE,FALSE);
	if(!vsn->vsd.vc_local){
		printk("[vxlan_server] updating mac addree entry fails\n");
	}
	//printk("mac set:%02x,%02x\n",addr->sa_data[0],addr->sa_data[1]);
	memcpy(ndev->dev_addr,addr->sa_data,ETH_ALEN);
	return 0;
}
struct rtnl_link_stats64* vxlan_server_get_stats(struct net_device *dev, struct rtnl_link_stats64 *storage)
{
	memset(storage,0x0,sizeof(struct rtnl_link_stats64));
	return storage;
}
static const struct net_device_ops vxlan_netdev_ops = {

	.ndo_open = vxlan_dummy_dev_open,
	.ndo_stop = vxlan_dummy_dev_stop,
	.ndo_set_mac_address=vxlan_server_dev_set_mac,
	.ndo_change_mtu=eth_change_mtu,
	.ndo_start_xmit=vxlan_server_do_xmit,
	.ndo_get_stats64=vxlan_server_get_stats,

};

void vxlan_server_dev_getinfo(struct net_device*ndev,struct ethtool_drvinfo*info)
{
	strlcpy(info->driver, "vxlan_server", sizeof(info->driver));
}
static const struct ethtool_ops vxlan_netdev_ethtool_ops=
{
	.get_drvinfo=vxlan_server_dev_getinfo,
	.get_link=ethtool_op_get_link,
};
void vxlan_server_do_setup(struct net_device*netdev)
{

	ether_setup(netdev);
	netdev->netdev_ops=&vxlan_netdev_ops;
	netdev->ethtool_ops=&vxlan_netdev_ethtool_ops;
	netdev->features= NETIF_F_HIGHDMA| NETIF_F_ALL_CSUM  ;
	netdev->needed_headroom=14+20+8+8+14;
	eth_hw_addr_random(netdev);
	
}
int create_netdevice(struct net* net)
{
	int rc;
	struct vxlan_server_net *vsn=net_generic(net,vxlan_server_id);
	void ** ptr;
	vsn->vsd.ndev=alloc_netdev(sizeof(void*),"vxlan_server",vxlan_server_do_setup);
	if(!vsn->vsd.ndev){
		printk("[x]netdevice allocation fails\n");
		vsn->vsd.dev_ok=0;
		return -1;
	}
	ptr=netdev_priv(vsn->vsd.ndev);
	*ptr=vsn;//store the struct into prevate area of net device
	dev_net_set(vsn->vsd.ndev,net);
	rtnl_lock();
	rc=register_netdevice(vsn->vsd.ndev);
	if(rc){
		
		printk("[x]netdevice registration fails\n");
		vsn->vsd.dev_ok=0;
		rtnl_unlock();
		return -1;
	}
	dev_set_promiscuity(vsn->vsd.ndev,1);
	rtnl_unlock();
	vsn->vsd.dev_ok=1;
	vsn->vsd.ndev->tx_queue_len=0;
	/*start the dev queue*/
	netif_start_queue(vsn->vsd.ndev);
	
	return 0;
}
int close_netdevice(struct net*net)
{
	/*if netdev is registered successfully,unregister it*/
	struct vxlan_server_net*vsn=net_generic(net,vxlan_server_id);
	if(vsn->vsd.dev_ok){
		netif_stop_queue(vsn->vsd.ndev);
		rtnl_lock();
		dev_set_promiscuity(vsn->vsd.ndev,-1);
		unregister_netdevice(vsn->vsd.ndev);
		rtnl_unlock();
	}
	if(vsn->vsd.ndev){
		free_netdev(vsn->vsd.ndev);
	}
	return 0;
}

int vxlan_server_net_init(struct net * net)
{
	
	int idx;
	
	struct vxlan_server_net *vsn=net_generic(net,vxlan_server_id);
	struct sockaddr_in vxlan_server_addr={
		.sin_family=AF_INET,
		.sin_addr.s_addr=0,
		.sin_port=htons(glocal_port),
	};
	struct sock* sock;
	int rc;
	vsn->socket=NULL;
	/*create UDP socket in kernel and setup udp hook callback function*/
	rc=sock_create_kern(AF_INET,SOCK_DGRAM,IPPROTO_UDP,&vsn->socket);
	if(rc<0){
		printk(">>>udp socket create fails\n");
		return 0;
	}
	sock=vsn->socket->sk;
	sk_change_net(sock,net);

	rc=kernel_bind(vsn->socket,(struct sockaddr*)&vxlan_server_addr,sizeof(struct sockaddr_in));
	if(rc<0){
		vsn->socket=NULL;
		sk_release_kernel(sock);
		printk(">>>udp bind socket fails\n");
		return 0;
	}
	inet_sk(sock)->mc_loop = 0;
	udp_sk(sock)->encap_type=1;
	udp_sk(sock)->encap_rcv=vxlan_server_udp_encap_recv;
	udp_encap_enable();


	/*init some private data structure */
	spin_lock_init(&vsn->client_hash_lock);
	spin_lock_init(&vsn->endpoint_hash_lock);
	for(idx=0;idx<CLIENT_HASH_SIZE;idx++)
		INIT_HLIST_HEAD(&vsn->client_hhead[idx]);
	for(idx=0;idx<ENDPOINT_HASH_SIZE;idx++)
		INIT_HLIST_HEAD(&vsn->endpoint_hhead[idx]);
	/*create virtual network device here,only one device per net namespace will be created,
	*so this vxlan_server function may be limited somehow
	*/

	create_netdevice(net);
	vsn->vni=gvni;
	
	/*add this net priv structure into the gobal net list*/
	list_add_tail(&vsn->list,&gnet_list);

	/*here we add some local port fdb entry when netdevice is created*/
	if(vsn->vsd.dev_ok){
		vsn->vsd.vc_local=vxlan_server_create_client(net,vsn->vsd.ndev->dev_addr,0,0,TRUE,FALSE);
		if(!vsn->vsd.vc_local)
			printk("[vxlan_server]craeting local entry fails\n");
	}
	vsn->default_ip=0x828c170d;//limited broadcast address
	vsn->default_port=htons(glocal_port);
	
	return 0;
}

void vxlan_server_net_exit(struct net * net)
{

	struct vxlan_server_net *vsn=net_generic(net,vxlan_server_id);
	if(vsn->socket){
		sk_release_kernel(vsn->socket->sk);
		vsn->socket=NULL;
	}
	close_netdevice(net);

}
struct pernet_operations vxlan_server_net_ops=
{
	.id=&vxlan_server_id,
	.size=sizeof(struct vxlan_server_net),
	.init=vxlan_server_net_init,
	.exit=vxlan_server_net_exit,
};

int __init vxlan_server_init(void)
{
	
	int rc=0;
	INIT_LIST_HEAD(&gnet_list);
	rc=register_pernet_subsys(&vxlan_server_net_ops);
	printk("[r] register pernet subsystem :%s\n",rc?"fails":"succeeds");
	return 0;
}
void __exit vxlan_server_exit(void)
{
	
	unregister_pernet_subsys(&vxlan_server_net_ops);
	
}

module_init(vxlan_server_init);
module_exit(vxlan_server_exit);

MODULE_AUTHOR("jzheng from bjtu");
MODULE_LICENSE("GPL");
