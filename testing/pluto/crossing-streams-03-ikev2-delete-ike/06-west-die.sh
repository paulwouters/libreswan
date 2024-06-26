# wait for EAST's delete message to be blocked

../../guestbin/wait-for-inbound.sh 1

# stop further blocking so revival can occur

ipsec whack --no-impair block_inbound
ipsec whack --no-impair block_outbound

# Process EAST's delete request
#
# Even though WEST has a delete IKE SA request outstanding it will
# accept and act on EASTs delete request.

../../guestbin/drip-inbound.sh 1 '#2: connection is supposed to remain up'
../../guestbin/wait-for-pluto.sh '#2: ESP traffic information'
../../guestbin/wait-for-pluto.sh '#1: deleting IKE SA'

# Leave WEST's delete request hanging.  The IKE SA has gone.

# revived

../../guestbin/wait-for-pluto.sh --timeout 80 '#4: initiator established Child SA using #3'
../../guestbin/ping-once.sh --up 192.0.2.254
ipsec trafficstatus
