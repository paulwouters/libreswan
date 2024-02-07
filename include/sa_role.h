/* manifest constants
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2002,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012 Philippe Vouters <philippe.vouters@laposte.net>
 * Copyright (C) 2013 David McCullough <ucdevel@gmail.com>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2016-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017-2018 Sahana Prasad <sahana.prasad07@gmail.com>
 * Copyright (C) 2017 Vukasin Karadzic <vukasin.karadzic@gmail.com>
 * Copyright (C) 2019-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2020 Yulia Kuzovkova <ukuzovkova@gmail.com>
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

#ifndef SA_ROLE_H
#define SA_ROLE_H

/*
 * The SA role determined by who initiated the SA.
 *
 * For all IKEv2 exchanges establishing or rekeying an SA it is
 * determined by who initiated that SA exchange.  During the exchange,
 * the SA_INITIATOR will always have the R(esponse) bit clear and the
 * SA_RESPONDER will always have the R(esponse) bit set.
 *
 * The IKE SA's role is used to identify which SPI (cookie) to use in
 * the header by setting or clearing the I(Initiator) flag.
 *
 * The IKE or CHILD SA role is used when assigning keying material.
 */

enum sa_role {
#define SA_ROLE_FLOOR SA_INITIATOR
	SA_INITIATOR = 1,
	SA_RESPONDER = 2,
#define SA_ROLE_ROOF (SA_RESPONDER+1)
};

extern const struct enum_names sa_role_names;

#endif
