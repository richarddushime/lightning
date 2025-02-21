#include "config.h"
#include <common/bolt11.h>
#include <plugins/renepay/pay.h>
#include <plugins/renepay/uncertainty_network.h>

static bool chan_extra_check_invariants(struct chan_extra *ce)
{
	bool all_ok = true;
	for(int i=0;i<2;++i)
	{
		all_ok &= amount_msat_less_eq(ce->half[i].known_min,
					      ce->half[i].known_max);
		all_ok &= amount_msat_less_eq(ce->half[i].known_max,
					      ce->capacity);
	}
	struct amount_msat diff_cb,diff_ca;

	all_ok &= amount_msat_sub(&diff_cb,ce->capacity,ce->half[1].known_max);
	all_ok &= amount_msat_sub(&diff_ca,ce->capacity,ce->half[1].known_min);

	all_ok &= amount_msat_eq(ce->half[0].known_min,diff_cb);
	all_ok &= amount_msat_eq(ce->half[0].known_max,diff_ca);
	return all_ok;
}


/* Checks the entire uncertainty network for invariant violations. */
bool uncertainty_network_check_invariants(struct chan_extra_map *chan_extra_map)
{
	bool all_ok = true;

	struct chan_extra_map_iter it;
	for(struct chan_extra *ce = chan_extra_map_first(chan_extra_map,&it);
	    ce && all_ok;
	    ce=chan_extra_map_next(chan_extra_map,&it))
	{
		all_ok &= chan_extra_check_invariants(ce);
	}

	return all_ok;
}

static void add_hintchan(
		struct chan_extra_map *chan_extra_map,
		struct gossmap_localmods *local_gossmods,
		const struct node_id *src,
		const struct node_id *dst,
		u16 cltv_expiry_delta,
		const struct short_channel_id scid,
		u32 fee_base_msat,
		u32 fee_proportional_millionths)
{
	int dir = node_id_cmp(src, dst) < 0 ? 0 : 1;

	struct chan_extra *ce = chan_extra_map_get(chan_extra_map,
						   scid);
	if(!ce)
	{
		/* this channel is not public, we don't know his capacity */
		// TODO(eduardo): one possible solution is set the capacity to
		// MAX_CAP and the state to [0,MAX_CAP]. Alternatively we set
		// the capacity to amoung and state to [amount,amount].
		ce = new_chan_extra(chan_extra_map,
				    scid,
				    MAX_CAP);
		/* FIXME: features? */
		gossmap_local_addchan(local_gossmods,
				      src, dst, &scid, NULL);
		gossmap_local_updatechan(local_gossmods,
					 &scid,
					 /* We assume any HTLC is allowed */
					 AMOUNT_MSAT(0), MAX_CAP,
					 fee_base_msat, fee_proportional_millionths,
					 cltv_expiry_delta,
					 true,
					 dir);
	}

	/* It is wrong to assume that this channel has sufficient capacity!
	 * Doing so leads to knowledge updates in which the known min liquidity
	 * is greater than the channel's capacity. */
	// chan_extra_can_send(chan_extra_map,scid,dir,amount);
}

/* Add routehints provided by bolt11 */
void uncertainty_network_add_routehints(
		struct chan_extra_map *chan_extra_map,
		const struct route_info **routes,
		struct payment *p)
{
	for (size_t i = 0; i < tal_count(routes); i++) {
		/* Each one, presumably, leads to the destination */
		const struct route_info *r = routes[i];
		const struct node_id *end = & p->destination;
		for (int j = tal_count(r)-1; j >= 0; j--) {
			add_hintchan(
				chan_extra_map,
				p->local_gossmods,
				&r[j].pubkey, end,
				r[j].cltv_expiry_delta,
				r[j].short_channel_id,
				r[j].fee_base_msat,
				r[j].fee_proportional_millionths);
			end = &r[j].pubkey;
		}
	}
}

/* Mirror the gossmap in the public uncertainty network.
 * result: Every channel in gossmap must have associated data in chan_extra_map,
 * while every channel in chan_extra_map is also registered in gossmap.
 * */
void uncertainty_network_update(
		const struct gossmap *gossmap,
		struct chan_extra_map *chan_extra_map)
{
	const tal_t* this_ctx = tal(tmpctx,tal_t);

	// For each chan in chan_extra_map remove if not in the gossmap
	struct short_channel_id *del_list
		= tal_arr(this_ctx,struct short_channel_id,0);

	struct chan_extra_map_iter it;
	for(struct chan_extra *ce = chan_extra_map_first(chan_extra_map,&it);
	    ce;
	    ce=chan_extra_map_next(chan_extra_map,&it))
	{
		struct gossmap_chan * chan = gossmap_find_chan(gossmap,&ce->scid);
		/* Only if the channel is not in the gossmap and there are not
		 * HTLCs pending we can remove it. */
		if(!chan && !chan_extra_is_busy(ce))
		{
			// TODO(eduardo): is this efficiently implemented?
			// otherwise i'll use a ccan list
			tal_arr_expand(&del_list, ce->scid);
		}
	}

	for(size_t i=0;i<tal_count(del_list);++i)
	{
 		struct chan_extra *ce = chan_extra_map_get(chan_extra_map,del_list[i]);
		if(!ce)
		{
			plugin_err(pay_plugin->plugin,"%s (line %d) unexpected chan_extra ce is NULL",
				__PRETTY_FUNCTION__,
				__LINE__);
		}
		chan_extra_map_del(chan_extra_map, ce);
		tal_free(ce);
		// TODO(eduardo): if you had added a destructor to ce, you could have removed
		// the ce from the map automatically.

	}

	// For each channel in the gossmap, create a extra data in
	// chan_extra_map
	for(struct gossmap_chan *chan = gossmap_first_chan(gossmap);
	    chan;
	    chan=gossmap_next_chan(gossmap,chan))
	{
		struct short_channel_id scid =
			gossmap_chan_scid(gossmap,chan);
		struct chan_extra *ce = chan_extra_map_get(chan_extra_map,
							   gossmap_chan_scid(gossmap,chan));
		if(!ce)
		{
			struct amount_sat cap;
			struct amount_msat cap_msat;

			if(!gossmap_chan_get_capacity(gossmap,chan,&cap))
			{
				plugin_err(pay_plugin->plugin,"%s (line %d) unable to fetch channel capacity",
					__PRETTY_FUNCTION__,
					__LINE__);
			}
			if(!amount_sat_to_msat(&cap_msat,cap))
			{
				plugin_err(pay_plugin->plugin,"%s (line %d) unable convert sat to msat",
					__PRETTY_FUNCTION__,
					__LINE__);
			}
			new_chan_extra(chan_extra_map,scid,cap_msat);
		}
	}
	tal_free(this_ctx);
}

void uncertainty_network_flow_success(
		struct chan_extra_map *chan_extra_map,
		struct pay_flow *pf)
{
	for (size_t i = 0; i < tal_count(pf->path_scidds); i++)
	{
		chan_extra_sent_success(
			pf, chan_extra_map,
			&pf->path_scidds[i],
			pf->amounts[i]);
	}
}
/* All parts up to erridx succeeded, so we know something about min
 * capacity! */
void uncertainty_network_channel_can_send(
		struct chan_extra_map * chan_extra_map,
		struct pay_flow *pf,
		u32 erridx)
{
	for (size_t i = 0; i < erridx; i++)
	{
		chan_extra_can_send(chan_extra_map,
				    &pf->path_scidds[i],
				    /* This channel can send all that was
				     * commited in HTLCs.
				     * Had we removed the commited amount then
				     * we would have to put here pf->amounts[i]. */
				    AMOUNT_MSAT(0));
	}
}

/* listpeerchannels gives us the certainty on local channels' capacity.  Of course,
 * this is racy and transient, but better than nothing! */
bool uncertainty_network_update_from_listpeerchannels(
		struct chan_extra_map * chan_extra_map,
		struct node_id my_id,
		struct payment *p,
		const char *buf,
		const jsmntok_t *toks)
{
	const jsmntok_t *channels, *channel;
	size_t i;

	if (json_get_member(buf, toks, "error"))
		goto malformed;

	channels = json_get_member(buf, toks, "channels");
	if (!channels)
		goto malformed;

	json_for_each_arr(i, channel, channels) {
		struct short_channel_id_dir scidd;
		const jsmntok_t *scidtok = json_get_member(buf, channel, "short_channel_id");
		/* If channel is still opening, this won't be there.
		 * Also it won't be in the gossmap, so there is
		 * no need to mark it as disabled. */
		if (!scidtok)
			continue;
		if (!json_to_short_channel_id(buf, scidtok, &scidd.scid))
			goto malformed;

		bool connected;
		if(!json_to_bool(buf,
				 json_get_member(buf,channel,"peer_connected"),
				 &connected))
			goto malformed;

		if (!connected) {
			payment_disable_chan(p, scidd.scid, LOG_DBG,
					     "peer disconnected");
			continue;
		}

		const jsmntok_t *spendabletok, *dirtok,*statetok, *totaltok,
			*peeridtok;
		struct amount_msat spendable,capacity;

		const struct node_id src=my_id;
		struct node_id dst;

		spendabletok = json_get_member(buf, channel, "spendable_msat");
		dirtok = json_get_member(buf, channel, "direction");
		statetok = json_get_member(buf, channel, "state");
		totaltok = json_get_member(buf, channel, "total_msat");
		peeridtok = json_get_member(buf,channel,"peer_id");

		if(spendabletok==NULL || dirtok==NULL || statetok==NULL ||
		   totaltok==NULL || peeridtok==NULL)
			goto malformed;
		if (!json_to_msat(buf, spendabletok, &spendable))
			goto malformed;
		if (!json_to_msat(buf, totaltok, &capacity))
			goto malformed;
		if (!json_to_int(buf, dirtok, &scidd.dir))
			goto malformed;
		if(!json_to_node_id(buf,peeridtok,&dst))
			goto malformed;

		/* Don't report opening/closing channels */
		if (!json_tok_streq(buf, statetok, "CHANNELD_NORMAL")
		    && !json_tok_streq(buf, statetok, "CHANNELD_AWAITING_SPLICE")) {
			payment_disable_chan(p, scidd.scid, LOG_DBG,
					     "channel in state %.*s",
					     statetok->end - statetok->start,
					     buf + statetok->start);
			continue;
		}

		struct chan_extra *ce = chan_extra_map_get(chan_extra_map,
							   scidd.scid);

		if(!ce)
		{
			/* this channel is not public, but it belongs to us */
			ce = new_chan_extra(chan_extra_map,
					    scidd.scid,
					    capacity);
			/* FIXME: features? */
			gossmap_local_addchan(p->local_gossmods,
					      &src, &dst, &scidd.scid, NULL);
		}
		gossmap_local_updatechan(p->local_gossmods,
					 &scidd.scid,

					 /* TODO(eduardo): does it
					  * matter to consider HTLC
					  * limits in our own channel? */
					 AMOUNT_MSAT(0),capacity,

					 /* fees = */0,0,

					 /* TODO(eduardo): does it
					  * matter to set this delay? */
					 /*delay=*/0,
					 true,
					 scidd.dir);

		/* FIXME: There is a bug with us trying to send more down a local
		 * channel (after fees) than it has capacity.  For now, we reduce
		 * our capacity by 1% of total, to give fee headroom. */
		if (!amount_msat_sub(&spendable, spendable,
				     amount_msat_div(p->amount, 100)))
			spendable = AMOUNT_MSAT(0);

		// TODO(eduardo): this includes pending HTLC of previous
		// payments!
		/* We know min and max liquidity exactly now! */
		chan_extra_set_liquidity(chan_extra_map,
					 &scidd,spendable);
	}
	return true;

malformed:
	return false;
}

/* Forget ALL channels information by a fraction of the capacity. */
void uncertainty_network_relax_fraction(
		struct chan_extra_map* chan_extra_map,
		double fraction)
{
	struct chan_extra_map_iter it;
	for(struct chan_extra *ce=chan_extra_map_first(chan_extra_map,&it);
	    ce;
	    ce=chan_extra_map_next(chan_extra_map,&it))
	{
		chan_extra_relax_fraction(ce,fraction);
	}
}
