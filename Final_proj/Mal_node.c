#include "contiki.h"
#include "net/rime/rime.h"
#include "lib/list.h"
#include "lib/memb.h"
#include "lib/random.h"
#include "dev/button-sensor.h"
#include "dev/leds.h"

#include <stdio.h>

/*------------------------- DECLARATIONS -------------------------*/

/* MACROS */

#define CHANNEL 135
#define NEIGHBOR_TIMEOUT 10 * CLOCK_SECOND
#define MAX_NEIGHBORS 16
#define MAT 50
// minimum delay in seconds
#define MINIMUM_DELAY 5
#define DEFAULT_DELAY 1

/* STRUCTS */
// a node in the neighbor list
struct neighbor {
  struct neighbor *next;
  linkaddr_t addr;
  int trust;
  struct ctimer ctimer;
  // value in seconds
  long unsigned int last_received;
};
// the struct sent over broadcast
struct neighbor_trust
{
  linkaddr_t addr;
  int trust;
};

/* UTILITY FUNCTIONS */
// check if an address is trusted
static int addr_is_blocked(const linkaddr_t* a);
static void update_table(void* _nt);
// called when a neighbor's ctimer runs out and reduecs its trust value
static void remove_neighbor(void* _n);
/* MULTIHOP FUNCTIONS */
// called when a multihop message is received (only at the target address)
// decreases trust value if messages received too frequently
static void recv(struct multihop_conn* c, const linkaddr_t* sender,
  const linkaddr_t* prevhop, uint8_t hops);
// multihop forward
// forwards to a random neighbor
static linkaddr_t* forward(struct multihop_conn* c, const linkaddr_t* originator, 
  const linkaddr_t* dest, const linkaddr_t* prevhop, uint8_t hops);
/* BROADCAST FUNCTIONS */
// called when a broadcast message is received
// adds the sender to neighbor list if not already present
static void broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from);

/* PROCESS REGISTRATION */
// sends multihop messages to 1.0 via neighbors
PROCESS(multihop_process, "multihop process");
// shares trust table with neighbors
PROCESS(broadcast_process, "broadcast process");
AUTOSTART_PROCESSES(&multihop_process, &broadcast_process);

/* GLOBAL VARIABLES */
// neighbor list
LIST(neighbor_table);
// one member in the neighbor list
MEMB(neighbor_mem, struct neighbor, MAX_NEIGHBORS);
// the sink node's address
static linkaddr_t sink_addr;
// multi hop callbackfunctions
static const struct multihop_callbacks multihop_call = {recv, forward};
// multi hop connection
static struct multihop_conn multihop;
// broadcast callback functions
static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
// broadcast connection
static struct broadcast_conn broadcast;
/*---------------------------------------------------------------------------*/
/*------------------------- DEFINITIONS -------------------------*/

/* PROCESS THREADS */

// multihop process
// sends data to 1.0 every 2 seconds
PROCESS_THREAD(multihop_process, ev, data)
{
  static struct etimer et;

  PROCESS_EXITHANDLER(multihop_close(&multihop);)
  
  PROCESS_BEGIN();

  sink_addr.u8[0] = 1;
  sink_addr.u8[1] = 0;

  /* Initialize the memory for the neighbor table entries. */
  memb_init(&neighbor_mem);

  /* Initialize the list used for the neighbor table. */
  list_init(neighbor_table);

  /* Open a multihop connection on Rime channel CHANNEL. */
  multihop_open(&multihop, CHANNEL, &multihop_call);

  while(1) {
    etimer_set(&et, DEFAULT_DELAY * CLOCK_SECOND);

    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    packetbuf_copyfrom("Hello", 6);

    if(!linkaddr_cmp(&linkaddr_node_addr, &sink_addr))
    {
      multihop_send(&multihop, &sink_addr);
      printf("Sending multihop message to 1.0\n");
    }

  }

  PROCESS_END();
}
// broadcast process
// shares trust table with neighbors
PROCESS_THREAD(broadcast_process, ev, data)
{
  struct neighbor_trust nt[MAX_NEIGHBORS] = {};
  static struct etimer et;
  struct neighbor* n;
  int i;
  PROCESS_EXITHANDLER(broadcast_close(&broadcast));
  PROCESS_BEGIN();
  broadcast_open(&broadcast, 129, &broadcast_call);
  while(1)
  {
    etimer_set(&et, 1 * CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    for(n = list_head(neighbor_table), i = 0; n != NULL; n = n->next, i++)
    {
      nt[i].addr = n->addr;
      nt[i].trust = n->trust;
    }
    packetbuf_copyfrom(nt, sizeof(nt));
    broadcast_send(&broadcast);

  }
  PROCESS_END();
}

/* EVENT HANDLERS */

static void recv(struct multihop_conn* c, const linkaddr_t* sender,
  const linkaddr_t* prevhop, uint8_t hops)
{
  struct neighbor* e;
  if(addr_is_blocked(sender))
  {
    printf("Message from untrusted neighbor %d.%d, ignored\n",
      sender->u8[0], sender->u8[1]
    );
    return;
  }

  printf("multihop message from %d.%d received '%s'\n", 
    sender->u8[0], sender->u8[1], (char *)packetbuf_dataptr()
  );

  for(e = list_head(neighbor_table); e != NULL; e = e->next)
  {
    if(linkaddr_cmp(sender, &e->addr))
    {
      ctimer_set(&e->ctimer, NEIGHBOR_TIMEOUT, remove_neighbor, e);
    }
  }
}

static linkaddr_t* forward(struct multihop_conn* c, const linkaddr_t* originator, 
  const linkaddr_t* dest, const linkaddr_t* prevhop, uint8_t hops)
{
  /* Find a random neighbor to send to. */
  int num, i;
  struct neighbor *n;

  if(!linkaddr_cmp(prevhop, &linkaddr_node_addr) && addr_is_blocked(prevhop))
  {
    printf("packet from blocked neighbor %d.%d, dropped\n",
      prevhop->u8[0], prevhop->u8[1]
    );
    return NULL;
  }

  for(n = list_head(neighbor_table); n != NULL; n = n->next)
  {
    if(linkaddr_cmp(prevhop, &n->addr))
    {
      ctimer_set(&n->ctimer, NEIGHBOR_TIMEOUT, remove_neighbor, n);
      if(clock_seconds() - n->last_received < MINIMUM_DELAY && n->trust > 49)
        n->trust *= 0.99;
      n->last_received = clock_seconds();
    }
  }

  if(list_length(neighbor_table) > 0) {
    num = random_rand() % list_length(neighbor_table);
    i = 0;
    for(n = list_head(neighbor_table); n != NULL && i != num; n = n->next) {
      ++i;
    }
    if(n != NULL) {
      printf("%d.%d: Forwarding packet to %d.%d (%d in list), hops %d\n",
	     linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1],
	     n->addr.u8[0], n->addr.u8[1], num,
	     packetbuf_attr(PACKETBUF_ATTR_HOPS));
      return &n->addr;
    }
  }
  printf("%d.%d: did not find a neighbor to foward to\n",
	 linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);
  return NULL;
}
/*
static void broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
  struct neighbor_trust nt[MAX_NEIGHBORS] = {};
  int i;
  struct neighbor* e;
  
  packetbuf_copyto(nt);
  printf("Broadcast from %d.%d: ", from->u8[0], from->u8[1]);
  for(i = 0; i < MAX_NEIGHBORS; i++)
  {
    printf("%d.%d %d | ", nt[i].addr.u8[0], nt[i].addr.u8[1], nt[i].trust);
  }
  printf("\n");

  for(e = list_head(neighbor_table); e != NULL; e = e->next) {
    if(linkaddr_cmp(from, &e->addr)) {
      ctimer_set(&e->ctimer, NEIGHBOR_TIMEOUT, remove_neighbor, e);
      return;
    }
  }
  e = memb_alloc(&neighbor_mem);
  if(e != NULL) {
    linkaddr_copy(&e->addr, from);
    list_add(neighbor_table, e);
    ctimer_set(&e->ctimer, NEIGHBOR_TIMEOUT, remove_neighbor, e);
    e->trust = 100;
    e->last_received = clock_seconds();
  }
}*/
static void broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
  struct neighbor_trust nt[MAX_NEIGHBORS] = {};
  struct neighbor* e;
  printf("Broadcast from %d.%d \n", from->u8[0], from->u8[1]);
  for(e = list_head(neighbor_table); e != NULL; e = e->next) {
    if(linkaddr_cmp(from, &e->addr)) {
	if(e->trust<50){
		return;	
	}
	else{
		packetbuf_copyto(nt);
  		update_table(&nt);
		return;
	}     
    }
  }  
  e = memb_alloc(&neighbor_mem); 
  if(e != NULL) {
    linkaddr_copy(&e->addr, from);
    list_add(neighbor_table, e);
    e->trust = 100;
    e->last_received = clock_seconds();
    ctimer_set(&e->ctimer, NEIGHBOR_TIMEOUT, remove_neighbor, e);
  }
  packetbuf_copyto(nt);
  update_table(&nt);
}
/* HELPER FUNCTIONS */

static void update_table(void* _nt){
  struct neighbor_trust *nt=_nt;
  int i;
  struct neighbor* e;
  printf("received neighbor trusts: ");
  for(i = 0; i < MAX_NEIGHBORS; i++){
   if(nt[i].trust==0)
	break;
   printf("%d.%d %d ", nt[i].addr.u8[0], nt[i].addr.u8[1], nt[i].trust);
   for(e = list_head(neighbor_table); e != NULL; e = e->next) {
    if(linkaddr_cmp(&nt[i].addr, &e->addr)) {
	if(nt[i].trust!=e->trust){
   	e->trust=(e->trust+nt[i].trust)/2;
		}    	
    }
    if(linkaddr_cmp(&e->addr, &sink_addr))
      e->trust = 100;
  }
 }
  printf("\nown neighbor trusts: ");
  for(e = list_head(neighbor_table); e != NULL; e = e->next) {
    printf(" %d.%d %d | ", e->addr.u8[0], e->addr.u8[1], e->trust);
  }
  printf("\n");
}

static int addr_is_blocked(const linkaddr_t* a)
{
  struct neighbor* n;
  for(n = list_head(neighbor_table); n != NULL; n = n->next)
  {
    if(linkaddr_cmp(a, &n->addr) && n->trust < MAT)
    {
      return 1;
    }
  }
  return 0;
}

static void remove_neighbor(void* _n)
{
  struct neighbor *n = _n;
  if(linkaddr_cmp(&sink_addr, &n->addr))
    return;

  if (n->trust >= MAT)
  {
    //n->trust *= 0.99;
    ctimer_set(&n->ctimer, NEIGHBOR_TIMEOUT, remove_neighbor, n);
    return;
  }
  else
    printf("Trust of %d.%d fell below 50\n", n->addr.u8[0], n->addr.u8[1]);
}





