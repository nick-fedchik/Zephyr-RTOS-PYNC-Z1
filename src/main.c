#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/hostname.h>
#include <zephyr/sys/printk.h>

static struct net_mgmt_event_callback mgmt_cb;

static void start_dhcpv4_client(struct net_if *iface, void *user_data)
{
	ARG_UNUSED(user_data);

	printk("Starting DHCPv4 on %s (hostname=%s)...\n",
	       net_if_get_device(iface)->name, net_hostname_get());
	net_dhcpv4_start(iface);
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event, struct net_if *iface)
{
	struct net_if_config *cfg = net_if_get_config(iface);

	ARG_UNUSED(cb);

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		char addr_buf[NET_IPV4_ADDR_LEN];
		char mask_buf[NET_IPV4_ADDR_LEN];
		char gw_buf[NET_IPV4_ADDR_LEN];

		if (cfg->ip.ipv4->unicast[i].ipv4.addr_type != NET_ADDR_DHCP) {
			continue;
		}

		net_addr_ntop(NET_AF_INET,
			      &cfg->ip.ipv4->unicast[i].ipv4.address.in_addr,
			      addr_buf, sizeof(addr_buf));
		net_addr_ntop(NET_AF_INET, &cfg->ip.ipv4->unicast[i].netmask,
			      mask_buf, sizeof(mask_buf));
		net_addr_ntop(NET_AF_INET, &cfg->ip.ipv4->gw,
			      gw_buf, sizeof(gw_buf));

		printk("DHCPv4 lease acquired: IP=%s mask=%s gw=%s\n",
		       addr_buf, mask_buf, gw_buf);
	}
}

int main(void)
{
	printk("Hello from Zephyr on PYNQ-Z1\n");

	net_mgmt_init_event_callback(&mgmt_cb, ipv4_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&mgmt_cb);
	net_if_foreach(start_dhcpv4_client, NULL);

	return 0;
}
