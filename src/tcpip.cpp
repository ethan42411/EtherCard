// IP, ARP, UDP and TCP functions.
// Author: Guido Socher
// Copyright: GPL V2
//
// The TCP implementation uses some size optimisations which are valid
// only if all data can be sent in one single packet. This is however
// not a big limitation for a microcontroller as you will anyhow use
// small web-pages. The web server must send the entire web page in one
// packet. The client "web browser" as implemented here can also receive
// large pages.
//
// 2010-05-20 <jc@wippler.nl>

#include "EtherCard.h"
#include "net.h"
#include "EtherUtil.h"
#undef word // arduino nonsense

#define ICMP_PING_PAYLOAD_PATTERN 0x42
#define ICMP_PING_PAYLOAD_SIZE 56

// Avoid spurious pgmspace warnings - http://forum.jeelabs.net/node/327
// See also http://gcc.gnu.org/bugzilla/show_bug.cgi?id=34734
//#undef PROGMEM
//#define PROGMEM __attribute__(( section(".progmem.data") ))
//#undef PSTR
//#define PSTR(s) (__extension__({static prog_char c[] PROGMEM = (s); &c[0];}))

#define TCP_STATE_SENDSYN       1
#define TCP_STATE_SYNSENT       2
#define TCP_STATE_ESTABLISHED   3
#define TCP_STATE_NOTUSED       4
#define TCP_STATE_CLOSING       5
#define TCP_STATE_CLOSED        6

#define TCPCLIENT_SRC_PORT_H 11 //Source port (MSB) for TCP/IP client connections - hardcode all TCP/IP client connection from ports in range 2816-3071
static uint8_t tcpclient_src_port_l=1; // Source port (LSB) for tcp/ip client connections - increments on each TCP/IP request
static uint8_t tcp_fd; // a file descriptor, will be encoded into the port
static uint8_t tcp_client_state; //TCP connection state: 1=Send SYN, 2=SYN sent awaiting SYN+ACK, 3=Established, 4=Not used, 5=Closing, 6=Closed
static uint8_t tcp_client_port_h; // Destination port (MSB) of TCP/IP client connection
static uint8_t tcp_client_port_l; // Destination port (LSB) of TCP/IP client connection
static uint8_t (*client_tcp_result_cb)(uint8_t,uint8_t,uint16_t,uint16_t); // Pointer to callback function to handle response to current TCP/IP request
static uint16_t (*client_tcp_datafill_cb)(uint8_t); //Pointer to callback function to handle payload data in response to current TCP/IP request
static uint8_t www_fd; // ID of current http request (only one http request at a time - one of the 8 possible concurrent TCP/IP connections)
static void (*client_browser_cb)(uint8_t,uint16_t,uint16_t); // Pointer to callback function to handle result of current HTTP request
static const char *client_additionalheaderline; // Pointer to c-string additional http request header info
static const char *client_postval;
static const char *client_urlbuf; // Pointer to c-string path part of HTTP request URL
static const char *client_urlbuf_var; // Pointer to c-string filename part of HTTP request URL
static const char *client_hoststr; // Pointer to c-string hostname of current HTTP request
static IcmpCallback icmp_cb; // Pointer to callback function for ICMP ECHO response handler (triggers when localhost receives ping response (pong))

static uint16_t info_data_len; // Length of TCP/IP payload
static uint8_t seqnum = 0xa; // My initial tcp sequence number
static uint8_t result_fd = 123; // Session id of last reply
static const char* result_ptr; // Pointer to TCP/IP data
static unsigned long SEQ; // TCP/IP sequence number

#define CLIENTMSS 550
#define TCP_DATA_START ((uint16_t)TCP_SRC_PORT_H_P+(gPB[TCP_HEADER_LEN_P]>>4)*4) // Get offset of TCP/IP payload data

const unsigned char ntpreqhdr[] PROGMEM = { 0xE3,0,4,0xFA,0,1,0,0,0,1 }; //NTP request header
extern const uint8_t allOnes[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; // Used for hardware (MAC) and IP broadcast addresses

// static void log_data(const char *message, const uint8_t *ptr, uint16_t len)
// {
//     Serial.print(message);
//     Serial.println(':');
//     for (const uint8_t *last = ptr + len; ptr != last; ++ptr)
//     {
//         Serial.print(*ptr, HEX);
//         Serial.print(' ');
//     }
//     Serial.println();
// }

static void fill_checksum(uint16_t &checksum, const uint8_t *ptr, uint16_t len, uint8_t type) {
    uint32_t sum = type==1 ? IP_PROTO_UDP_V+len-8 :
                   type==2 ? IP_PROTO_TCP_V+len-8 : 0;
    while(len >1) {
        sum += (uint16_t) (((uint32_t)*ptr<<8)|*(ptr+1));
        ptr+=2;
        len-=2;
    }
    if (len)
        sum += ((uint32_t)*ptr)<<8;
    while (sum>>16)
        sum = (uint16_t) sum + (sum >> 16);
    uint16_t ck = ~ (uint16_t) sum;
    checksum = HTONS(ck);
}

static void fill_checksum(uint8_t dest, uint8_t off, uint16_t len, uint8_t type) {
    uint8_t *iter = gPB;
    const uint8_t* ptr = iter + off;
    uint16_t &checksum = *(uint16_t *)(iter + dest);
    fill_checksum(checksum, ptr, len, type);
}

static boolean is_lan(const uint8_t source[IP_LEN], const uint8_t destination[IP_LEN]);

static void init_eth_header(const uint8_t *thaddr)
{
    EthHeader &h = ethernet_header();
    EtherCard::copyMac(h.thaddr, thaddr);
    EtherCard::copyMac(h.shaddr, EtherCard::mymac);
}

static void init_eth_header(
        const uint8_t *thaddr,
        const uint16_t etype
    )
{
    init_eth_header(thaddr);
    ethernet_header().etype = etype;
}

// check if ARP request is ongoing
static bool client_arp_waiting(const uint8_t *ip)
{
    const uint8_t *mac = EtherCard::arpStoreGetMac(is_lan(EtherCard::myip, ip) ? ip : EtherCard::gwip);
    return !mac || memcmp(mac, allOnes, ETH_LEN) == 0;
}

// check if ARP is request is done
static bool client_arp_ready(const uint8_t *ip)
{
    const uint8_t *mac = EtherCard::arpStoreGetMac(ip);
    return mac && memcmp(mac, allOnes, ETH_LEN) != 0;
}

// return
//  - IP MAC address if IP is part of LAN
//  - gwip MAC address if IP is outside of LAN
//  - broadcast MAC address if none are found
static const uint8_t *client_arp_get(const uint8_t *ip)
{
    // see http://tldp.org/HOWTO/Multicast-HOWTO-2.html
    // multicast or broadcast address, https://github.com/njh/EtherCard/issues/59
    const uint8_t *mac;
    if (
            (ip[0] & 0xF0) == 0xE0
            || *((uint32_t *) ip) == 0xFFFFFFFF
            || !memcmp(EtherCard::broadcastip, ip, IP_LEN)
            || (mac = EtherCard::arpStoreGetMac(is_lan(EtherCard::myip, ip) ? ip : EtherCard::gwip)) == NULL
        )
        return allOnes;

    return mac;
}

static void init_ip_header(
        const uint8_t *dst
    )
{
    IpHeader &iph = ip_header();
    EtherCard::copyIp(iph.tpaddr, dst);
    EtherCard::copyIp(iph.spaddr, EtherCard::myip);
    iph.flags(IP_DF);
    iph.fragmentOffset(0);
    iph.ttl = 64;
}

static uint8_t check_ip_message_is_from(const IpHeader &iph, const uint8_t *ip) {
    return memcmp(iph.spaddr, ip, IP_LEN) == 0;
}

static boolean is_lan(const uint8_t source[IP_LEN], const uint8_t destination[IP_LEN]) {
    if(source[0] == 0 || destination[0] == 0) {
        return false;
    }
    for(int i = 0; i < IP_LEN; i++)
        if((source[i] & EtherCard::netmask[i]) != (destination[i] & EtherCard::netmask[i])) {
            return false;
        }
    return true;
}

static uint8_t is_my_ip(const IpHeader &iph) {
    return iph.version() == IP_V4 && iph.ihl() == IP_IHL &&
           (memcmp(iph.tpaddr, EtherCard::myip, IP_LEN) == 0  //not my IP
            || (memcmp(iph.tpaddr, EtherCard::broadcastip, IP_LEN) == 0) //not subnet broadcast
            || (memcmp(iph.tpaddr, allOnes, IP_LEN) == 0)); //not global broadcasts
    //!@todo Handle multicast
}

static void fill_ip_hdr_checksum(IpHeader &iph) {
    iph.hchecksum = 0;
    fill_checksum(iph.hchecksum, (const uint8_t *)&iph, sizeof(IpHeader), 0);
}

static void make_eth_ip_reply(const uint16_t payloadlen = 0xFFFF) {
    init_eth_header(ethernet_header().shaddr);
    init_ip_header(ip_header().spaddr);

    IpHeader &iph = ip_header();
    if (payloadlen != 0xFFFF)
        htons(iph.totalLen, ip_payload() - (uint8_t *)&iph + payloadlen);

    fill_ip_hdr_checksum(iph);
}

static void step_seq(uint16_t rel_ack_num,uint8_t cp_seq) {
    uint8_t i;
    uint8_t tseq;
    i = 4;
    while(i>0) {
        rel_ack_num = gPB[TCP_SEQ_H_P+i-1]+rel_ack_num;
        tseq = gPB[TCP_SEQACK_H_P+i-1];
        gPB[TCP_SEQACK_H_P+i-1] = rel_ack_num;
        if (cp_seq)
            gPB[TCP_SEQ_H_P+i-1] = tseq;
        else
            gPB[TCP_SEQ_H_P+i-1] = 0; // some preset value
        rel_ack_num = rel_ack_num>>8;
        i--;
    }
}

static void make_tcphead(uint16_t rel_ack_num,uint8_t cp_seq) {
    uint8_t i = gPB[TCP_DST_PORT_H_P];
    gPB[TCP_DST_PORT_H_P] = gPB[TCP_SRC_PORT_H_P];
    gPB[TCP_SRC_PORT_H_P] = i;
    uint8_t j = gPB[TCP_DST_PORT_L_P];
    gPB[TCP_DST_PORT_L_P] = gPB[TCP_SRC_PORT_L_P];
    gPB[TCP_SRC_PORT_L_P] = j;
    step_seq(rel_ack_num,cp_seq);
    gPB[TCP_CHECKSUM_H_P] = 0;
    gPB[TCP_CHECKSUM_L_P] = 0;
    gPB[TCP_HEADER_LEN_P] = 0x50;
}

static void make_arp_answer_from_request() {
    // set ethernet layer mac addresses
    init_eth_header(arp_header().shaddr);

    // set ARP answer from the request buffer
    ArpHeader &arp = arp_header();
    arp.opcode = ETH_ARP_OPCODE_REPLY;
    EtherCard::copyMac(arp.thaddr, arp.shaddr);
    EtherCard::copyMac(arp.shaddr, EtherCard::mymac);
    EtherCard::copyIp(arp.tpaddr, arp.spaddr);
    EtherCard::copyIp(arp.spaddr, EtherCard::myip);

    // send ethernet frame
    EtherCard::packetSend((uint8_t *)&arp + sizeof(ArpHeader) - gPB); // 42
}

static void make_echo_reply_from_request(uint16_t len) {
    make_eth_ip_reply();
    IcmpHeader &ih = icmp_header();
    ih.type = ICMP_TYPE_ECHOREPLY_V;
    ih.checksum = ih.checksum + 0x08;
    EtherCard::packetSend(len);
}

void EtherCard::makeUdpReply (const char *data,uint8_t datalen,uint16_t port) {
    if (datalen>220)
        datalen = 220;

    make_eth_ip_reply(sizeof(UdpHeader) + datalen);
    IpHeader &iph = ip_header();
    UdpHeader &udph = udp_header();
    udph.dport = udph.sport;
    htons(udph.sport, port);
    htons(udph.length, sizeof(UdpHeader)+datalen);
    udph.checksum = 0;
    memcpy(udp_payload(), data, datalen);
    fill_checksum(udph.checksum, (const uint8_t *)&iph.spaddr, 16 + datalen,1);
    packetSend(udp_payload() - gPB + datalen);
}

static void make_tcp_synack_from_syn() {
    make_eth_ip_reply(TCP_HEADER_LEN_PLAIN+4);
    gPB[TCP_FLAGS_P] = TCP_FLAGS_SYNACK_V;
    make_tcphead(1,0);
    gPB[TCP_SEQ_H_P+0] = 0;
    gPB[TCP_SEQ_H_P+1] = 0;
    gPB[TCP_SEQ_H_P+2] = seqnum;
    gPB[TCP_SEQ_H_P+3] = 0;
    seqnum += 3;
    gPB[TCP_OPTIONS_P] = 2;
    gPB[TCP_OPTIONS_P+1] = 4;
    gPB[TCP_OPTIONS_P+2] = 0x05;
    gPB[TCP_OPTIONS_P+3] = 0x0;
    gPB[TCP_HEADER_LEN_P] = 0x60;
    gPB[TCP_WIN_SIZE] = 0x5; // 1400=0x578
    gPB[TCP_WIN_SIZE+1] = 0x78;
    fill_checksum(TCP_CHECKSUM_H_P, (uint8_t *)&ip_header().spaddr - gPB, 8+TCP_HEADER_LEN_PLAIN+4,2);
    EtherCard::packetSend(tcp_header() - gPB + TCP_HEADER_LEN_PLAIN+4);
}

uint16_t EtherCard::getTcpPayloadLength() {
    int16_t i = ntohs(ip_header().totalLen) - sizeof(IpHeader);
    i -= (gPB[TCP_HEADER_LEN_P]>>4)*4; // generate len in bytes;
    if (i<=0)
        i = 0;
    return (uint16_t)i;
}

static void make_tcp_ack_from_any(int16_t datlentoack,uint8_t addflags) {
    gPB[TCP_FLAGS_P] = TCP_FLAGS_ACK_V|addflags;
    if (addflags!=TCP_FLAGS_RST_V && datlentoack==0)
        datlentoack = 1;
    make_tcphead(datlentoack,1); // no options
    make_eth_ip_reply(TCP_HEADER_LEN_PLAIN);
    gPB[TCP_WIN_SIZE] = 0x4; // 1024=0x400, 1280=0x500 2048=0x800 768=0x300
    gPB[TCP_WIN_SIZE+1] = 0;
    fill_checksum(TCP_CHECKSUM_H_P, (uint8_t *)&ip_header().spaddr - gPB, 8+TCP_HEADER_LEN_PLAIN,2);
    EtherCard::packetSend(tcp_header() - gPB + TCP_HEADER_LEN_PLAIN);
}

static void make_tcp_ack_with_data_noflags(uint16_t dlen) {
    IpHeader &iph = ip_header();
    htons(iph.totalLen, ip_payload() - (uint8_t *)&ip_header() + TCP_HEADER_LEN_PLAIN + dlen);
    fill_ip_hdr_checksum(iph);
    gPB[TCP_CHECKSUM_H_P] = 0;
    gPB[TCP_CHECKSUM_L_P] = 0;
    fill_checksum(TCP_CHECKSUM_H_P, (uint8_t *)&ip_header().spaddr - gPB, 8+TCP_HEADER_LEN_PLAIN+dlen,2);
    EtherCard::packetSend(tcp_header() - gPB + TCP_HEADER_LEN_PLAIN + dlen);
}

void EtherCard::httpServerReply (uint16_t dlen) {
    make_tcp_ack_from_any(info_data_len,0); // send ack for http get
    gPB[TCP_FLAGS_P] = TCP_FLAGS_ACK_V|TCP_FLAGS_PUSH_V|TCP_FLAGS_FIN_V;
    make_tcp_ack_with_data_noflags(dlen); // send data
}

static void setSequenceNumber(uint32_t seq) {
    gPB[TCP_SEQ_H_P]   = (seq & 0xff000000 ) >> 24;
    gPB[TCP_SEQ_H_P+1] = (seq & 0xff0000 ) >> 16;
    gPB[TCP_SEQ_H_P+2] = (seq & 0xff00 ) >> 8;
    gPB[TCP_SEQ_H_P+3] = (seq & 0xff );
}

uint32_t EtherCard::getSequenceNumber() {
    return ntohl(*(uint32_t *)(gPB + TCP_SEQ_H_P));
}

void EtherCard::httpServerReplyAck () {
    make_tcp_ack_from_any(getTcpPayloadLength(), 0); // send ack for http request
    SEQ = getSequenceNumber(); //get the sequence number of packets after an ack from GET
}

void EtherCard::httpServerReply_with_flags (uint16_t dlen , uint8_t flags) {
    setSequenceNumber(SEQ);
    gPB[TCP_FLAGS_P] = flags; // final packet
    make_tcp_ack_with_data_noflags(dlen); // send data
    SEQ=SEQ+dlen;
}

// initialize ethernet frame and IP header
static IpHeader &init_ip_frame(
        const uint8_t *destip,
        const uint8_t protocol
    )
{
    init_eth_header(client_arp_get(destip), ETHTYPE_IP_V);
    init_ip_header(destip);
    IpHeader &iph = ip_header();
    iph.version(IP_V4);
    iph.ihl(IP_IHL);
    iph.dscpEcn = 0;
    iph.identification = 0x0;
    iph.protocol = protocol;
    return iph;
}

void EtherCard::clientIcmpRequest(const uint8_t *destip) {
    IpHeader &iph = init_ip_frame(destip, IP_PROTO_ICMP_V);
    iph.totalLen = HTONS(0x54);
    fill_ip_hdr_checksum(iph);
    IcmpHeader &ih = icmp_header();
    ih.type = ICMP_TYPE_ECHOREQUEST_V;
    ih.code = 0;
    ih.checksum = 0;
    ih.ping.identifier = HTONS(0x0500 | EtherCard::myip[3]);
    ih.ping.sequence = HTONS(1);
    memset(icmp_payload(), ICMP_PING_PAYLOAD_PATTERN, ICMP_PING_PAYLOAD_SIZE);
    fill_checksum(ih.checksum, (const uint8_t *)&ih, sizeof(IcmpHeader) + ICMP_PING_PAYLOAD_SIZE, 0);
    packetSend(icmp_payload() - gPB + ICMP_PING_PAYLOAD_SIZE);
}

void EtherCard::ntpRequest (uint8_t *ntpip, uint8_t srcport) {
    IpHeader &iph = init_ip_frame(ntpip, IP_PROTO_UDP_V);
    iph.totalLen = HTONS(0x4c);
    fill_ip_hdr_checksum(iph);
    UdpHeader &udph = udp_header();
    uint8_t *udpp = udp_payload();
    udph.dport = HTONS(NTP_PORT);
    udph.sport = HTONS((10 << 8) | srcport);
    udph.length = HTONS(56);
    udph.checksum = 0;
    memset(udpp, 0, 48);
    memcpy_P(udpp,ntpreqhdr,10);
    fill_checksum(udph.checksum, (const uint8_t *)&iph.spaddr, 16 + 48, 1);
    packetSend(90);
}

uint8_t EtherCard::ntpProcessAnswer (uint32_t *time,uint8_t dstport_l) {
    UdpHeader &udph = udp_header();
    if ((dstport_l && (ntohs(udph.dport) & 0xFF) != dstport_l) || udph.length != HTONS(56)
            || udph.sport != HTONS(NTP_PORT))
        return 0;
    ((uint8_t*) time)[3] = gPB[0x52];
    ((uint8_t*) time)[2] = gPB[0x53];
    ((uint8_t*) time)[1] = gPB[0x54];
    ((uint8_t*) time)[0] = gPB[0x55];
    return 1;
}

void EtherCard::udpPrepare (uint16_t sport, const uint8_t *dip, uint16_t dport) {
    init_ip_frame(dip, IP_PROTO_UDP_V);
    UdpHeader &udph = udp_header();
    htons(udph.dport, dport);
    htons(udph.sport, sport);
    udph.length = 0;
    udph.checksum = 0;
}

void EtherCard::udpTransmit (uint16_t datalen) {
    IpHeader &iph = ip_header();
    htons(iph.totalLen, sizeof(IpHeader) + sizeof(UdpHeader) + datalen);
    fill_ip_hdr_checksum(iph);

    UdpHeader &udph = udp_header();
    htons(udph.length, sizeof(UdpHeader) + datalen);
    fill_checksum(udph.checksum, (const uint8_t *)&iph.spaddr, 16 + datalen,1);
    packetSend(udp_payload() - gPB + datalen);
}

void EtherCard::sendUdp (const char *data, uint8_t datalen, uint16_t sport,
                         const uint8_t *dip, uint16_t dport) {
    udpPrepare(sport, dip, dport);
    if (datalen>220)
        datalen = 220;
    memcpy(udp_payload(), data, datalen);
    udpTransmit(datalen);
}

void EtherCard::sendWol (uint8_t *wolmac) {
    udpPrepare(0x1042, allOnes, 9);
    uint8_t *pos = udp_payload();
    copyMac(pos, allOnes);
    pos += 6;
    for (uint8_t m = 0; m < 16; ++m, pos += 6) {
        copyMac(pos, wolmac);
    }
    udpTransmit(6 + 16*6);
}

// make a arp request
static void client_arp_whohas(const uint8_t *ip_we_search) {
    // set ethernet layer mac addresses
    init_eth_header(allOnes, ETHTYPE_ARP_V);

    ArpHeader &arp = arp_header();
    arp.htype = ETH_ARP_HTYPE_ETHERNET;
    arp.ptype = ETH_ARP_PTYPE_IPV4;
    arp.hlen = ETH_LEN;
    arp.plen = IP_LEN;
    arp.opcode = ETH_ARP_OPCODE_REQ;
    EtherCard::copyMac(arp.shaddr, EtherCard::mymac);
    EtherCard::copyIp(arp.spaddr, EtherCard::myip);
    memset(arp.thaddr, 0, sizeof(arp.thaddr));
    EtherCard::copyIp(arp.tpaddr, ip_we_search);

    // send ethernet frame
    EtherCard::packetSend((uint8_t *)&arp - gPB + sizeof(ArpHeader));

    // mark ip as "waiting" using broadcast mac address
    EtherCard::arpStoreSet(ip_we_search, allOnes);
}

static void client_arp_refresh(const uint8_t *ip)
{
    // Check every 65536 (no-packet) cycles whether we need to retry ARP requests
    if (is_lan(EtherCard::myip, ip) && (!EtherCard::arpStoreHasMac(ip) || EtherCard::delaycnt == 0))
        client_arp_whohas(ip);
}

void EtherCard::clientResolveIp(const uint8_t *ip)
{
    client_arp_refresh(ip);
}

uint8_t EtherCard::clientWaitIp(const uint8_t *ip)
{
    return client_arp_waiting(ip);
}

uint8_t EtherCard::clientWaitingGw () {
    return clientWaitIp(gwip);
}

uint8_t EtherCard::clientWaitingDns () {
    return clientWaitIp(dnsip);
}

void EtherCard::setGwIp (const uint8_t *gwipaddr) {
    if (memcmp(gwipaddr, gwip, IP_LEN) != 0)
        arpStoreInvalidIp(gwip);
    copyIp(gwip, gwipaddr);
}

void EtherCard::updateBroadcastAddress()
{
    for(uint8_t i=0; i<IP_LEN; i++)
        broadcastip[i] = myip[i] | ~netmask[i];
}

static void client_syn(uint8_t srcport,uint8_t dstport_h,uint8_t dstport_l) {
    IpHeader &iph = init_ip_frame(EtherCard::hisip, IP_PROTO_TCP_V);
    iph.totalLen = HTONS(44); // good for syn
    fill_ip_hdr_checksum(iph);
    gPB[TCP_DST_PORT_H_P] = dstport_h;
    gPB[TCP_DST_PORT_L_P] = dstport_l;
    gPB[TCP_SRC_PORT_H_P] = TCPCLIENT_SRC_PORT_H;
    gPB[TCP_SRC_PORT_L_P] = srcport; // lower 8 bit of src port
    memset(gPB + TCP_SEQ_H_P, 0, 8);
    gPB[TCP_SEQ_H_P+2] = seqnum;
    seqnum += 3;
    gPB[TCP_HEADER_LEN_P] = 0x60; // 0x60=24 len: (0x60>>4) * 4
    gPB[TCP_FLAGS_P] = TCP_FLAGS_SYN_V;
    gPB[TCP_WIN_SIZE] = 0x3; // 1024 = 0x400 768 = 0x300, initial window
    gPB[TCP_WIN_SIZE+1] = 0x0;
    gPB[TCP_CHECKSUM_H_P] = 0;
    gPB[TCP_CHECKSUM_L_P] = 0;
    gPB[TCP_CHECKSUM_L_P+1] = 0;
    gPB[TCP_CHECKSUM_L_P+2] = 0;
    gPB[TCP_OPTIONS_P] = 2;
    gPB[TCP_OPTIONS_P+1] = 4;
    gPB[TCP_OPTIONS_P+2] = (CLIENTMSS>>8);
    gPB[TCP_OPTIONS_P+3] = (uint8_t) CLIENTMSS;
    fill_checksum(TCP_CHECKSUM_H_P, (uint8_t *)&iph.spaddr - gPB, 8 + TCP_HEADER_LEN_PLAIN + 4, 2);
    // 4 is the tcp mss option:
    EtherCard::packetSend(tcp_header() - gPB + TCP_HEADER_LEN_PLAIN + 4);
}

uint8_t EtherCard::clientTcpReq (uint8_t (*result_cb)(uint8_t,uint8_t,uint16_t,uint16_t),
                                 uint16_t (*datafill_cb)(uint8_t),uint16_t port) {
    client_tcp_result_cb = result_cb;
    client_tcp_datafill_cb = datafill_cb;
    tcp_client_port_h = port>>8;
    tcp_client_port_l = port;
    tcp_client_state = TCP_STATE_SENDSYN; // Flag to packetloop to initiate a TCP/IP session by send a syn
    tcp_fd = (tcp_fd + 1) & 7;
    return tcp_fd;
}

static uint16_t www_client_internal_datafill_cb(uint8_t fd) {
    BufferFiller bfill = EtherCard::tcpOffset();
    if (fd==www_fd) {
        if (client_postval == 0) {
            bfill.emit_p(PSTR("GET $F$S HTTP/1.0\r\n"
                              "Host: $F\r\n"
                              "$F\r\n"
                              "\r\n"), client_urlbuf,
                         client_urlbuf_var,
                         client_hoststr, client_additionalheaderline);
        } else {
            const char* ahl = client_additionalheaderline;
            bfill.emit_p(PSTR("POST $F HTTP/1.0\r\n"
                              "Host: $F\r\n"
                              "$F$S"
                              "Accept: */*\r\n"
                              "Content-Length: $D\r\n"
                              "Content-Type: application/x-www-form-urlencoded\r\n"
                              "\r\n"
                              "$S"), client_urlbuf,
                         client_hoststr,
                         ahl != 0 ? ahl : PSTR(""),
                         ahl != 0 ? "\r\n" : "",
                         strlen(client_postval),
                         client_postval);
        }
    }
    return bfill.position();
}

static uint8_t www_client_internal_result_cb(uint8_t fd, uint8_t statuscode, uint16_t datapos, uint16_t len_of_data) {
    if (fd!=www_fd)
        (*client_browser_cb)(4,0,0);
    else if (statuscode==0 && len_of_data>12 && client_browser_cb) {
        uint8_t f = strncmp("200",(char *)&(gPB[datapos+9]),3) != 0;
        (*client_browser_cb)(f, ((uint16_t)TCP_SRC_PORT_H_P+(gPB[TCP_HEADER_LEN_P]>>4)*4),len_of_data);
    }
    return 0;
}

void EtherCard::browseUrl (const char *urlbuf, const char *urlbuf_varpart, const char *hoststr, void (*callback)(uint8_t,uint16_t,uint16_t)) {
    browseUrl(urlbuf, urlbuf_varpart, hoststr, PSTR("Accept: text/html"), callback);
}

void EtherCard::browseUrl (const char *urlbuf, const char *urlbuf_varpart, const char *hoststr, const char *additionalheaderline, void (*callback)(uint8_t,uint16_t,uint16_t)) {
    client_urlbuf = urlbuf;
    client_urlbuf_var = urlbuf_varpart;
    client_hoststr = hoststr;
    client_additionalheaderline = additionalheaderline;
    client_postval = 0;
    client_browser_cb = callback;
    www_fd = clientTcpReq(&www_client_internal_result_cb,&www_client_internal_datafill_cb,hisport);
}

void EtherCard::httpPost (const char *urlbuf, const char *hoststr, const char *additionalheaderline, const char *postval, void (*callback)(uint8_t,uint16_t,uint16_t)) {
    client_urlbuf = urlbuf;
    client_hoststr = hoststr;
    client_additionalheaderline = additionalheaderline;
    client_postval = postval;
    client_browser_cb = callback;
    www_fd = clientTcpReq(&www_client_internal_result_cb,&www_client_internal_datafill_cb,hisport);
}

static uint16_t tcp_datafill_cb(uint8_t /* fd */) {
    uint16_t len = Stash::length();
    Stash::extract(0, len, EtherCard::tcpOffset());
    Stash::cleanup();
    EtherCard::tcpOffset()[len] = 0;
#if SERIAL
    Serial.print("REQUEST: ");
    Serial.println(len);
    Serial.println((char*) EtherCard::tcpOffset());
#endif
    result_fd = 123; // bogus value
    return len;
}

static uint8_t tcp_result_cb(uint8_t fd, uint8_t status, uint16_t datapos, uint16_t /* datalen */) {
    if (status == 0) {
        result_fd = fd; // a valid result has been received, remember its session id
        result_ptr = (char*) ether.buffer + datapos;
    }
    return 1;
}

uint8_t EtherCard::tcpSend () {
    www_fd = clientTcpReq(&tcp_result_cb, &tcp_datafill_cb, hisport);
    return www_fd;
}

const char* EtherCard::tcpReply (uint8_t fd) {
    if (result_fd != fd)
        return 0;
    result_fd = 123; // set to a bogus value to prevent future match
    return result_ptr;
}

void EtherCard::registerPingCallback (const IcmpCallback callback) {
    icmp_cb = callback;
}

uint8_t EtherCard::packetLoopIcmpCheckReply (const uint8_t *ip_monitoredhost) {
    const IpHeader &iph = ip_header();
    return iph.protocol == IP_PROTO_ICMP_V &&
           icmp_header().type == ICMP_TYPE_ECHOREPLY_V &&
           icmp_payload()[0] == ICMP_PING_PAYLOAD_PATTERN &&
           check_ip_message_is_from(iph, ip_monitoredhost);
}

uint16_t EtherCard::accept(const uint16_t port, uint16_t plen) {
    uint16_t pos;

    if (gPB[TCP_DST_PORT_H_P] == (port >> 8) &&
            gPB[TCP_DST_PORT_L_P] == ((uint8_t) port))
    {   //Packet targeted at specified port
        if (gPB[TCP_FLAGS_P] & TCP_FLAGS_SYN_V)
            make_tcp_synack_from_syn(); //send SYN+ACK
        else if (gPB[TCP_FLAGS_P] & TCP_FLAGS_ACK_V)
        {   //This is an acknowledgement to our SYN+ACK so let's start processing that payload
            info_data_len = getTcpPayloadLength();
            if (info_data_len > 0)
            {   //Got some data
                pos = TCP_DATA_START; // TCP_DATA_START is a formula
                //!@todo no idea what this check pos<=plen-8 does; changed this to pos<=plen as otw. perfectly valid tcp packets are ignored; still if anybody has any idea please leave a comment
                if (pos <= plen)
                    return pos;
            }
            else if (gPB[TCP_FLAGS_P] & TCP_FLAGS_FIN_V)
                make_tcp_ack_from_any(0, 0); //No data so close connection
        }
    }
    return 0;
}

void EtherCard::packetLoopArp(const uint8_t *first, const uint8_t *last)
{
    // security: check if received data has expected size, only htype
    // "ethernet" and ptype "IPv4" is supported for the moment.
    // '<' and not '==' because Ethernet II require padding if ethernet frame
    // size is less than 60 bytes includes Ethernet II header
    if ((uint8_t)(last - first) < sizeof(ArpHeader))
        return;

    const ArpHeader &arp = *(const ArpHeader *)first;

    // check hardware type is "ethernet"
    if (arp.htype != ETH_ARP_HTYPE_ETHERNET)
        return;

    // check protocol type is "IPv4"
    if (arp.ptype != ETH_ARP_PTYPE_IPV4)
        return;

    // security: assert lengths are correct
    if (arp.hlen != ETH_LEN || arp.plen != IP_LEN)
        return;

    // ignore if not for us
    if (memcmp(arp.tpaddr, myip, IP_LEN) != 0)
        return;

    // add sender to cache...
    arpStoreSet(arp.spaddr, arp.shaddr);

    if (arp.opcode == ETH_ARP_OPCODE_REQ)
    {
        // ...and answer to sender
        make_arp_answer_from_request();
    }
}

void EtherCard::packetLoopIdle()
{
    if (isLinkUp())
    {
        client_arp_refresh(gwip);
        client_arp_refresh(dnsip);
        client_arp_refresh(hisip);
    }
    delaycnt++;

#if ETHERCARD_TCPCLIENT
    //Initiate TCP/IP session if pending
    if (tcp_client_state==TCP_STATE_SENDSYN && client_arp_ready(gwip)) { // send a syn
        tcp_client_state = TCP_STATE_SYNSENT;
        tcpclient_src_port_l++; // allocate a new port
        client_syn(((tcp_fd<<5) | (0x1f & tcpclient_src_port_l)),tcp_client_port_h,tcp_client_port_l);
    }
#endif
}

uint16_t EtherCard::packetLoop (uint16_t plen) {
    uint16_t len;

#if ETHERCARD_DHCP
    if(using_dhcp) {
        ether.DhcpStateMachine(plen);
    }
#endif

    if (plen < sizeof(EthHeader)) {
        packetLoopIdle();
        return 0;
    }

    const uint8_t *iter = gPB;
    const uint8_t *last = gPB + plen;
    const EthHeader &eh = ethernet_header();
    iter += sizeof(EthHeader);

    // log_data("packetLoop", gPB, plen);

    // arp payload
    if (eh.etype == ETHTYPE_ARP_V)
    {
        packetLoopArp(iter, last);
        return 0;
    }

    if (eh.etype != ETHTYPE_IP_V)
    {   //Not IP so ignoring
        return 0;
    }

    if ((uint8_t)(last - iter) < sizeof(IpHeader))
    {   // not enough data for IP packet
        return 0;
    }

    const IpHeader &iph = ip_header();
    iter += sizeof(IpHeader);

    if (is_my_ip(iph)==0)
        return 0;

    // refresh arp store
    if (memcmp(eh.thaddr, mymac, ETH_LEN) == 0)
        arpStoreSet(is_lan(myip, iph.spaddr) ? iph.spaddr : gwip, eh.shaddr);

#if ETHERCARD_ICMP
    if (iph.protocol == IP_PROTO_ICMP_V)
    {
        const IcmpHeader &ih = icmp_header();
        if (ih.type == ICMP_TYPE_ECHOREQUEST_V)
        {   //Service ICMP echo request (ping)
            if (icmp_cb)
                (*icmp_cb)(iph.spaddr);
            make_echo_reply_from_request(plen);
        }
        return 0;
    }
#endif
#if ETHERCARD_UDPSERVER
    if (ether.udpServerListening() && iph.protocol ==IP_PROTO_UDP_V)
    {   //Call UDP server handler (callback) if one is defined for this packet
        if(ether.udpServerHasProcessedPacket(iph, iter, last))
            return 0; //An UDP server handler (callback) has processed this packet
    }
#endif

    if (plen<54 || iph.protocol != IP_PROTO_TCP_V)
        return 0; //from here on we are only interested in TCP-packets; these are longer than 54 bytes

#if ETHERCARD_TCPCLIENT
    if (gPB[TCP_DST_PORT_H_P]==TCPCLIENT_SRC_PORT_H)
    {   //Source port is in range reserved (by EtherCard) for client TCP/IP connections
        if (check_ip_message_is_from(iph, hisip)==0)
            return 0; //Not current TCP/IP connection (only handle one at a time)
        if (gPB[TCP_FLAGS_P] & TCP_FLAGS_RST_V)
        {   //TCP reset flagged
            if (client_tcp_result_cb)
                (*client_tcp_result_cb)((gPB[TCP_DST_PORT_L_P]>>5)&0x7,3,0,0);
            tcp_client_state = TCP_STATE_CLOSING;
            return 0;
        }
        len = getTcpPayloadLength();
        if (tcp_client_state==TCP_STATE_SYNSENT)
        {   //Waiting for SYN-ACK
            if ((gPB[TCP_FLAGS_P] & TCP_FLAGS_SYN_V) && (gPB[TCP_FLAGS_P] &TCP_FLAGS_ACK_V))
            {   //SYN and ACK flags set so this is an acknowledgement to our SYN
                make_tcp_ack_from_any(0,0);
                gPB[TCP_FLAGS_P] = TCP_FLAGS_ACK_V|TCP_FLAGS_PUSH_V;
                if (client_tcp_datafill_cb)
                    len = (*client_tcp_datafill_cb)((gPB[TCP_SRC_PORT_L_P]>>5)&0x7);
                else
                    len = 0;
                tcp_client_state = TCP_STATE_ESTABLISHED;
                make_tcp_ack_with_data_noflags(len);
            }
            else
            {   //Expecting SYN+ACK so reset and resend SYN
                tcp_client_state = TCP_STATE_SENDSYN; // retry
                len++;
                if (gPB[TCP_FLAGS_P] & TCP_FLAGS_ACK_V)
                    len = 0;
                make_tcp_ack_from_any(len,TCP_FLAGS_RST_V);
            }
            return 0;
        }
        if (tcp_client_state==TCP_STATE_ESTABLISHED && len>0)
        {   //TCP connection established so read data
            if (client_tcp_result_cb) {
                uint16_t tcpstart = TCP_DATA_START; // TCP_DATA_START is a formula
                if (tcpstart>plen-8)
                    tcpstart = plen-8; // dummy but save
                uint16_t save_len = len;
                if (tcpstart+len>plen)
                    save_len = plen-tcpstart;
                (*client_tcp_result_cb)((gPB[TCP_DST_PORT_L_P]>>5)&0x7,0,tcpstart,save_len); //Call TCP handler (callback) function

                if(persist_tcp_connection)
                {   //Keep connection alive by sending ACK
                    make_tcp_ack_from_any(len,TCP_FLAGS_PUSH_V);
                }
                else
                {   //Close connection
                    make_tcp_ack_from_any(len,TCP_FLAGS_PUSH_V|TCP_FLAGS_FIN_V);
                    tcp_client_state = TCP_STATE_CLOSED;
                }
                return 0;
            }
        }
        if (tcp_client_state != TCP_STATE_CLOSING)
        {   //
            if (gPB[TCP_FLAGS_P] & TCP_FLAGS_FIN_V) {
                if(tcp_client_state == TCP_STATE_ESTABLISHED) {
                    return 0; // In some instances FIN is received *before* DATA.  If that is the case, we just return here and keep looking for the data packet
                }
                make_tcp_ack_from_any(len+1,TCP_FLAGS_PUSH_V|TCP_FLAGS_FIN_V);
                tcp_client_state = TCP_STATE_CLOSED; // connection terminated
            } else if (len>0) {
                make_tcp_ack_from_any(len,0);
            }
        }
        return 0;
    }
#endif

#if ETHERCARD_TCPSERVER
    //If we are here then this is a TCP/IP packet targeted at us and not related to our client connection so accept
    return accept(hisport, plen);
#endif
}

void EtherCard::persistTcpConnection(bool persist) {
    persist_tcp_connection = persist;
}
