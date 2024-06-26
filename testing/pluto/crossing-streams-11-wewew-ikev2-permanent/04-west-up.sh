# First wait for EAST's IKE_SA_INIT request to arrive.

../../guestbin/wait-for-inbound.sh 1

# WEST: create IKE SA; send IKE_SA_INIT request; wait for response

ipsec up --asynchronous east-west
../../guestbin/wait-for-pluto.sh '#1: sent IKE_SA_INIT request'
../../guestbin/wait-for-inbound.sh 2

# EAST: process IKE_SA_INIT request; create and establish IKE SA; send
# IKE_SA_INIT response; wait for IKE_AUTH request

../../guestbin/drip-inbound.sh 1 '#2: sent IKE_SA_INIT response'
../../guestbin/wait-for-inbound.sh 3

# WEST: process IKE_SA_INIT response; establish IKE SA; create Child
# SA; send IKE_AUTH request; wait for response

../../guestbin/drip-inbound.sh 2 '#1: processed IKE_SA_INIT response'
../../guestbin/wait-for-inbound.sh 4

# EAST: process IKE_AUTH request; establish Child SA

../../guestbin/drip-inbound.sh 3 '#4: responder established Child SA using #2'

# WEST: process IKE_AUTH response; establish Child SA

../../guestbin/drip-inbound.sh 4 '#3: initiator established Child SA using #1'
