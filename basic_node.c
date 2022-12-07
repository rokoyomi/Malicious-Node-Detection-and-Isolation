#include "contiki.h"
#include "net/rime.h"
#include "lib/list.h"
#include "lib/memb.h"
#include "lib/random.h"
#include "dev/button-sensor.h"
#include "dev/leds.h"

#include <stdio.h>

#define CHANNEL 135
#define NEIGHBOR_TIMEOUT 10 * CLOCK_SECOND
#define MAX_NEIGHBORS 16

struct neighbor {
  struct neighbor *next;
  rimeaddr_t addr;
  struct ctimer ctimer;
  int trust;
};
struct neighbor_trust
{
  rimeaddr_t addr;
  int trust;
};

LIST(neighbor_table);
MEMB(neighbor_mem, struct neighbor, MAX_NEIGHBORS);

PROCESS(multihop_process, "multihop process");
PROCESS(broadcast_process, "broadcast process");
AUTOSTART_PROCESSES(&multihop_process, &broadcast_process);

static rimeaddr_t sink_addr;

static void remove_neighbor(void* _n)
{
  struct neighbor *n = _n;
  if(rimeaddr_cmp(&sink_addr, &n->addr))
    return;

  if (n->trust >= 50)
  {
    n->trust *= 0.99;
    ctimer_set(&n->ctimer, NEIGHBOR_TIMEOUT, remove_neighbor, n);
    return;
  }
  else
    printf("Trust of %d.%d fell below 50\n", n->addr.u8[0], n->addr.u8[1]);
}

static void recv(struct multihop_conn* c, const rimeaddr_t* sender,
  const rimeaddr_t* prevhop, uint8_t hops)
{
  struct neighbor* n;
  if(addr_is_blocked(sender))
  {
    printf("Message from blocked neighbor %d.%d, ignored\n",
      sender->u8[0], sender->u8[1]
    );
    return;
  }
  printf("multihop message from %d.%d received '%s'\n", 
    sender->u8[0], sender->u8[1], (char *)packetbuf_dataptr()
  );
  for(n = list_head(neighbor_table); n != NULL; n = n->next);
  {
    if(rimeaddr_cmp(sender, &n->addr))
      ctimer_set(&n->ctimer, NEIGHBOR_TIMEOUT, remove_neighbor, n);
  }
}

static rimeaddr_t* forward(struct multihop_conn* c, const rimeaddr_t* originator, 
  const rimeaddr_t* dest, const rimeaddr_t* prevhop, uint8_t hops)
{
  /* Find a random neighbor to send to. */
  int num, i;
  struct neighbor *n;

  if(addr_is_blocked(originator))
  {
    printf("packet from blocked neighbor %d.%d, dropped\n",
      originator->u8[0], originator->u8[1]
    );
    return NULL;
  }

  if(list_length(neighbor_table) > 0) {
    num = random_rand() % list_length(neighbor_table);
    i = 0;
    for(n = list_head(neighbor_table); n != NULL && i != num; n = n->next) {
      ++i;
    }
    if(n != NULL) {
      printf("%d.%d: Forwarding packet to %d.%d (%d in list), hops %d\n",
	     rimeaddr_node_addr.u8[0], rimeaddr_node_addr.u8[1],
	     n->addr.u8[0], n->addr.u8[1], num,
	     packetbuf_attr(PACKETBUF_ATTR_HOPS));
      return &n->addr;
    }
  }
  printf("%d.%d: did not find a neighbor to foward to\n",
	 rimeaddr_node_addr.u8[0], rimeaddr_node_addr.u8[1]);
  return NULL;
}

static void broadcast_recv(struct broadcast_conn *c, const rimeaddr_t *from)
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
    if(rimeaddr_cmp(from, &e->addr)) {
      /* Our neighbor was found, so we update the timeout. */
//      ctimer_set(&e->ctimer, NEIGHBOR_TIMEOUT, remove_neighbor, e);
      return;
    }
  }
  e = memb_alloc(&neighbor_mem);
  if(e != NULL) {
    rimeaddr_copy(&e->addr, from);
    list_add(neighbor_table, e);
//    ctimer_set(&e->ctimer, NEIGHBOR_TIMEOUT, remove_neighbor, e);
    e->trust = 100;
  }
}

static const struct multihop_callbacks multihop_call = {recv, forward};
static struct multihop_conn multihop;

static struct broadcast_conn broadcast;
static struct broadcast_callbacks broadcast_call = {broadcast_recv};

/*---------------------------------------------------------------------------*/
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
    etimer_set(&et, 2 * CLOCK_SECOND);

    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    packetbuf_copyfrom("Hello", 6);

    if(!rimeaddr_cmp(&rimeaddr_node_addr, &sink_addr))
    {
      multihop_send(&multihop, &sink_addr);
      printf("Sending multihop message to 1.0\n");
    }

  }

  PROCESS_END();
}

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
    etimer_set(&et, 10 * CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    for(n = list_head(&neighbor_table), i = 0; n != NULL; n = n->next, i++)
    {
      nt[i].addr = n->addr;
      nt[i].trust = n->trust;
    }
    packetbuf_copyfrom(nt, sizeof(nt));
    broadcast_send(&broadcast);
  }
  PROCESS_END();
}




