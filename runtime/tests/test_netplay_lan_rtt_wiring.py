"""Run the production LAN probe protocol and launcher latency callback."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_netplay_load_apply_flow import function_body


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    args = parser.parse_args()
    runtime = Path(__file__).resolve().parents[1]
    source = (runtime/'src/main.cpp').read_text(encoding='utf-8')
    key = function_body(source, 'static uint64_t ae_np_lan_rtt_key(')
    probe = function_body(source, 'static void ae_np_lan_rtt_probe(')
    pump = function_body(source, 'static void ae_np_lan_udp_pump(')
    receive = pump[pump.index('            if (std::strncmp(buf, "MOTK7 PING'):]
    receive = receive[:receive.index('            if (std::strncmp(buf, "MOTK6 MODS')]
    member = function_body(source, 'int ae_np_member_get(')
    member = member[member.index('if (ae_np_use_lan_members())'):]
    latency = member[member.index('                    out->latency_ms = -1;'):]
    latency = latency[:latency.index('                    if (out->is_local)')]
    # The exercised code must actually feed the room, including lifecycle edges.
    assert 'ae_np_lan_rtt_probe(slot, g_lnch_lan_peers[slot], now)' in pump
    assert 'ae_np_lan_rtt_probe(g_lnch_remote_lan_state.host_slot, to, now)' in pump
    assert 'netplay_lan_rtt_bind(&g_lnch_lan_rtt[slot], 0)' in function_body(source, 'static void ae_np_lan_clear_peer_slot(')
    assert 'netplay_lan_rtt_bind(&g_lnch_lan_rtt[i], 0)' in function_body(source, 'static void ae_np_lan_udp_close(')
    assert 'std::swap(g_lnch_lan_rtt[a], g_lnch_lan_rtt[b])' in function_body(source, 'static bool ae_np_lan_swap_seats(')
    assert 'std::swap(g_lnch_lan_rtt[from_slot], g_lnch_lan_rtt[to_slot])' in function_body(source, 'int ae_np_move_member(')
    fixture = r'''
#include "netplay_lan_rtt.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
struct in_addr { unsigned s_addr; };
struct sockaddr_in { in_addr sin_addr; unsigned short sin_port; };
static constexpr int AF_INET=2, kAeLanMaxSlots=4;
static int ntohs(unsigned short p) { return p; }
static int inet_pton(int, const char* host, in_addr* out) {
    out->s_addr=(unsigned)std::strtoul(host,nullptr,10); return out->s_addr != 0;
}
static bool ae_np_lan_endpoint_host(const std::string& endpoint,char* out,size_t cap) {
    std::snprintf(out,cap,"%s",endpoint.substr(0,endpoint.find(':')).c_str());return true;
}
static int ae_np_lan_endpoint_port(const std::string& endpoint) {
    return std::atoi(endpoint.c_str()+endpoint.find(':')+1);
}
static bool g_lnch_hosting_lan, g_lnch_remote_lan;
static std::string g_lnch_lan_endpoint="1:7000";
static sockaddr_in g_lnch_lan_peers[4];
static bool g_lnch_lan_peer_ok[4];
static NetplayLanRtt g_lnch_lan_rtt[4];
static struct {int host_slot=2;} g_lnch_remote_lan_state;
static uint32_t now_ms;
static uint32_t SDL_GetTicks() {return now_ms;}
static std::string sent;
static void ae_np_lan_udp_sendto(const sockaddr_in&, const char* text) {sent=text;}
''' + 'static uint64_t ae_np_lan_rtt_key(const sockaddr_in& address) {' + key + '}\n' + \
        'static void ae_np_lan_rtt_probe(int slot,const sockaddr_in& to,uint32_t now) {' + probe + '}\n' + \
        'static void receive_packet(const char* text,const sockaddr_in& from) {char buf[1024];std::snprintf(buf,sizeof(buf),"%s",text);for(int once=0;once<1;++once){' + receive + '}}\n' + \
        'static int member_latency(int slot,int local) {struct {int is_local,latency_ms;} entry{local,-7};auto* out=&entry;auto state=g_lnch_remote_lan_state;' + latency + 'return out->latency_ms;}\n' + r'''
int main() {
    const sockaddr_in host{{1},7000},guest{{2},8000},stranger{{3},8000};
    g_lnch_hosting_lan=true; g_lnch_lan_peers[3]=guest; g_lnch_lan_peer_ok[3]=true;
    ae_np_lan_rtt_probe(3,guest,100);
    assert(sent=="MOTK7 PING\n100\n");
    const std::string ping=sent; sent.clear();
    g_lnch_hosting_lan=false;g_lnch_remote_lan=true;
    receive_packet(ping.c_str(),stranger); assert(sent.empty());
    receive_packet(ping.c_str(),host); assert(sent=="MOTK7 PONG\n100\n");
    const std::string pong=sent;
    g_lnch_hosting_lan=true;g_lnch_remote_lan=false;now_ms=300;
    receive_packet(pong.c_str(),stranger); assert(member_latency(3,0)==-1);
    receive_packet("MOTK7 PONG\n99\n",guest);assert(member_latency(3,0)==-1);
    receive_packet(pong.c_str(),guest);assert(member_latency(3,0)==200);
    assert(member_latency(3,1)==-1); // self latency stays blank
    assert(member_latency(1,0)==0);  // same-machine file participant
    // Both sides measure; the guest attributes its host even after a seat move.
    g_lnch_hosting_lan=false;g_lnch_remote_lan=true;
    ae_np_lan_rtt_probe(2,host,1000);const std::string guest_ping=sent;
    g_lnch_hosting_lan=true;g_lnch_remote_lan=false;
    receive_packet(guest_ping.c_str(),guest);const std::string host_pong=sent;
    g_lnch_hosting_lan=false;g_lnch_remote_lan=true;now_ms=1180;
    receive_packet(host_pong.c_str(),host);assert(member_latency(2,0)==180);
    assert(member_latency(1,0)==-1); // another guest has no direct path
    for(const char* invalid:{"MOTK7 PING\n\n","MOTK7 PING\n4294967296\n",
                            "MOTK7 PING\n1junk\n","MOTK7 PING\n1\nextra"}) {
        sent.clear();receive_packet(invalid,host);assert(sent.empty());
    }
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='psx-lan-rtt-') as temporary:
        path=Path(temporary)/'probe.cpp';binary=Path(temporary)/'probe.exe'
        path.write_text(fixture)
        subprocess.run([args.compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                        '-I',str(runtime/'include'),str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('production LAN host/guest probes, source ownership, seats and UI latency: PASS')


if __name__=='__main__':
    main()
