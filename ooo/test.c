#include <time.h>
#include <math.h>
#include <signal.h>
#include <assert.h> 
#include <sys/mman.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include "agent.h"

#define SERVER_PORT "12345"
#define IP_ADDR "10.0.1.102"


struct sockaddr_in addr;
struct conn_context ctx;


int main(int argc, char **argv) {
    // 1. 클라이언트와 서버 연결
    if (argc == 1) {
        // server
        debug_print("server mode\n");
        // call back 함수 수정해야 함
        // 서버임을 구분하기 위해 NULL
        init_rdma_agent(NULL, mrs, num_mrs, MAX_BUFFER, app_conn_cb_fn, app_disc_cb_fn, app_recv_cb_fn);
    }
    else if (argc > 1) {
        debug_print("client mode\n");

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(SERVER_PORT);
        addr.sin_addr.s_addr = inet_addr(IP_ADDR);
        
        // getaddrinfo, rdma_create_id, rdma_resolve_addr
        add_connection(argv[1], SERVER_PORT, 1, 1, 1);
        init_rdma_agent(SERVER_PORT, mrs, num_mrs, MAX_BUFFER, app_conn_cb_fn, app_disc_cb_fn, app_recv_cb_fn);
    }
  
    return 0;
}