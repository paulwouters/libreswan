/* information about connections between hosts and clients
 *
 * Copyright (C) 1998-2010,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2007 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2007 Ken Bantoft <ken@xelerance.com>
 * Copyright (C) 2009 Stefan Arentz <stefan@arentz.ca>
 * Copyright (C) 2009-2010 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2007-2011 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2010,2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012 Panagiotis Tamtamis <tamtamis@gmail.com>
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2013 Antony Antony <antony@phenome.org>
 * Copyright (C) 2019 Andrew Cagney <cagney@gnu.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include "connections.h"
#include "pending.h"
#include "timer.h"
#include "kernel_ops.h"			/* for raw_policy() */
#include "log.h"
#include "ikev1_spdb.h"			/* for kernel_alg_makedb() !?! */
#include "initiate.h"
#include "host_pair.h"
#include "orient.h"
#include "ikev1.h"			/* for aggr_outI1() and main_outI1() */
#include "ikev1_quick.h"		/* for quick_outI1() */
#include "ikev2.h"			/* for ikev2_state_transition_fn; */
#include "ikev2_ike_sa_init.h"		/* for ikev2_out_IKE_SA_INIT_I() */
#include "ikev2_create_child_sa.h"	/* for initiate_v2_CREATE_CHILD_SA_create_child() */
#include "labeled_ipsec.h"		/* for sec_label_within_range() */
#include "ip_info.h"

static bool initiate_connection_1_basics(struct connection *c,
					 const char *remote_host,
					 bool background);
static bool initiate_connection_2_address(struct connection *c,
					  const char *remote_host,
					  bool background,
					  const threadtime_t inception);
static bool initiate_connection_3_sec_label(struct connection *c,
					    bool background,
					    const threadtime_t inception);
static bool initiate_connection_4_fab(struct connection *c,
				      bool background,
				      const threadtime_t inception);

/* attach FD */

bool initiate_connection(struct connection *c,
			 const char *remote_host,
			 bool background, bool log_failure,
			 struct logger *logger)
{
	/* XXX: something better? */
	fd_delref(&c->logger->global_whackfd);
	c->logger->global_whackfd =
		(/* old IKE SA */ fd_p(logger->object_whackfd) ? fd_addref(logger->object_whackfd) :
		 /* global */ fd_p(logger->global_whackfd) ? fd_addref(logger->global_whackfd) :
		 null_fd);
	bool ok = initiate_connection_1_basics(c, remote_host, background);
	if (log_failure && !ok) {
		llog(RC_FATAL, c->logger, "failed to initiate connection");
	}
	/* XXX: something better? */
	fd_delref(&c->logger->global_whackfd);
	return ok;
}

/*
 * Perform some basic, and presumably cheap, checks on the connection.
 * No point continuing if the connection isn't viable.
 */

static bool initiate_connection_1_basics(struct connection *c,
					 const char *remote_host,
					 bool background)
{
	connection_buf cb;
	dbg("%s() for "PRI_CONNECTION" in the %s with "PRI_LOGGER,
	    __func__, pri_connection(c, &cb),
	    background ? "background" : "foreground",
	    pri_logger(c->logger));

	if (!oriented(c)) {
		ipstr_buf a;
		ipstr_buf b;
		llog(RC_ORIENT, c->logger,
		     "we cannot identify ourselves with either end of this connection.  %s or %s are not usable",
		     ipstr(&c->local->host.addr, &a),
		     ipstr(&c->remote->host.addr, &b));
		return false;
	}

	if (NEVER_NEGOTIATE(c->policy)) {
		llog(RC_INITSHUNT, c->logger,
		     "cannot initiate an authby=never connection");
		return false;
	}

	threadtime_t inception = threadtime_start();
	return initiate_connection_2_address(c, remote_host, background, inception);
}

/*
 * Resolve remote host, If there's a REMOTE_HOST, convert that into an
 * IP address and the instantiate C filling in that address as the
 * peer.
 */

static bool initiate_connection_2_address(struct connection *c,
					  const char *remote_host,
					  bool background,
					  const threadtime_t inception)
{
	connection_buf cb;
	dbg("%s() for "PRI_CONNECTION" in the %s with "PRI_LOGGER,
	    __func__, pri_connection(c, &cb),
	    background ? "background" : "foreground",
	    pri_logger(c->logger));

	if (remote_host != NULL && !address_is_specified(c->remote->host.addr)) {

		/*
		 * The connection has no remote address but whack
		 * supplied one.  Assuming it resolves, use that as
		 * the remote address and then continue.
		 */

		if (c->kind != CK_TEMPLATE) {
			llog(RC_NOPEERIP, c->logger,
			     "cannot instantiate non-template connection to a supplied remote IP address");
			return false;
		}

		ip_address remote_ip;
		err_t e = ttoaddress_dns(shunk1(remote_host), NULL/*UNSPEC*/, &remote_ip);
		if (e != NULL) {
			llog(RC_NOPEERIP, c->logger,
			     "cannot instantiate connection: resolution of \"%s\" failed: %s",
			     remote_host, e);
			return false;
		}

		if (!address_is_specified(remote_ip)) {
			llog(RC_NOPEERIP, c->logger,
			     "cannot instantiate connection: \"%s\" resolved to the unspecified address",
			     remote_host);
			return false;
		}

		struct connection *d = spd_instantiate(c, remote_ip, NULL, null_shunk);

		/*
		 * D could either be an instance, or a sec_label
		 * connection?
		 */

		/* XXX: something better? */
		fd_delref(&d->logger->global_whackfd);
		d->logger->global_whackfd = fd_addref(c->logger->global_whackfd);

		address_buf ab;
		llog(RC_LOG, d->logger,
		     "instantiated connection with remote IP set to %s",
		     str_address(&remote_ip, &ab));

		/* flip cur_connection */
		bool ok = initiate_connection_3_sec_label(d, background, inception);

		if (!ok) {
			/* instance so free to delete */
			delete_connection(&d);
		} else {
			/* XXX: something better? */
			fd_delref(&d->logger->global_whackfd);
		}
		return ok;
	}

	if (!address_is_specified(c->remote->host.addr)) {

		/*
		 * Cant proceed; there's no peer address!  However, if
		 * there's a DNS hostname flip things to up so that
		 * the DNS code, below, will kick in.  Try to provide
		 * a really detailed message!!!
		 */

		if (c->config->dnshostname != NULL) {
			if (c->policy & POLICY_IKEV2_ALLOW_NARROWING) {
				esb_buf b;
				llog(RC_NOPEERIP, c->logger,
				     "cannot initiate connection without resolved dynamic peer IP address, will keep retrying (kind=%s, narrowing=%s)",
				     enum_show(&connection_kind_names, c->kind, &b),
				     bool_str((c->policy & POLICY_IKEV2_ALLOW_NARROWING) != LEMPTY));
			} else {
				esb_buf b;
				llog(RC_NOPEERIP, c->logger,
				     "cannot initiate connection without resolved dynamic peer IP address, will keep retrying (kind=%s)",
				     enum_show(&connection_kind_names, c->kind, &b));
			}
			dbg("%s() connection '%s' +POLICY_UP", __func__, c->name);
			c->policy |= POLICY_UP;
			return true;
		}

		if (c->policy & POLICY_IKEV2_ALLOW_NARROWING) {
			esb_buf b;
			llog(RC_NOPEERIP, c->logger,
			     "cannot initiate connection without knowing peer IP address (kind=%s narrowing=%s)",
			     enum_show(&connection_kind_names, c->kind, &b),
			     bool_str((c->policy & POLICY_IKEV2_ALLOW_NARROWING) != LEMPTY));
		} else {
			esb_buf b;
			llog(RC_NOPEERIP, c->logger,
			     "cannot initiate connection (serial "PRI_CO") without knowing peer IP address (kind=%s)",
			     pri_co(c->serialno),
			     enum_show(&connection_kind_names, c->kind, &b));
		}
		return false;
	}

	return initiate_connection_3_sec_label(c, background, inception);
}

static bool initiate_connection_3_sec_label(struct connection *c,
					    bool background,
					    const threadtime_t inception)
{
	connection_buf cb;
	dbg("%s() for "PRI_CONNECTION" in the %s with "PRI_LOGGER,
	    __func__, pri_connection(c, &cb),
	    background ? "background" : "foreground",
	    pri_logger(c->logger));

	passert(address_is_specified(c->remote->host.addr));

	if (c->kind == CK_TEMPLATE &&
	    c->config->ike_version == IKEv2 &&
	    (c->policy & POLICY_IKEV2_ALLOW_NARROWING)) {
		struct connection *d = spd_instantiate(c, c->remote->host.addr,
						       NULL, null_shunk);
		/* XXX: something better? */
		fd_delref(&d->logger->global_whackfd);
		d->logger->global_whackfd = fd_addref(c->logger->global_whackfd);
		/*
		 * LOGGING: why not log this (other than it messes
		 * with test output)?
		 */
		llog(LOG_STREAM|RC_LOG, d->logger, "instantiated connection");
		/* flip cur_connection */
		bool ok = initiate_connection_4_fab(d, background, inception);
		if (!ok) {
			delete_connection(&d);
		} else {
			/* XXX: something better? */
			fd_delref(&d->logger->global_whackfd);
		}
		return ok;
	}

	return initiate_connection_4_fab(c, background, inception);
}

static bool initiate_connection_4_fab(struct connection *c,
				      bool background,
				      const threadtime_t inception)
{
	connection_buf cb;
	dbg("%s() for "PRI_CONNECTION" in the %s with "PRI_LOGGER,
	    __func__, pri_connection(c, &cb), background ? "background" : "foreground",
	    pri_logger(c->logger));

	/* We will only request an IPsec SA if policy isn't empty
	 * (ignoring Main Mode items).
	 * This is a fudge, but not yet important.
	 *
	 * XXX:  Is this still useful?
	 *
	 * In theory, by delaying the the kernel algorithm probe until
	 * here when the connection is being initiated, it is possible
	 * to detect kernel algorithms that have been loaded after
	 * pluto has started or are only loaded on-demand.
	 *
	 * In reality, the kernel algorithm DB is "static": PFKEY is
	 * only probed during startup(?); and XFRM, even if it does
	 * support probing, is using static entries.  See
	 * kernel_alg.c.
	 *
	 * Consequently:
	 *
	 * - when the connection's proposal suite is specified, the
	 * algorithm parser will check the algorithms against the
	 * kernel algorithm DB, so calling kernel_alg_makedb() to to
	 * perform an identical check is redundant
	 *
	 * - when default proposals are used (CHILD_PROPOSALS.P==NULL)
	 * (the parser can't see these) kernel_alg_makedb(NULL)
	 * returns a static table and skips all checks
	 *
	 * - finally, kernel_alg_makedb() is IKEv1 only
	 *
	 * A better fix would be to feed the proposal parser the
	 * default proposal suite.
	 *
	 * For moment leave call but make it IKEv1 only - for IKEv2
	 * all it does is give spdb.c some busy work (and log bogus
	 * stats).
	 *
	 * XXX: mumble something about c->config->ike_version
	 */
#ifdef USE_IKEv1
	if (c->config->ike_version == IKEv1 &&
	    (c->policy & (POLICY_ENCRYPT | POLICY_AUTHENTICATE))) {
		struct db_sa *phase2_sa = v1_kernel_alg_makedb(c->policy, c->config->child_proposals,
							       true, c->logger);
		if (c->config->child_proposals.p != NULL && phase2_sa == NULL) {
			llog(WHACK_STREAM|RC_LOG_SERIOUS, c->logger,
			     "cannot initiate: no acceptable kernel algorithms loaded");
			return false;
		}
		free_sa(&phase2_sa);
	}
#endif

	dbg("%s() connection '%s' +POLICY_UP", __func__, c->name);
	c->policy |= POLICY_UP;

	/*
	 * FOR IKEv2, when the sec_label template connection is
	 * initiated, there is no acquire and, hence, no Child SA to
	 * establish.
	 */

	ipsecdoi_initiate(c, c->policy, 1, SOS_NOBODY, &inception,
			  (c->config->ike_version == IKEv1 ? HUNK_AS_SHUNK(c->child.sec_label) : null_shunk),
			  background, c->logger);
	return true;
}

void ipsecdoi_initiate(struct connection *c,
		       lset_t policy,
		       unsigned long try,
		       so_serial_t replacing,
		       const threadtime_t *inception,
		       shunk_t sec_label,
		       bool background, struct logger *logger)
{
	if (sec_label.len > 0)
		dbg("ipsecdoi_initiate() called with sec_label "PRI_SHUNK, pri_shunk(sec_label));

	switch (c->config->ike_version) {
#ifdef USE_IKEv1
	case IKEv1:
	{
		/*
		 * If there's already an IKEv1 ISAKMP SA established,
		 * use that and go directly to Quick Mode.  We are
		 * even willing to use one that is still being
		 * negotiated, but only if we are the Initiator (thus
		 * we can be sure that the IDs are not going to
		 * change; other issues around intent might matter).
		 * Note: there is no way to initiate with a Road
		 * Warrior.
		 */
		struct state *st = find_phase1_state(c,
						     V1_ISAKMP_SA_ESTABLISHED_STATES |
						     V1_PHASE1_INITIATOR_STATES);
		struct fd *whackfd = background ? null_fd : logger->global_whackfd;
		if (st == NULL && (policy & POLICY_AGGRESSIVE)) {
			aggr_outI1(whackfd, c, NULL, policy, try, inception, sec_label);
		} else if (st == NULL) {
			main_outI1(whackfd, c, NULL, policy, try, inception, sec_label);
		} else if (IS_V1_ISAKMP_SA_ESTABLISHED(st)) {
			/*
			 * ??? we assume that peer_nexthop_sin isn't
			 * important: we already have it from when we
			 * negotiated the ISAKMP SA!  It isn't clear
			 * what to do with the error return.
			 */
			quick_outI1(whackfd, st, c, policy, try,
				    replacing, sec_label);
		} else {
			/* leave our Phase 2 negotiation pending */
			add_v1_pending(whackfd, pexpect_ike_sa(st),
				       c, policy, try,
				       replacing, sec_label,
				       false /*part of initiate*/);
		}
		break;
	}
#endif
	case IKEv2:
	{
		/*
		 * If there's already an IKEv2 IKE SA established, use
		 * that and go directly to a CHILD exchange.
		 *
		 * We are even willing to use one that is still being
		 * established, but only if we are the Initiator (thus
		 * we can be sure that the IDs are not going to
		 * change; other issues around intent might matter).
		 * Note: there is no way to initiate with a Road
		 * Warrior.
		 */
		struct ike_sa *ike =
			pexpect_ike_sa(find_phase1_state(c,
							 LELEM(STATE_V2_ESTABLISHED_IKE_SA) |
							 IKEV2_ISAKMP_INITIATOR_STATES));
		if (ike != NULL) {
			dbg("found #%lu in state %s established=%s viable=%s",
			    ike->sa.st_serialno, ike->sa.st_state->name,
			    bool_str(IS_IKE_SA_ESTABLISHED(&ike->sa)),
			    bool_str(ike->sa.st_viable_parent));
		}
		if (ike == NULL) {
			initiate_v2_IKE_SA_INIT_request(c, NULL, policy, try, inception,
							sec_label, background, logger);
		} else if (!IS_IKE_SA_ESTABLISHED(&ike->sa)) {
			/* leave CHILD SA negotiation pending */
			add_v2_pending(background ? null_fd : logger->global_whackfd,
				       ike, c, policy, try,
				       replacing, sec_label,
				       false /*part of initiate*/);
		} else if (!already_has_larval_v2_child(ike, c)) {
			dbg("initiating child sa with "PRI_LOGGER, pri_logger(logger));
			struct connection *cc;
			if (c->kind == CK_TEMPLATE && sec_label.len > 0) {
				/*
				 * create instance and switch to it.
				 *
				 * Since the newly instantiated
				 * connection has a security label due
				 * to an `ACQUIRE` message from the
				 * kernel, it is not a template
				 * connection.
				 */
				ip_address remote_addr = endpoint_address(ike->sa.st_remote_endpoint);
				cc = spd_instantiate(c, remote_addr, NULL, sec_label);
			} else {
				cc = c;
			}
			submit_v2_CREATE_CHILD_SA_new_child(ike, cc, policy, try,
							    logger->global_whackfd);
		}
		break;
	}
	default:
		bad_case(c->config->ike_version);
	}
}

static bool same_host(const char *a_dnshostname, const ip_address *a_host_addr,
		const char *b_dnshostname, const ip_address *b_host_addr)
{
	/* should this be dnshostname and host_addr ?? */

	return a_dnshostname == NULL ?
		b_dnshostname == NULL && sameaddr(a_host_addr, b_host_addr) :
		b_dnshostname != NULL && streq(a_dnshostname, b_dnshostname);
}

void restart_connections_by_peer(struct connection *const c, struct logger *logger)
{
	/*
	 * If c is a CK_INSTANCE, it will be removed by terminate_connection.
	 * Any parts of c we need after that must be copied first.
	 */

	struct host_pair *hp = c->host_pair;
	enum connection_kind c_kind = c->kind;
	struct connection *hp_next = hp->connections->hp_next;

	pexpect(hp != NULL);	/* ??? why would this happen? */
	if (hp == NULL)
		return;

	char *dnshostname = clone_str(c->config->dnshostname, "dnshostname for restart");

	ip_address host_addr = c->remote->host.addr;

	struct connection *d;

	for (d = hp->connections; d != NULL;) {
		struct connection *next = d->hp_next; /* copy before d is deleted, CK_INSTANCE */

		if (same_host(dnshostname, &host_addr,
			      d->config->dnshostname, &d->remote->host.addr)) {
			/* This might delete c if CK_INSTANCE */
			/* ??? is there a chance hp becomes dangling? */
			terminate_connections_by_name(d->name, /*quiet?*/false, logger);
		}
		d = next;
	}

	if (c_kind != CK_INSTANCE) {
		/* reference to c is OK because not CK_INSTANCE */
		update_host_pairs(c);
		/* host_pair/host_addr changes with dynamic dns */
		hp = c->host_pair;
		host_addr = c->remote->host.addr;
	}

	if (c_kind == CK_INSTANCE && hp_next == NULL) {
		/* in simple cases this is a dangling hp */
		dbg("no connection to restart after termination");
	} else {
		for (struct connection *d = hp->connections; d != NULL; d = d->hp_next) {
			if (same_host(dnshostname, &host_addr,
				      d->config->dnshostname,
				      &d->remote->host.addr)) {
				initiate_connection(d, NULL/*remote-host*/,
						    false/*background*/,
						    true/*verbose*/,
						    logger);
			}
		}
	}
	pfreeany(dnshostname);
}

/* (Possibly) Opportunistic Initiation:
 * Knowing clients (single IP addresses), try to build a tunnel.
 * This may involve discovering a gateway and instantiating an
 * Opportunistic connection.  Called when a packet is caught by
 * a %trap, or when whack --oppohere --oppothere is used.
 * It may turn out that an existing or non-opporunistic connection
 * can handle the traffic.
 *
 * Most of the code will be restarted if an ADNS request is made
 * to discover the gateway.  The only difference between the first
 * and second entry is whether gateways_from_dns is NULL or not.
 *	initiate_opportunistic: initial entrypoint
 *	continue_oppo: where we pickup when ADNS result arrives
 *	initiate_opportunistic_body: main body shared by above routines
 *	cannot_ondemand: a helper function to log a diagnostic
 * This structure repeats a lot of code when the ADNS result arrives.
 * This seems like a waste, but anything learned the first time through
 * may no longer be true!
 *
 * After the first IKE message is sent, the regular state machinery
 * carries negotiation forward.
 */

struct find_oppo_bundle {
	ip_packet packet; /* that triggered the opportunistic exchange */
	bool by_acquire;	/* acquire? whack? */
	policy_prio_t policy_prio;
	enum shunt_policy negotiation_shunt;
	enum shunt_policy failure_shunt;
	struct logger *logger;	/* has whack attached */
	bool background;
	shunk_t sec_label;
};

static void jam_oppo_bundle(struct jambuf *buf, struct find_oppo_bundle *b)
{
	jam(buf, "initiate on-demand for packet ");
	jam_packet(buf, &b->packet);
	if (!b->by_acquire) {
		jam(buf, " by whack");
	}
	if (b->sec_label.len > 0) {
		jam(buf, " sec_label=");
		jam_sanitized_hunk(buf, b->sec_label);
	}
}

static void cannot_ondemand(lset_t rc_flags, struct find_oppo_bundle *b, const char *ughmsg)
{
	LLOG_JAMBUF(rc_flags, b->logger, buf) {
		jam(buf, "cannot ");
		jam_oppo_bundle(buf, b);
		jam(buf, ": %s", ughmsg);
	}

	if (b->by_acquire) {
		/*
		 * A kernel policy was created for us based on the
		 * packet trigger and not whack --oppo trigger.
		 * Hence, there really is something in the kernel that
		 * needs updating.
		 *
		 * Replace negotiationshunt (hold or pass) with
		 * failureshunt (hold or pass).  If no failure_shunt
		 * specified, use SHUNT_PASS -- THIS MAY CHANGE.
		 *
		 * Should SHUNT_PASS instead call with a delete?
		 */
		dbg("cannot_ondemand() replaced negotiationshunt with bare failureshunt=%s",
		    enum_name_short(&shunt_policy_names, b->failure_shunt));
		pexpect(b->failure_shunt != SHUNT_UNSET); /* set to something */
		ip_selector src = packet_src_selector(b->packet);
		ip_selector dst = packet_dst_selector(b->packet);
		struct kernel_policy outbound_policy =
			stateless_kernel_policy(&src, &dst,
						/* we don't know connection for priority yet */
						max_kernel_priority,
						b->failure_shunt, HERE);

		if (!raw_policy(KERNEL_POLICY_OP_REPLACE,
				DIRECTION_OUTBOUND,
				EXPECT_KERNEL_POLICY_OK,
				&outbound_policy.src.client, &outbound_policy.dst.client,
				&outbound_policy,
				deltatime(SHUNT_PATIENCE),
				NULL, /* sa_marks */
				0 /* xfrm interface id */,
				b->sec_label, b->logger,
				"%s() %s", __func__, ughmsg)) {
			llog(RC_LOG_SERIOUS, b->logger,
			     "failed to replace negotiationshunt with bare failureshunt");
			return;
		}
	}
}

/*
 * Widen (narrow) the shunt?
 *
 * If we have protoport= set, narrow to it.  Zero the
 * ephemeral port.
 *
 * XXX: should local/remote shunts be computed independently?
	*/
static ip_selector shunt_from_address_and_selector(const char *what,
						   const ip_address packet_address,
						   ip_selector end_selector)
{
	const struct ip_protocol *end_protocol = selector_protocol(end_selector);
	ip_port end_port = selector_port(end_selector);
	/* log and cross check */
	if (end_protocol == &ip_protocol_all) {
		dbg("widening %s shunt to all protocols + all ports", what);
		pexpect(end_port.hport == 0);
	} else if (end_port.hport == 0) {
		dbg("widening %s shunt %s protocol to all ports",
		    what, end_protocol->name);
	} else {
		dbg("leaving %s shunt %s protocol %d port alone",
		    what, end_protocol->name, end_port.hport);
	}
	return selector_from_address_protocol_port(packet_address,
						   end_protocol,
						   end_port);
}

static void initiate_ondemand_body(struct find_oppo_bundle *b)
{
	threadtime_t inception = threadtime_start();

	if (b->sec_label.len > 0) {
		dbg("oppo bundle: received security label string: "PRI_SHUNK,
		    pri_shunk(b->sec_label));
	}

	/*
	 * What connection shall we use?  First try for one that
	 * explicitly handles the clients.
	 */

	if (!b->packet.is_set) {
		cannot_ondemand(RC_OPPOFAILURE, b, "impossible IP address");
		return;
	}

	/* XXX: shouldn't this have happened earlier? */
	if (thingeq(b->packet.src.bytes, b->packet.dst.bytes)) {
		/*
		 * NETKEY gives us acquires for our own IP. This code
		 * does not handle talking to ourselves on another ip.
		 */
		cannot_ondemand(RC_OPPOFAILURE, b, "acquire for our own IP address");
		return;
	}

	struct spd_route *sr = NULL;
	struct connection *c = find_connection_for_packet(&sr, b->packet,
							  b->sec_label,
							  b->logger);
	if (c == NULL) {
		/*
		 * No connection explicitly handles the clients and
		 * there are no Opportunistic connections -- whine and
		 * give up.  The failure policy cannot be gotten from
		 * a connection; we pick %pass.
		 */
		cannot_ondemand(RC_OPPOFAILURE, b, "no routed template covers this pair");
		return;
	}

	if (c->config->ike_version == IKEv2 &&
	    c->config->sec_label.len > 0 &&
	    c->kind == CK_TEMPLATE) {
		dbg("IKEv2 connection has security label");

		if (b->sec_label.len == 0) {
			cannot_ondemand(RC_LOG_SERIOUS, b,
					"kernel acquire missing security label");
			return;
		}

		if (!sec_label_within_range("acquire", HUNK_AS_SHUNK(b->sec_label),
					    c->config->sec_label, b->logger)) {
			cannot_ondemand(RC_LOG_SERIOUS, b,
					"received kernel security label does not fall within range of our connection");
			return;
		}

		/*
		 * We've found a connection that can serve.  Do we
		 * have to initiate it?  Not if there is currently an
		 * IPSEC SA.  This may be redundant if a
		 * non-opportunistic negotiation is already being
		 * attempted.
		 *
		 * If we are to proceed asynchronously, b->whackfd
		 * will be NULL_WHACKFD.
		 *
		 * We have a connection: fill in the negotiation_shunt
		 * and failure_shunt.
		 */
		b->failure_shunt = c->config->failure_shunt;
		b->negotiation_shunt = c->config->negotiation_shunt;

		/*
		 * Announce this to the world.  Use c->logger instead?
		 */
		LLOG_JAMBUF(RC_LOG, b->logger, buf) {
			jam_oppo_bundle(buf, b);
			/* jam(buf, " using "); */
		}

		ipsecdoi_initiate(c, c->policy, 1, SOS_NOBODY, &inception, b->sec_label,
				  b->background, b->logger);

		packet_buf pb;
		enum_buf hab;
		dbg("initiated on demand using security label and %s %s",
		    str_enum_short(&keyword_auth_names, c->local->host.config->auth, &hab),
		    str_packet(&b->packet, &pb));

		return;
	}

	if ((c->policy & POLICY_OPPORTUNISTIC) && !orient(c, b->logger)) {
		/*
		 * happens when dst is ourselves on a different IP
		 */
		cannot_ondemand(RC_OPPOFAILURE, b, "connection to self on another IP?");
		return;
	}

	if (c->kind == CK_TEMPLATE && (c->policy & POLICY_OPPORTUNISTIC) == 0) {
		cannot_ondemand(RC_NOPEERIP, b, "template connection");
		return;
	}

	/* Labeled IPsec and Opportunistic cannot both be used */
	if (c->kind == CK_INSTANCE && b->sec_label.len == 0) {
		connection_buf cib;
		/* there is already an instance being negotiated */
#if 0
		llog(RC_LOG, b->logger,
			    "rekeying existing instance "PRI_CONNECTION", due to acquire",
			    pri_connection(c, &cib));

		/*
		 * We used to return here, but rekeying is a better
		 * choice.  If we got the acquire, it is because
		 * something turned stuff into a %trap, or something
		 * got deleted, perhaps due to an expiry.
		 */
#else
		/*
		 * XXX We got an acquire (NETKEY only?) for
		 * something we already have an instance for ??
		 * We cannot process as normal because the
		 * bare_shunts table and assign_holdpass()
		 * would get confused between this new entry
		 * and the existing one.  So we return without
		 * doing anything.
		 */
		llog(RC_LOG, b->logger,
			    "ignoring found existing connection instance "PRI_CONNECTION" that covers kernel acquire with IKE state #%lu and IPsec state #%lu - due to duplicate acquire?",
			    pri_connection(c, &cib),
			    c->newest_ike_sa, c->newest_ipsec_sa);
		return;
#endif
	}

	if (c->kind != CK_TEMPLATE) {
		/*
		 * We've found a connection that can serve.  Do we
		 * have to initiate it?  Not if there is currently an
		 * IPSEC SA.  This may be redundant if a
		 * non-opportunistic negotiation is already being
		 * attempted.
		 *
		 * If we are to proceed asynchronously, b->whackfd
		 * will be NULL_WHACKFD.
		 *
		 * We have a connection: fill in the negotiation_shunt
		 * and failure_shunt.
		 */
		b->failure_shunt = c->config->failure_shunt;
		b->negotiation_shunt = c->config->negotiation_shunt;

		/*
		 * Otherwise, there is some kind of static conn that
		 * can handle this connection, so we initiate it.
		 *
		 * Only needed if we this was triggered by a packet
		 * and acquire not by whack.
		 */
		if (b->by_acquire) {
			/*
			 * Add the kernel shunt to the pluto bare
			 * shunt list.
			 *
			 * We need to do this because the %hold shunt
			 * was installed by kernel and we want to keep
			 * track of it inside pluto.
			 *
			 * XXX: hack to keep code below happy - need
			 * to figigure out what to do with the shunt
			 * functions.
			 */
			ip_selector src_client = packet_src_selector(b->packet);
			ip_selector dst_client = packet_dst_selector(b->packet);
			add_bare_shunt(&src_client, &dst_client,
				       SHUNT_HOLD, UNSET_CO_SERIAL,
				       b->by_acquire ? "acquire" : "whack",
				       b->logger);

			if (assign_holdpass(c, sr,
					    b->negotiation_shunt,
					    &b->packet)) {
				dbg("initiate_ondemand_body() installed negotiation_shunt,");
			} else {
				llog(RC_LOG, b->logger,
					    "initiate_ondemand_body() failed to install negotiation_shunt,");
			}
		}

		LLOG_JAMBUF(RC_LOG, b->logger, buf) {
			jam_oppo_bundle(buf, b);
			/* jam(buf, " using "); */
		}

		ipsecdoi_initiate(c, c->policy, 1, SOS_NOBODY, &inception, b->sec_label,
				  b->background, b->logger);

		packet_buf pb;
		enum_buf hab;
		dbg("initiated on demand using %s %s",
		    str_enum_short(&keyword_auth_names, c->local->host.config->auth, &hab),
		    str_packet(&b->packet, &pb));

		return;
	}

	/*
	 * We are handling an opportunistic situation.  This involves
	 * several DNS lookup steps that require suspension.
	 *
	 * NOTE: will be re-implemented
	 *
	 * old comment:
	 *
	 * The first chunk of code handles the result of the previous
	 * DNS query (if any).  It also selects the kind of the next
	 * step.  The second chunk initiates the next DNS query (if
	 * any).
	 */

	/*
	 * XXX: this is unconditional; at one point it was conditional
	 * on DBG() to file; or whack to whack.
	 */
	LLOG_JAMBUF(RC_LOG, b->logger, buf) {
		jam_oppo_bundle(buf, b);
	}

	connection_buf cib;
	dbg("creating new instance from "PRI_CONNECTION, pri_connection(c, &cib));

	pexpect(c->kind == CK_TEMPLATE);
	passert(c->policy & POLICY_OPPORTUNISTIC); /* can't initiate Road Warrior connections */

	/* we have a connection: fill in the negotiation_shunt and failure_shunt */
	b->failure_shunt = c->config->failure_shunt;
	b->negotiation_shunt = c->config->negotiation_shunt;

	/*
	 * Always have shunts with protoports, even when no
	 * protoport= settings in conn.
	 */
	const char *const addwidemsg = "oe-negotiating";

	/*
	 * Widen the packet shunt to something based on the
	 * connection?
	 *
	 * If we have protoport= set, narrow to it.  Zero the
	 * ephemeral port.
	 *
	 * XXX: should local/remote shunts be computed independently?
	 */
	pexpect(selector_protocol(c->spd->local->client) == selector_protocol(c->spd->remote->client));
	ip_selector local_shunt = shunt_from_address_and_selector("local",
								  packet_src_address(b->packet),
								  c->spd->local->client);
	ip_selector remote_shunt = shunt_from_address_and_selector("remote",
								   packet_dst_address(b->packet),
								   c->spd->remote->client);
	pexpect(selector_protocol(local_shunt) == selector_protocol(remote_shunt));
	selector_buf ls, rs;
	packet_buf pb;
	selector_buf lc, rc;
	dbg("packet %s + %s->%s = %s+%s",
	    str_packet(&b->packet, &pb),
	    str_selector(&c->spd->local->client, &lc),
	    str_selector(&c->spd->remote->client, &rc),
	    str_selector(&local_shunt, &ls),
	    str_selector(&remote_shunt, &rs));


	/* XXX: re-use c */
	/*
	 * XXX Shouldn't this pass b->sec_label too in theory?  But we
	 * don't support OE with labels.
	 *
	 * XXX: why is this looking for a new connection.  Isn't C
	 * good enough, at least as a template?
	 */
	struct connection *t = find_outgoing_opportunistic_template(b->packet);

	if (t == NULL) {
		cannot_ondemand(RC_OPPOFAILURE, b, "no suitable template between endpoints");
		return;
	}

	if (t == c) {
		dbg("opportunistic and matching templates match");
	} else {
		connection_buf cb, tb;
		llog_pexpect(b->logger, HERE,
			     "need to switch from matching template "PRI_CONNECTION" to opportunistic template "PRI_CONNECTION,
			     pri_connection(c, &cb), pri_connection(t, &tb));
	}

	/*
	 * XXX we might not yet know the ID!
	 *
	 * XXX: what about the shunts computed above, and then stored
	 * later.
	 */
	ip_address src_address = packet_src_address(b->packet);
	ip_address dst_address = packet_dst_address(b->packet);
	c = oppo_instantiate(t, NULL, src_address, dst_address);

	selectors_buf sb;
	dbg("going to initiate opportunistic %s, first installing %s negotiationshunt",
	    str_selectors(&local_shunt, &remote_shunt, &sb),
	    enum_name_short(&shunt_policy_names, b->negotiation_shunt));

	pexpect(selector_eq_selector(c->spd->local->client, local_shunt));
	pexpect(selector_eq_selector(c->spd->remote->client, remote_shunt));

	/*
	 * PAUL: should this use shunt_eroute() instead of API
	 * violation into raw_policy()?
	 */

	struct kernel_policy outbound_kernel_policy =
		stateless_kernel_policy(&local_shunt, &remote_shunt,
					calculate_kernel_priority(c),
					b->negotiation_shunt, HERE);

	if (raw_policy(KERNEL_POLICY_OP_ADD,
		       DIRECTION_OUTBOUND,
		       EXPECT_KERNEL_POLICY_OK,
		       &outbound_kernel_policy.src.client,
		       &outbound_kernel_policy.dst.client,
		       &outbound_kernel_policy,
		       deltatime(SHUNT_PATIENCE),
		       NULL, 0 /* xfrm-if-id */,
		       b->sec_label, b->logger,
		       "%s() %s", __func__, addwidemsg)) {
		dbg("adding bare (possibly wided) passthrough negotiationshunt succeeded (violating API)");
		add_bare_shunt(&local_shunt, &remote_shunt,
			       b->negotiation_shunt, UNSET_CO_SERIAL,
			       addwidemsg, b->logger);
	} else {
		llog(RC_LOG, b->logger, "adding bare wide passthrough negotiationshunt failed");
	}

	/* If we are to proceed asynchronously, b->background will be true. */
	passert(c->kind == CK_INSTANCE);
	passert(HAS_IPSEC_POLICY(c->policy));
	passert(LHAS(LELEM(RT_UNROUTED) |
		     LELEM(RT_ROUTED_PROSPECTIVE),
		     c->spd->routing));
	/*
	 * Save the selector in .client.
	 */
	set_first_selector(c, local, local_shunt);
	set_first_selector(c, remote, remote_shunt);
	rehash_db_spd_route_remote_client(c->spd);

	if (b->by_acquire) {
		/*
		 * XXX: updating the policy inserted by the kernel.
		 */
		if (assign_holdpass(c, c->spd,
				    b->negotiation_shunt,
				    &b->packet)) {
			dbg("assign_holdpass succeeded");
		} else {
			llog(RC_LOG, b->logger, "assign_holdpass failed!");
		}
	}

	ipsecdoi_initiate(c, c->policy, 1,
			  SOS_NOBODY, &inception, b->sec_label,
			  b->background, b->logger);
}

void initiate_ondemand(const ip_packet *packet,
		       bool by_acquire, bool background,
		       const shunk_t sec_label,
		       struct logger *logger)
{
	struct find_oppo_bundle b = {
		.packet = *packet,
		.by_acquire = by_acquire,
		.policy_prio = BOTTOM_PRIO,
		.negotiation_shunt = SHUNT_HOLD, /* until we found connection policy */
		.failure_shunt = SHUNT_HOLD, /* until we found connection policy */
		.logger = logger, /*on-stack*/
		.background = background,
		.sec_label = sec_label
	};

	initiate_ondemand_body(&b);
}

/*
 * Find a connection that owns the shunt eroute between subnets.
 * There ought to be only one.
 * This might get to be a bottleneck -- try hashing if it does.
 */
struct connection *shunt_owner(const ip_selector *ours, const ip_selector *peers)
{
	struct connection_filter cf = { .where = HERE, };
	while (next_connection_new2old(&cf)) {
		struct connection *c = cf.c;
		for (const struct spd_route *sr = c->spd; sr; sr = sr->spd_next) {
			if (shunt_erouted(sr->routing) &&
			    selector_range_eq_selector_range(*ours, sr->local->client) &&
			    selector_range_eq_selector_range(*peers, sr->remote->client))
				return c;
		}
	}
	return NULL;
}


/* time before retrying DDNS host lookup for phase 1 */
#define PENDING_DDNS_INTERVAL secs_per_minute

/*
 * Call me periodically to check to see if any DDNS tunnel can come up.
 * The order matters, we try to do the cheapest checks first.
 */

static void connection_check_ddns1(struct connection *c, struct logger *logger)
{
	ip_address new_addr;
	const char *e;

	/* this is the cheapest check, so do it first */
	if (c->config->dnshostname == NULL)
		return;

	/* should we let the caller get away with this? */
	if (NEVER_NEGOTIATE(c->policy))
		return;

	/*
	 * We do not update a resolved address once resolved.  That might
	 * be considered a bug.  Can we count on liveness if the target
	 * changed IP?  The connection might need to get its host_addr
	 * updated.  Do we do that when terminating the conn?
	 */
	if (address_is_specified(c->remote->host.addr)) {
		connection_buf cib;
		dbg("pending ddns: connection "PRI_CONNECTION" has address",
		    pri_connection(c, &cib));
		return;
	}

	if (c->remote->config->child.protoport.has_port_wildcard ||
	    (c->config->prospective_shunt == SHUNT_TRAP &&
	     id_has_wildcards(&c->remote->host.id))) {
		connection_buf cib;
		dbg("pending ddns: connection "PRI_CONNECTION" with wildcard not started",
		    pri_connection(c, &cib));
		return;
	}

	/* XXX: blocking call */
	e = ttoaddress_dns(shunk1(c->config->dnshostname), NULL/*UNSPEC*/, &new_addr);
	if (e != NULL) {
		connection_buf cib;
		dbg("pending ddns: connection "PRI_CONNECTION" lookup of \"%s\" failed: %s",
		    pri_connection(c, &cib), c->config->dnshostname, e);
		return;
	}

	if (!address_is_specified(new_addr)) {
		connection_buf cib;
		dbg("pending ddns: connection "PRI_CONNECTION" still no address for \"%s\"",
		    pri_connection(c, &cib), c->config->dnshostname);
		return;
	}

	/* do not touch what is not broken */
	struct state *newest_ike_sa = state_by_serialno(c->newest_ike_sa);
	if (newest_ike_sa != NULL &&
	    (IS_IKE_SA_ESTABLISHED(newest_ike_sa) ||
	     IS_V1_ISAKMP_SA_ESTABLISHED(newest_ike_sa))) {
		connection_buf cib;
		dbg("pending ddns: connection "PRI_CONNECTION" is established",
		    pri_connection(c, &cib));
		return;
	}

	/*
	 * This cannot currently be reached.  If in the future we do,
	 * don't do weird things
	 */
	if (sameaddr(&new_addr, &c->remote->host.addr)) {
		connection_buf cib;
		dbg("pending ddns: IP address unchanged for connection "PRI_CONNECTION"",
		    pri_connection(c, &cib));
		return;
	}

	/* I think this is OK now we check everything above. */

	address_buf old, new;
	dbg("pending ddns: updating IP address for %s from %s to %s",
	    c->config->dnshostname,
	    str_address_sensitive(&c->remote->host.addr, &old),
	    str_address_sensitive(&new_addr, &new));
	pexpect(!address_is_specified(c->remote->host.addr)); /* per above */
	c->remote->host.addr = new_addr;
	update_hosts_from_end_host_addr(c, c->remote);
	/* just re-do both */
	update_spd_ends_from_host_ends(c);

	/*
	 * reduce the work we do by updating all connections waiting for this
	 * lookup
	 */
	update_host_pairs(c);
	if (c->policy & POLICY_UP) {
		connection_buf cib;
		dbg("pending ddns: re-initiating connection "PRI_CONNECTION"",
		    pri_connection(c, &cib));
		initiate_connection(c, /*remote-host-name*/NULL,
				    /*background*/true,
				    /*log-failure*/true,
				    logger);
	} else {
		connection_buf cib;
		dbg("pending ddns: connection "PRI_CONNECTION" was updated, but does not want to be up",
		    pri_connection(c, &cib));
	}
}

void connection_check_ddns(struct logger *logger)
{
	threadtime_t start = threadtime_start();

	struct connection_filter cf = { .where = HERE, };
	while (next_connection_new2old(&cf)) {
		struct connection *c = cf.c;
		connection_check_ddns1(c, logger);
	}
	check_orientations(logger);

	threadtime_stop(&start, SOS_NOBODY, "in %s for hostname lookup", __func__);
}

void init_connections_timer(void)
{
	enable_periodic_timer(EVENT_PENDING_DDNS, connection_check_ddns,
			      deltatime(PENDING_DDNS_INTERVAL));
}
