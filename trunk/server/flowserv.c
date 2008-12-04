/*
 * file:	flowserv.c
 * purpose:	to sniff for packets using libpcap, and to chunk data together
 * 		and deliver it to somebody
 * created:	03-09-2007
 * creator:	rian shelley
 */

#include <stdio.h>
#include <pcap.h>
#include <features.h>
#include <sys/time.h>
#include "structs.h"
#include "mygettime.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include "../subnetpassword.h"
#include "../shared/flowdata.h"
#include "../shared/config.h"
#include "subnets.h"
#include "fwrules.h"
#include "globals.h"

/* the minimum elapsed time between packets, in milliseconds. this corresponds
 * to a sustainable 24 frames per second */
#define MINRATE 41



/* the snaplen. 80 bytes should be sufficient */
#define SNAPLEN 80



#define T_IP 0x0800
#define T_TCP 6
#define T_UDP 17


unsigned long long lastupdate = 0;

struct packetheader flowbuffer;
struct flowpacket *buffer;
struct subnetpacket* subnets;
struct packetheader cachedsubnets;
struct fwrule* firewallrules;
int numfwrules;
struct {
	unsigned int ip;
	unsigned short port;
} clients[MAXCLIENTS] = {{0,0}};
time_t timeouts[MAXCLIENTS] = {0};
int sendsock;
int numclients = 0;

void delclient(unsigned int ip, unsigned short port);

/*
 * function:	sendclients()
 * purpose:		to send some data to all of our clients
 * recieves:	a pointer to the data, and its length
 */
inline void sendclients(void* data, int dlen)
{
	int i;
	struct sockaddr_in sin;
	sin.sin_family = AF_INET;
	for (i = 0; i < MAXCLIENTS; i++) {
		if (clients[i].ip) {
			if (timeouts[i] < time(0)) {
				delclient(clients[i].ip, clients[i].port);
			} else {
				sin.sin_addr.s_addr = clients[i].ip;
				sin.sin_port = clients[i].port;
				int len = sendto(sendsock, data, 
						dlen, 
						0, (struct sockaddr*)&sin, sizeof(sin));

				if (len < 0) {
					printf("error writing %i bytes: %s\n",
						dlen, 
						strerror(errno));
				} 
			}
		}
	}
}

/*
 * function:	flushbuffer()
 * purpose:	to flush data to a client socket
 * recieves:	a client struct
 */
inline void flushbuffer() {
	sendclients(&flowbuffer, flowpacketsize(buffer) + SIZEOF_PACKETHEADER);
	lastupdate = gettime();
	buffer->count = 0;
}

/*
 * function:	report()
 * purpose:		to report a packet
 * recieves:	the source ip, the destination ip, the packet size, 
 * 				the packet type
 */
void report(unsigned int src, unsigned int dst, unsigned short size, enum packettype pt) {
	if ((src & localmask) == localip)
	{
		buffer->data[buffer->count].incoming = 0;
		buffer->data[buffer->count].ip = src & ~localmask;
	}
	else if ((dst & localmask) == localip)
	{
		buffer->data[buffer->count].incoming = 1;
		buffer->data[buffer->count].ip = dst & ~localmask;
	}
	else 
	{
		/* ignore the packet */
		return;
	}
	buffer->data[buffer->count].packet = (unsigned int)pt;
	buffer->data[buffer->count].packetsize = size;
/*
	printf("saw flow from xx.xx.%i.%i\n",
		(buffer.data[buffer.count].ip & 0x0000ff00) >> 8,
		 buffer.data[buffer.count].ip & 0x000000ff);
*/

	buffer->count++;
	if (buffer->count >= MAXINDEX || lastupdate + MINRATE < gettime())
		flushbuffer();
}

/*
 * function:	addclient()
 * purpose:		to add a new client
 * recieves:	the ip of a client, in network byte order
 * returns:		whether or not the client was added
 */
int addclient(unsigned int ip, unsigned short port)
{
	if (!port)
		port = localport; /* the default */
	int i;
	for (i = 0; i < MAXCLIENTS; i++)
	{
		if (clients[i].ip == ip && clients[i].port == port)
		{
			timeouts[i] = time(0) + TIMEOUT;
			return 1;
		}
	}
	for (i = 0; i < MAXCLIENTS; i++)
	{
		if (!clients[i].ip)
		{
			printf("added a client\n");
			clients[i].ip = ip;
			clients[i].port = port;
			timeouts[i] = time(0) + TIMEOUT; 
			numclients++;
			return 1;
		} 
	}
	return 0;
}

/*
 * function:	delclient()
 * purpose:		to delete a client
 * recieves:	the ip of a client, in network byte order
 */
void delclient(unsigned int ip, unsigned short port)
{
	int i;
	if (port == 0)
		port = localport;
	for (i = 0; i < MAXCLIENTS; i++)
	{
		if (clients[i].ip == ip && clients[i].port == port)
		{
			printf("removed a client\n");
			clients[i].ip = 0;
			numclients--;
			break;
		}
	}
}

/*
 * function:	initnetworkbyhost()
 * purpose:		to set up a socket to stream to a client
 * recieves:	a string representing the person who wants the data
 * returns:		success or failure
 */
int initnetworkbyhost(const char* host)
{
	/* see if we have room */
	if (numclients >= MAXCLIENTS)
		return 0;

	struct hostent * he = gethostbyname(host);
	if (he == 0) 
		return 0;
	
	//memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
	return addclient(*(int*)he->h_addr_list[0], 0);
}

/* 
 * function:	srvlisten()
 * purpose:		to return a socket that is listening for connections
 * returns:		a socket. didn't i just say that?
 */
int srvlisten()
{
	int s;
	struct sockaddr_in addr;

	memset(&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(localport);

	addr.sin_addr.s_addr = INADDR_ANY;

	s = socket(AF_INET, SOCK_DGRAM, 0);

	if (bind(s, (struct sockaddr*)&addr, sizeof(addr)))
	{
		close(s);
		return 0;
	}
	return s;
}

/*
 * this function will send the firewall rules to a given client
 */
void sendrules(int sock, struct sockaddr* address, socklen_t fromlen)
{
	int count = 0;
	struct fwrule* tmp = firewallrules;
	struct packetheader ph;
	ph.version = VERSION;
	ph.packettype = PKT_FWRULE;
	ph.reserved = 0;
	while (tmp)
	{
		int size = writerulepacket(ph.data, count++, numfwrules, tmp->rule);
		tmp = tmp->next;
		sendto(sock, &ph, SIZEOF_PACKETHEADER+size, 0, address, fromlen);
	}
}

/*
 * function:	checklisten()
 * purpose:		to check the socket for a client wishing data
 * recieves:	listening socket
 */
void checklisten(int listen)
{
	struct sockaddr_in addr;
	struct flowrequest *fr;
	struct subnetpacket* sp;
	struct packetheader ph;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(localport);
	addr.sin_addr.s_addr = INADDR_ANY;
	socklen_t fromlen = sizeof(addr);
	int err;
	if ((err = recvfrom(listen, &ph, sizeof(ph), MSG_DONTWAIT, 
		(struct sockaddr*)&addr, &fromlen)) > 0)
	{
		switch (ph.packettype)
		{
		case PKT_FLOW:
			fr = (struct flowrequest*)ph.data;
			if (fr->flowon) 
				addclient(addr.sin_addr.s_addr, addr.sin_port);
			else 
				delclient(addr.sin_addr.s_addr, addr.sin_port);
			break;
		case PKT_SUBNET:
			printf("Recieved a subnet request\n");
			sp = (struct subnetpacket*)ph.data;
			subnets->requestnum = sp->requestnum;
			sendto(listen, &cachedsubnets, subnetpacketsize(subnets) + SIZEOF_PACKETHEADER, 0, (struct sockaddr*)&addr, fromlen);
			printf("Sent subnet information\n");
			break;
		case PKT_FIREWALL:
			/* printf("Recieved a firewall packet\n"); */
			sendclients(&ph, err);
			break;
		case PKT_FWRULE:
			printf("Recieved a request for firewall rules\n");
			addr.sin_port = htons(localport);
			sendrules(listen, (struct sockaddr*)&addr, fromlen);
			break;
		}
	}
	else 
	{
	/*	printf("\t%s\n", strerror(errno)); */
	}
}

/*
 * function:	main
 * purpose:	to set stuff up and get things rolling. duh. 
 */
int main(int argc, char* argv[])
{
	config_loadfile("ipvisualizer.conf");
	config_loadargs(argc, argv);
	/* these are loaded from the config, so must happen afterwards */
	initglobals();
	char ebuff[PCAP_ERRBUF_SIZE];
	subnet subarray[MAXINDEX];
	unsigned int i, j;
	subnets = (struct subnetpacket*)cachedsubnets.data;
	cachedsubnets.version = VERSION;
	cachedsubnets.packettype = PKT_SUBNET;
	
	unsigned int numsubnets = getsubnets(subarray, MAXINDEX, config_string(CONFIG_SUBNET), config_string(CONFIG_AUTH));
	if (numsubnets > MAXINDEX) 
		numsubnets = MAXINDEX;
	j = 0;
	for (i = 0; i < numsubnets; i++) {
		/* only report the subnets if they are within our range */
		if ((subarray[i].base & localmask) == localip) {
			/*mask off the subnets according to our local mask, since /16 is limit*/
			subnets->subnets[j].base = subarray[i].base & ~localmask;
			subnets->subnets[j].mask = subarray[i].mask;
			j++;
		}
	}
	subnets->count = j;
	subnets->base = localip;
	subnets->mask = localmask;
	printf("Recieved information about %i subnets\n", numsubnets);
/*
	firewallrules = getfwrules("singsing.usu.edu", "/admin/firewall/currentrules.php?f=1", fwruleauthorization, &numfwrules);

	printf("Recieved information about %i firewall rules\n", numfwrules);
*/

	/* set up our buffers */
	buffer = (struct flowpacket*)flowbuffer.data;
	buffer->base = localip;
	buffer->mask = masktocidr(localmask);

	/* open up the sockets */
	/*
	for (i = 2; i < argc; i++) 
	{
		if (!initnetworkbyhost(argv[i]))
			printf("Error binding to %s: %s\n", argv[i], strerror(errno));
	}
	*/

	pcap_t* handle = pcap_open_live(config_string(CONFIG_INTERFACE), SNAPLEN, 1, MINRATE, ebuff);

	if (!handle) {
		fprintf(stderr, "Couldn't sniff on device %s: %s\n", 
			argv[1], ebuff);
		return 2;
	}

	struct pcap_pkthdr* header;
	const u_char* packet;
	const struct sniff_ethernet *ethernet; /* The ethernet header */
	const struct sniff_ip *ip; /* The IP header */


	/* start listening for clients */
	int listensock;
	if (!(listensock = srvlisten()))
		return -1; /* die if we can't listen */
	/* create a socket we can send with */
	sendsock = socket(AF_INET, SOCK_DGRAM, 0);


	for(;;) {
		int result;
		if(1 == (result = pcap_next_ex(handle, &header, &packet))) {
			ethernet = (struct sniff_ethernet*)(packet);
			/* verify that the packet is an ip packet */
			if (ntohs(ethernet->ether_type) == T_IP) {
				ip = (struct sniff_ip*)(packet + SIZE_ETHERNET);
				/* grab the protocol */
				enum packettype pt;
				if (ip->ip_p == T_TCP)
					pt = TCP;
				else if (ip->ip_p == T_UDP)
					pt = UDP;
				else
					pt = OTHER;
				report(ntohl(ip->ip_src), ntohl(ip->ip_dst), header->len, pt);
			} else {
			}

		} else if (result == 0) {
			/* supposed to be if the timeout expires, but may not need this */
		}
		/* do other misc stuff here */
		/* like check for udp packets */
		checklisten(listensock);
	}

	pcap_close(handle);
	freefwrules(firewallrules);

	return 0;
}
