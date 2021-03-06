/*
 * OSPF Flooding -- RFC2328 Section 13.
 * Copyright (C) 1999, 2000 Toshiaki Takada
 *
 * This file is part of GNU Zebra.
 * 
 * GNU Zebra is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2, or (at your
 * option) any later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <zebra.h>

#include "linklist.h"
#include "prefix.h"
#include "if.h"
#include "command.h"
#include "table.h"
#include "thread.h"
#include "memory.h"
#include "log.h"
#include "zclient.h"

#include "ospfd/ospfd.h"
#include "ospfd/ospf_interface.h"
#include "ospfd/ospf_ism.h"
#include "ospfd/ospf_asbr.h"
#include "ospfd/ospf_lsa.h"
#include "ospfd/ospf_lsdb.h"
#include "ospfd/ospf_neighbor.h"
#include "ospfd/ospf_nsm.h"
#include "ospfd/ospf_spf.h"
#include "ospfd/ospf_flood.h"
#include "ospfd/ospf_packet.h"
#include "ospfd/ospf_abr.h"
#include "ospfd/ospf_route.h"
#include "ospfd/ospf_zebra.h"
#include "ospfd/ospf_dump.h"

extern struct zclient *zclient;

/* Do the LSA acking specified in table 19, Section 13.5, row 2
 * This get called from ospf_flood_out_interface. Declared inline 
 * for speed. */
static void
ospf_flood_delayed_lsa_ack (struct ospf_neighbor *inbr, struct ospf_lsa *lsa)
{
  /* LSA is more recent than database copy, but was not
     flooded back out receiving interface.  Delayed
     acknowledgment sent. If interface is in Backup state
     delayed acknowledgment sent only if advertisement
     received from Designated Router, otherwise do nothing See
     RFC 2328 Section 13.5 */

  /* Whether LSA is more recent or not, and whether this is in
     response to the LSA being sent out recieving interface has been 
     worked out previously */

  /* Deal with router as BDR */
  if (inbr->oi->status == ISM_Backup && ! NBR_IS_DR (inbr))
    return;

  /* Schedule a delayed LSA Ack to be sent */ 
  listnode_add (inbr->oi->ls_ack, ospf_lsa_lock (lsa));
}

/* Check LSA is related to external info. */
struct external_info *
ospf_external_info_check (struct ospf_lsa *lsa)
{
  struct as_external_lsa *al;
  struct prefix_ipv4 p;
  struct route_node *rn;
  int type;

  al = (struct as_external_lsa *) lsa->data;

  p.family = AF_INET;
  p.prefix = lsa->data->id;
  p.prefixlen = ip_masklen (al->mask);

  for (type = 0; type <= ZEBRA_ROUTE_MAX; type++)
    {
      int redist_type = is_prefix_default (&p) ? DEFAULT_ROUTE : type;
      if (ospf_is_type_redistributed (redist_type))
	if (EXTERNAL_INFO (type))
	  {
	    rn = route_node_lookup (EXTERNAL_INFO (type),
				    (struct prefix *) &p);
	    if (rn != NULL)
	      {
		route_unlock_node (rn);
		if (rn->info != NULL)
		  return (struct external_info *) rn->info;
	      }
	  }
    }

  return NULL;
}

void
ospf_process_self_originated_lsa (struct ospf_lsa *new, struct ospf_area *area)
{
  struct network_lsa *nlsa;
  listnode node;
  struct ospf_interface *oi;
  struct external_info *ei;
  
  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("LSA[Type%d:%s]: Process self-originated LSA",
	       new->data->type, inet_ntoa (new->data->id));

  /* If we're here, we installed a self-originated LSA that we received
     from a neighbor, i.e. it's more recent.  We must see whether we want
     to originate it.
     If yes, we should use this LSA's sequence number and reoriginate
     a new instance.
     if not --- we must flush this LSA from the domain. */
  switch (new->data->type)
    {
    case OSPF_ROUTER_LSA:
      /* Originate a new instance and schedule flooding */
      /* It shouldn't be necessary, but anyway */
      ospf_lsa_unlock (area->router_lsa_self);
      area->router_lsa_self = ospf_lsa_lock (new);

      ospf_router_lsa_timer_add (area);
      return;
    case OSPF_NETWORK_LSA:
      /* We must find the interface the LSA could belong to.
	 If the interface is no more a broadcast type or we are no more
	 the DR, we flush the LSA otherwise -- create the new instance and
	 schedule flooding. */
      nlsa = (struct network_lsa *) new->data;

      /* Look through all interfaces, not just area, since interface
	 could be moved from one area to another. */
      for (node = listhead (ospf_top->oiflist); node; nextnode (node))
	/* These are sanity check. */
	if ((oi = getdata (node)) != NULL)
	  if (IPV4_ADDR_SAME (&oi->address->u.prefix4, &new->data->id))
	    {
	      if (oi->area != area ||
		  oi->type != OSPF_IFTYPE_BROADCAST ||
		  !IPV4_ADDR_SAME (&oi->address->u.prefix4, &DR (oi)))
		{
		  ospf_schedule_lsa_flush_area (area, new);
		  return;
		}
	      
	      ospf_lsa_unlock (oi->network_lsa_self);
	      oi->network_lsa_self = ospf_lsa_lock (new);
	      
	      /* Schedule network-LSA origination. */
	      ospf_network_lsa_timer_add (oi);
	      return;
	    }
    case OSPF_SUMMARY_LSA:
    case OSPF_SUMMARY_LSA_ASBR:
      ospf_schedule_abr_task ();
      break;
    case OSPF_AS_EXTERNAL_LSA :
#ifdef HAVE_NSSA
    case OSPF_AS_NSSA_LSA:
#endif /* HAVE_NSSA */
      ei = ospf_external_info_check (new);
      if (ei)
	ospf_external_lsa_refresh (new, ei, LSA_REFRESH_FORCE);
      else
	ospf_lsa_flush_as (new);
      break;
    default:
      break;
    }
}

/* OSPF LSA flooding -- RFC2328 Section 13.(5). */

/* Now Updated for NSSA operation, as follows:


	Type-5's have no change.  Blocked to STUB or NSSA.

	Type-7's can be received, and if a DR
	they will also flood the local NSSA Area as Type-7's

	If a Self-Originated LSA (now an ASBR), 
	The LSDB will be updated as Type-5's, (for continual re-fresh)

	    If an NSSA-IR it is installed/flooded as Type-7, P-bit on.
	    if an NSSA-ABR it is installed/flooded as Type-7, P-bit off.

	Later, during the ABR TASK, if the ABR is the Elected NSSA
	translator, then All Type-7s (with P-bit ON) are Translated to
	Type-5's and flooded to all non-NSSA/STUB areas.

	During ASE Calculations, 
	    non-ABRs calculate external routes from Type-7's
	    ABRs calculate external routes from Type-5's and non-self Type-7s
*/
int
ospf_flood (struct ospf_neighbor *nbr, struct ospf_lsa *current,
	    struct ospf_lsa *new)
{
  struct ospf_interface *oi;
  struct timeval now;
  int lsa_ack_flag;

  /* Type-7 LSA's will be flooded throughout their native NSSA area,
     but will also be flooded as Type-5's into ABR capable links.  */

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("LSA[Flooding]: start");

  lsa_ack_flag = 0;
  oi = nbr->oi;

  /* Get current time. */
  gettimeofday (&now, NULL);

  /* If there is already a database copy, and if the
     database copy was received via flooding and installed less
     than MinLSArrival seconds ago, discard the new LSA
     (without acknowledging it). */
  /*  if (current && (ts - current->tv_recv) < OSPF_MIN_LS_ARRIVAL) */
  if (current != NULL &&
      tv_cmp (tv_sub (now, current->tv_recv),
	      int2tv (OSPF_MIN_LS_ARRIVAL)) < 0)
    {
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("LSA[Flooding]: LSA is received recently.");
      return -1;
    }

  /* Flood the new LSA out some subset of the router's interfaces.
     In some cases (e.g., the state of the receiving interface is
     DR and the LSA was received from a router other than the
     Backup DR) the LSA will be flooded back out the receiving
     interface. */
  lsa_ack_flag = ospf_flood_through (nbr, new);

  /* Remove the current database copy from all neighbors' Link state
     retransmission lists.  Only AS_EXTERNAL does not have area ID.
     All other (even NSSA's) do have area ID.  */
  if (current)
    {
      if (current->data->type != OSPF_AS_EXTERNAL_LSA)
	ospf_ls_retransmit_delete_nbr_all (nbr->oi->area, current);
      else
	ospf_ls_retransmit_delete_nbr_all (NULL, current);
    }

  /* Do some internal house keeping that is needed here */
  SET_FLAG (new->flags, OSPF_LSA_RECEIVED);
  ospf_lsa_is_self_originated (new); /* Let it set the flag */

  /* Install the new LSA in the link state database
     (replacing the current database copy).  This may cause the
     routing table calculation to be scheduled.  In addition,
     timestamp the new LSA with the current time.  The flooding
     procedure cannot overwrite the newly installed LSA until
     MinLSArrival seconds have elapsed. */  

  new = ospf_lsa_install (nbr->oi, new);

#ifdef HAVE_NSSA 
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("LSA[Flooding]: Type-%d installed", new->data->type);

  /* if  (new->data->type == OSPF_AS_NSSA_LSA )
     return 0;  */
#endif /* HAVE_NSSA */

  /* Acknowledge the receipt of the LSA by sending a Link State
     Acknowledgment packet back out the receiving interface. */
  if (lsa_ack_flag)
    ospf_flood_delayed_lsa_ack (nbr, new);     

  /* If this new LSA indicates that it was originated by the
     receiving router itself, the router must take special action,
     either updating the LSA or in some cases flushing it from
     the routing domain. */
  if (ospf_lsa_is_self_originated (new))
    ospf_process_self_originated_lsa (new, oi->area);
  else
    /* Update statistics value for OSPF-MIB. */
    ospf_top->rx_lsa_count++;

  return 0;
}

/* OSPF LSA flooding -- RFC2328 Section 13.3. */
int
ospf_flood_through_interface (struct ospf_interface *oi,
			      struct ospf_neighbor *inbr,
			      struct ospf_lsa *lsa)
{
  struct ospf_neighbor *onbr;
  struct route_node *rn;
  int retx_flag;

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_flood_through_interface(): considering int %s",
	       IF_NAME (oi)); 

  if (!ospf_if_is_enable (oi))
    return 0;

  /* Remember if new LSA is aded to a retransmit list. */
  retx_flag = 0;

  /* Each of the neighbors attached to this interface are examined,
     to determine whether they must receive the new LSA.  The following
     steps are executed for each neighbor: */
  for (rn = route_top (oi->nbrs); rn; rn = route_next (rn))
    {
      struct ospf_lsa *ls_req;
 
      if (rn->info == NULL)
	continue;

      onbr = rn->info;
      if (IS_DEBUG_OSPF_EVENT)
	zlog_info ("ospf_flood_through_interface(): considering nbr %s",
		   inet_ntoa (onbr->router_id));

      /* If the neighbor is in a lesser state than Exchange, it
	 does not participate in flooding, and the next neighbor
	 should be examined. */
      if (onbr->status < NSM_Exchange)
	continue;

      /* If the adjacency is not yet full (neighbor state is
	 Exchange or Loading), examine the Link state request
	 list associated with this adjacency.  If there is an
	 instance of the new LSA on the list, it indicates that
	 the neighboring router has an instance of the LSA
	 already.  Compare the new LSA to the neighbor's copy: */
      if (onbr->status < NSM_Full)
	{
	  if (IS_DEBUG_OSPF_EVENT)
	    zlog_info ("ospf_flood_through_interface(): nbr adj is not Full");
	  ls_req = ospf_ls_request_lookup (onbr, lsa);
	  if (ls_req != NULL)
	    {
	      int ret;

	      ret = ospf_lsa_more_recent (ls_req, lsa);
	      /* The new LSA is less recent. */
	      if (ret > 0)
		continue;
	      /* The two copies are the same instance, then delete
		 the LSA from the Link state request list. */
	      else if (ret == 0)
		{
		  ospf_ls_request_delete (onbr, ls_req);
		  ospf_check_nbr_loading (onbr);
		  continue;
		}
	      /* The new LSA is more recent.  Delete the LSA
		 from the Link state request list. */
	      else
		{
		  ospf_ls_request_delete (onbr, ls_req);
		  ospf_check_nbr_loading (onbr);
		}
	    }
	}

      /* If the new LSA was received from this neighbor,
	 examine the next neighbor. */
      if (inbr)
	if (IPV4_ADDR_SAME (&inbr->router_id, &onbr->router_id))
	  continue;

      /* Add the new LSA to the Link state retransmission list
	 for the adjacency. The LSA will be retransmitted
	 at intervals until an acknowledgment is seen from
	 the neighbor. */
      ospf_ls_retransmit_add (onbr, lsa);
      retx_flag = 1;
    }

  /* If in the previous step, the LSA was NOT added to any of
     the Link state retransmission lists, there is no need to
     flood the LSA out the interface. */
  if (retx_flag == 0) 
    {
      return (inbr && inbr->oi == oi);
    }

  /* if we've received the lsa on this interface we need to perform
     additional checking */
  if (inbr && (inbr->oi == oi))
    {
      /* If the new LSA was received on this interface, and it was
	 received from either the Designated Router or the Backup
	 Designated Router, chances are that all the neighbors have
	 received the LSA already. */
      if (NBR_IS_DR (inbr) || NBR_IS_BDR (inbr))
	{
#ifdef HAVE_NSSA
	  if (IS_DEBUG_OSPF_NSSA)
	    zlog_info ("ospf_flood_through_interface(): "
		       "DR/BDR NOT SEND to int %s", IF_NAME (oi));
#endif /* HAVE_NSSA */
	  return 1;
	}
	  
      /* If the new LSA was received on this interface, and the
	 interface state is Backup, examine the next interface.  The
	 Designated Router will do the flooding on this interface.
	 However, if the Designated Router fails the router will
	 end up retransmitting the updates. */

      if (oi->status == ISM_Backup)
	{
#ifdef HAVE_NSSA
	  if (IS_DEBUG_OSPF_NSSA)
	    zlog_info ("ospf_flood_through_interface(): "
		       "ISM_Backup NOT SEND to int %s", IF_NAME (oi));
#endif /* HAVE_NSSA */
	  return 1;
	}
    }

  /* The LSA must be flooded out the interface. Send a Link State
     Update packet (including the new LSA as contents) out the
     interface.  The LSA's LS age must be incremented by InfTransDelay
     (which	must be	> 0) when it is copied into the outgoing Link
     State Update packet (until the LS age field reaches the maximum
     value of MaxAge). */

#ifdef HAVE_NSSA
  if (IS_DEBUG_OSPF_NSSA)
    zlog_info ("ospf_flood_through_interface(): "
	       "DR/BDR sending upd to int %s", IF_NAME (oi));
#else /* ! HAVE_NSSA */

  if (IS_DEBUG_OSPF_EVENT)
    zlog_info ("ospf_flood_through_interface(): "
	       "sending upd to int %s", IF_NAME (oi));
#endif /* HAVE_NSSA */

  /*  RFC2328  Section 13.3
      On non-broadcast networks, separate	Link State Update
      packets must be sent, as unicasts, to each adjacent	neighbor
      (i.e., those in state Exchange or greater).	 The destination
      IP addresses for these packets are the neighbors' IP
      addresses.   */
  if (oi->type == OSPF_IFTYPE_NBMA)
    {
      struct route_node *rn;
      struct ospf_neighbor *nbr;

      for (rn = route_top (oi->nbrs); rn; rn = route_next (rn))
        if ((nbr = rn->info) != NULL)
	  if (nbr != oi->nbr_self && nbr->status >= NSM_Exchange)
	    ospf_ls_upd_send_lsa (nbr, lsa, OSPF_SEND_PACKET_DIRECT);
    }
  else
    ospf_ls_upd_send_lsa (oi->nbr_self, lsa, OSPF_SEND_PACKET_INDIRECT);

  return 0;
}

int
ospf_flood_through_area (struct ospf_area * area,struct ospf_neighbor *inbr,
			 struct ospf_lsa *lsa)
{
  listnode node;
  int lsa_ack_flag = 0;

  /* All other types are specific to a single area (Area A).  The
     eligible interfaces are all those interfaces attaching to the
     Area A.  If Area A is the backbone, this includes all the virtual
     links.  */
  for (node = listhead (area->oiflist); node; nextnode (node))
    {
      struct ospf_interface *oi = getdata (node);

      if (area->area_id.s_addr != OSPF_AREA_BACKBONE &&
	  oi->type ==  OSPF_IFTYPE_VIRTUALLINK) 
	continue;

      if (ospf_flood_through_interface (oi, inbr, lsa))
	lsa_ack_flag = 1;
    }

  return (lsa_ack_flag);
}

int
ospf_flood_through_as (struct ospf_neighbor *inbr, struct ospf_lsa *lsa)
{
  listnode node;
  int lsa_ack_flag;

  lsa_ack_flag = 0;

  /* The incoming LSA is type 5 or type 7  (AS-EXTERNAL or AS-NSSA )

    Divert the Type-5 LSA's to all non-NSSA/STUB areas

    Divert the Type-7 LSA's to all NSSA areas

     AS-external-LSAs are flooded throughout the entire AS, with the
     exception of stub areas (see Section 3.6).  The eligible
     interfaces are all the router's interfaces, excluding virtual
     links and those interfaces attaching to stub areas.  */

#ifdef HAVE_NSSA
  if (CHECK_FLAG (lsa->flags, OSPF_LSA_LOCAL_XLT)) /* Translated from 7  */
    if (IS_DEBUG_OSPF_NSSA)
      zlog_info ("Flood/AS: NSSA TRANSLATED LSA");
#endif /* HAVE_NSSA */

  for (node = listhead (ospf_top->areas); node; nextnode (node))
    {
      int continue_flag = 0;
      struct ospf_area *area = getdata (node);
      listnode if_node;

      switch (area->external_routing)
	{
	  /* Don't send AS externals into stub areas.  Various types
             of support for partial stub areas can be implemented
             here.  NSSA's will receive Type-7's that have areas
             matching the originl LSA. */
	case OSPF_AREA_NSSA:	/* Sending Type 5 or 7 into NSSA area */
#ifdef HAVE_NSSA
	  /* Type-7, flood NSSA area */
          if (lsa->data->type == OSPF_AS_NSSA_LSA) 
	    /* We will send it. */
	    continue_flag = 0;
          else
	    continue_flag = 1;  /* Skip this NSSA area for Type-5's et al */
          break;
#endif /* HAVE_NSSA */

	case OSPF_AREA_TYPE_MAX:
	case OSPF_AREA_STUB:
	  continue_flag = 1;	/* Skip this area. */
	  break;

	case OSPF_AREA_DEFAULT:
	default:
#ifdef HAVE_NSSA
	  /* No Type-7 into normal area */
          if (lsa->data->type == OSPF_AS_NSSA_LSA) 
	    continue_flag = 1; /* skip Type-7 */
          else
#endif /* HAVE_NSSA */
	    continue_flag = 0;	/* Do this area. */
	  break;
	}
      
      /* Do continue for above switch.  Saves a big if then mess */
      if (continue_flag) 
	continue; /* main for-loop */
      
      /* send to every interface in this area */

      for (if_node = listhead (area->oiflist); if_node; nextnode (if_node))
	{
	  struct ospf_interface *oi = getdata (if_node);

	  /* Skip virtual links */
	  if (oi->type !=  OSPF_IFTYPE_VIRTUALLINK)
	    if (ospf_flood_through_interface (oi, inbr, lsa)) /* lsa */
	      lsa_ack_flag = 1;
	}
    } /* main area for-loop */
  
  return (lsa_ack_flag);
}

int
ospf_flood_through (struct ospf_neighbor *inbr, struct ospf_lsa *lsa)
{
  int lsa_ack_flag = 0;
  
  /* Type-7 LSA's for NSSA are flooded throughout the AS here, and
     upon return are updated in the LSDB for Type-7's.  Later,
     re-fresh will re-send them (and also, if ABR, packet code will
     translate to Type-5's)
  
     As usual, Type-5 LSA's (if not DISCARDED because we are STUB or
     NSSA) are flooded throughout the AS, and are updated in the
     global table.  */
  switch (lsa->data->type)
    {
    case OSPF_ROUTER_LSA:
    case OSPF_NETWORK_LSA:
    case OSPF_SUMMARY_LSA:
    case OSPF_SUMMARY_LSA_ASBR:
      lsa_ack_flag = ospf_flood_through_area (inbr->oi->area, inbr, lsa);
      break;
    case OSPF_AS_EXTERNAL_LSA: /* Type-5 */
      lsa_ack_flag = ospf_flood_through_as (inbr, lsa);
      break;
#ifdef HAVE_NSSA
      /* Type-7 Only received within NSSA, then flooded */
    case OSPF_AS_NSSA_LSA:
      /* Any P-bit was installed with the Type-7. */
      lsa_ack_flag = ospf_flood_through_area (inbr->oi->area, inbr, lsa);

      if (IS_DEBUG_OSPF_NSSA)
	zlog_info ("ospf_flood_through: LOCAL NSSA FLOOD of Type-7.");
      break;
#endif /* HAVE_NSSA */
    default:
      break;
    }
  
  return (lsa_ack_flag);
}


/* Management functions for neighbor's Link State Request list. */
void
ospf_ls_request_add (struct ospf_neighbor *nbr, struct ospf_lsa *lsa)
{
  ospf_lsdb_add (&nbr->ls_req, lsa);
}

unsigned long
ospf_ls_request_count (struct ospf_neighbor *nbr)
{
  return ospf_lsdb_count_all (&nbr->ls_req);
}

int
ospf_ls_request_isempty (struct ospf_neighbor *nbr)
{
  return ospf_lsdb_isempty (&nbr->ls_req);
}

/* Remove LSA from neighbor's ls-request list. */
void
ospf_ls_request_delete (struct ospf_neighbor *nbr, struct ospf_lsa *lsa)
{
  if (nbr->ls_req_last == lsa)
    {
      ospf_lsa_unlock (nbr->ls_req_last);
      nbr->ls_req_last = NULL;
    }
  ospf_lsdb_delete (&nbr->ls_req, lsa);
}

/* Remove all LSA from neighbor's ls-requenst list. */
void
ospf_ls_request_delete_all (struct ospf_neighbor *nbr)
{
  ospf_lsa_unlock (nbr->ls_req_last);
  nbr->ls_req_last = NULL;
  ospf_lsdb_delete_all (&nbr->ls_req);
}

/* Lookup LSA from neighbor's ls-request list. */
struct ospf_lsa *
ospf_ls_request_lookup (struct ospf_neighbor *nbr, struct ospf_lsa *lsa)
{
  return ospf_lsdb_lookup (&nbr->ls_req, lsa);
}

struct ospf_lsa *
ospf_ls_request_new (struct lsa_header *lsah)
{
  struct ospf_lsa *new;

  new = ospf_lsa_new ();
  new->data = ospf_lsa_data_new (OSPF_LSA_HEADER_SIZE);
  memcpy (new->data, lsah, OSPF_LSA_HEADER_SIZE);

  return new;
}


/* Management functions for neighbor's ls-retransmit list. */
unsigned long
ospf_ls_retransmit_count (struct ospf_neighbor *nbr)
{
  return ospf_lsdb_count_all (&nbr->ls_rxmt);
}

int
ospf_ls_retransmit_isempty (struct ospf_neighbor *nbr)
{
  return ospf_lsdb_isempty (&nbr->ls_rxmt);
}

/* Add LSA to be retransmitted to neighbor's ls-retransmit list. */
void
ospf_ls_retransmit_add (struct ospf_neighbor *nbr, struct ospf_lsa *lsa)
{
  struct ospf_lsa *old;

  old = ospf_ls_retransmit_lookup (nbr, lsa);

  if (ospf_lsa_more_recent (old, lsa) < 0)
    {
      if (old)
	{
	  old->retransmit_counter--;
	  ospf_lsdb_delete (&nbr->ls_rxmt, old);
	}
      lsa->retransmit_counter++;
      ospf_lsdb_add (&nbr->ls_rxmt, lsa);
    }
/*
    if (!ospf_ls_retransmit_lookup (nbr, lsa))
    {
      lsa->retransmit_counter++;
      ospf_lsdb_add (&nbr->ls_rxmt, lsa);
    }
*/    
}

/* Remove LSA from neibghbor's ls-retransmit list. */
void
ospf_ls_retransmit_delete (struct ospf_neighbor *nbr, struct ospf_lsa *lsa)
{
  if (ospf_ls_retransmit_lookup (nbr, lsa))
    {
      lsa->retransmit_counter--;  
      ospf_lsdb_delete (&nbr->ls_rxmt, lsa);
    }
}

/* Clear neighbor's ls-retransmit list. */
void
ospf_ls_retransmit_clear (struct ospf_neighbor *nbr)
{
  struct ospf_lsdb *lsdb;
  int i;

  lsdb = &nbr->ls_rxmt;

  for (i = OSPF_MIN_LSA; i < OSPF_MAX_LSA; i++)
    {
      struct route_table *table = lsdb->type[i].db;
      struct route_node *rn;
      struct ospf_lsa *lsa;

      for (rn = route_top (table); rn; rn = route_next (rn))
	if ((lsa = rn->info) != NULL)
	  ospf_ls_retransmit_delete (nbr, lsa);
    }

  ospf_lsa_unlock (nbr->ls_req_last);
  nbr->ls_req_last = NULL;
}

/* Lookup LSA from neighbor's ls-retransmit list. */
struct ospf_lsa *
ospf_ls_retransmit_lookup (struct ospf_neighbor *nbr, struct ospf_lsa *lsa)
{
  return ospf_lsdb_lookup (&nbr->ls_rxmt, lsa);
}

/* Remove All neighbor/interface's Link State Retransmit list in area. */
void
ospf_ls_retransmit_delete_nbr_all (struct ospf_area *area,
				   struct ospf_lsa *lsa)
{
  listnode node;
  list oiflist = area ? area->oiflist : ospf_top->oiflist;
  
  for (node = listhead (oiflist); node; nextnode (node))
    {
      struct ospf_interface *oi = getdata (node);
      struct route_node *rn;
      struct ospf_neighbor *nbr;
      struct ospf_lsa *lsr;
      
      if (ospf_if_is_enable (oi))
	for (rn = route_top (oi->nbrs); rn; rn = route_next (rn))
	  /* If LSA find in LS-retransmit list, then remove it. */
	  if ((nbr = rn->info) != NULL)
	    {
	      lsr = ospf_ls_retransmit_lookup (nbr, lsa);
	     
	      /* If LSA find in ls-retransmit list, remove it. */
	      if (lsr != NULL && lsr->data->ls_seqnum == lsa->data->ls_seqnum)
		ospf_ls_retransmit_delete (nbr, lsr);
	    }
    }
}

/* Add LSA to the current database copy of all neighbors'
   Link state retransmission lists. */
void
ospf_ls_retransmit_add_nbr_all (struct ospf_interface *ospfi,
				struct ospf_lsa *lsa)
{
  listnode node;

  for (node = listhead (ospf_top->oiflist); node; nextnode (node))
    {
      struct ospf_interface *oi = getdata (node);
      struct route_node *rn;
      struct ospf_neighbor *nbr;
      struct ospf_lsa *old;

      if (ospf_if_is_enable (oi))
	if (OSPF_AREA_SAME (&ospfi->area, &oi->area))
	  for (rn = route_top (oi->nbrs); rn; rn = route_next (rn))
	    if ((nbr = rn->info) != NULL)
	      if (nbr->status == NSM_Full)
		{
		  if ((old = ospf_ls_retransmit_lookup (nbr, lsa)))
		    ospf_ls_retransmit_delete (nbr, old);

		  ospf_ls_retransmit_add (nbr, lsa);
		}
    }
}


/* Sets ls_age to MaxAge and floods throu the area. 
   When we implement ASE routing, there will be anothe function
   flushing an LSA from the whole domain. */
void
ospf_lsa_flush_area (struct ospf_lsa *lsa, struct ospf_area *area)
{
  lsa->data->ls_age = htons (OSPF_LSA_MAXAGE);
  ospf_flood_through_area (area, NULL, lsa);
  ospf_lsa_maxage (lsa);
}

void
ospf_lsa_flush_as (struct ospf_lsa *lsa)
{
  lsa->data->ls_age = htons (OSPF_LSA_MAXAGE);
  ospf_flood_through_as (NULL, lsa);
  ospf_lsa_maxage (lsa);
}

/* Flush LSA through AS -- used for AS-external-LSAs. */
void
ospf_flush_through_as (struct ospf_lsa *lsa)
{
  lsa->data->ls_age = htons (OSPF_LSA_MAXAGE);
  ospf_flood_through_as (NULL, lsa);
  ospf_lsa_maxage (lsa);
}
