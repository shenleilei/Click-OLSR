/*
 * localroute.{cc,hh} -- Grid multihop local routing element
 * Douglas S. J. De Couto
 *
 * Copyright (c) 2000 Massachusetts Institute of Technology.
 *
 * This software is being provided by the copyright holders under the GNU
 * General Public License, either version 2 or, at your discretion, any later
 * version. For more information, see the `COPYRIGHT' file in the source
 * distribution.
 */

#include <stddef.h>
#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include "localroute.hh"
#include "confparse.hh"
#include "error.hh"
#include "click_ether.h"
#include "click_ip.h"
#include "elements/standard/scheduleinfo.hh"
#include "grid.hh"
#include "router.hh"
#include "glue.hh"

LocalRoute::LocalRoute() : Element(2, 2), _nbr(0), _max_forwarding_hops(5)
{
}

LocalRoute::~LocalRoute()
{
}

void *
LocalRoute::cast(const char *n)
{
  if (strcmp(n, "LocalRoute") == 0)
    return (LocalRoute *)this;
  else
    return 0;
}

int
LocalRoute::configure(const Vector<String> &conf, ErrorHandler *errh)
{
  int res = cp_va_parse(conf, this, errh,
			cpEthernetAddress, "source Ethernet address", &_ethaddr,
			cpIPAddress, "source IP address", &_ipaddr,
			cpOptional,
			cpInteger, "max hops to forward packets for", &_max_forwarding_hops,
			0);
  return res;
}

int
LocalRoute::initialize(ErrorHandler *errh)
{
  /*
   * Try to find a LocationInfo element
   */
  for (int fi = 0; fi < router()->nelements() && !_nbr; fi++) {
    Element *f = router()->element(fi);
    Neighbor *lr = (Neighbor *) f->cast("Neighbor");
    if (lr != 0)
      _nbr = lr;
  }

  if (_nbr == 0) 
    errh->warning("%s: could not find a Neighbor element, will be unable to forward or originate Grid packets", id().cc());

  if (input_is_pull(0))
    ScheduleInfo::join_scheduler(this, errh);
  return 0;
}

void
LocalRoute::run_scheduled()
{
  if (Packet *p = input(0).pull())
    push(0, p); 
  reschedule();

}

void
LocalRoute::push(int port, Packet *packet)
{
  /* 
   * input 1 and output 1 hook up to higher level (e.g. ip), input 0
   * and output 0 hook up to lower level (e.g. ethernet) 
   */

  assert(packet);
  
  if (port == 0) {
    /*
     * input from net device
     */
    grid_hdr *gh = (grid_hdr *) (packet->data() + sizeof(click_ether));
    switch (gh->type) {

    case GRID_HELLO:
      // nothing further to do, Hello packets are not forwarded
      packet->kill();
      break;

    case GRID_NBR_ENCAP:
      {/* 
	* try to either receive the packet or forward it
	*/
	struct grid_nbr_encap *encap = (grid_nbr_encap *) (packet->data() + sizeof(click_ether) + gh->hdr_len);
	IPAddress dest_ip(encap->dst_ip);
	if (dest_ip == _ipaddr) {
	  // it's for us, send to higher level
	  packet->pull(sizeof(click_ether) + gh->hdr_len + sizeof(grid_nbr_encap)); 
	  output(1).push(packet);
	  break;
	}
	else {
	  // try to forward it!
	  forward_grid_packet(packet, encap->dst_ip);
	}
      }
      break;

    default:
      click_chatter("%s: received unknown Grid packet type: %d", id().cc(), (int) gh->type);
      packet->kill();
    }
  }
  else {
    /*
     * input from higher level protocol -- expects IP packets
     * annotated with src IP address.
     */
    assert(port == 1);
    // check to see is the desired dest is our neighbor
    IPAddress dst = packet->dst_ip_anno();
    if (dst == _ipaddr) {
      click_chatter("%s: got IP packet from us for our address; looping it back", id().cc());
      output(1).push(packet);
    }
    else {
      // encapsulate packet with grid hdr and try to send it out
      packet = packet->push(sizeof(click_ether) + sizeof(grid_hdr) + sizeof(grid_nbr_encap));
      memset(packet->data(), 0, sizeof(click_ether) + sizeof(grid_hdr) + sizeof(grid_nbr_encap));

      struct click_ether *eh = (click_ether *) packet->data();
      eh->ether_type = htons(ETHERTYPE_GRID);

      struct grid_hdr *gh = (grid_hdr *) (packet->data() + sizeof(click_ether));
      gh->hdr_len = sizeof(grid_hdr);
      gh->total_len = packet->length(); // encapsulate everything we get, don't look inside it for length info
      gh->type = GRID_NBR_ENCAP;

      struct grid_nbr_encap *encap = (grid_nbr_encap *) (packet->data() + sizeof(click_ether) + sizeof(grid_hdr));
      encap->hops_travelled = 0;
      encap->dst_ip = dst.addr();

      forward_grid_packet(packet, dst);
    }
  }
}

LocalRoute *
LocalRoute::clone() const
{
  return new LocalRoute;
}


void
LocalRoute::add_handlers()
{
  add_default_handlers(true);
}


void
LocalRoute::forward_grid_packet(Packet *packet, IPAddress dest_ip)
{

  click_chatter("fwding for dst %s", dest_ip.s().cc());

  /*
   * packet must have a MAC hdr, grid_hdr, and a grid_nbr_encap hdr on
   * it.  This function will update the hop count, src ip (us) and
   * dst/src MAC addresses.  
   */

  if (_nbr == 0) {
    // no Neighbor next-hop table in configuration
    packet->kill();
    return;
  }

  struct grid_nbr_encap *encap = (grid_nbr_encap *) (packet->data() + sizeof(click_ether) + sizeof(grid_hdr));
  if (encap->hops_travelled > _max_forwarding_hops) {
    click_chatter("%s: dropping packet that has travelled too many hops", id().cc());
    packet->kill();
    return;
  }

  EtherAddress next_hop_eth;
  bool found_next_hop = _nbr->get_next_hop(dest_ip, &next_hop_eth);

  if (found_next_hop) {
    struct click_ether *eh = (click_ether *) packet->data();
    memcpy(eh->ether_shost, _ethaddr.data(), 6);
    memcpy(eh->ether_dhost, next_hop_eth.data(), 6);
    struct grid_hdr *gh = (grid_hdr *) (packet->data() + sizeof(click_ether));
    gh->ip = _ipaddr.addr();
    encap->hops_travelled++;
    // leave src location update to FixSrcLoc element
    output(0).push(packet);
  }
  else {
    click_chatter("%s: unable to forward packet for %s", id().cc(), dest_ip.s().cc());
    packet->kill();
  }
}


EXPORT_ELEMENT(LocalRoute)
