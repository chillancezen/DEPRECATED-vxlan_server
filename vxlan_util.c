#include "vxlan_server.h"

/*how does this will contributes bug,I ccan not figure out*/
int is_mac_addr_equal(uint8_t mac1[],uint8_t mac2[])
{
	int idx=0;
	for(;idx<6;idx++)
		if(mac1[idx]!=mac2[idx])
			return 0;
	return 1;
}
int copy_mac_addr(uint8_t mac1[],const uint8_t mac2[])
{
	memcpy(mac1,mac2,6);
	return 0;
}
