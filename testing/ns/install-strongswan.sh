#!/bin/sh

# Build and install strongSwan for namespace (nsrun) interop testing
#
# The interop-*-strongswan-* reference output is generated on Fedora,
# and the test scripts expect Fedora's strongSwan layout:
#
#   /usr/sbin/strongswan              (the "ipsec" script, renamed)
#   /usr/libexec/strongswan/charon
#   /etc/strongswan/{strongswan.conf,swanctl/}
#   /run/strongswan/charon.pid
#
# and the version that swan-prep checks for (see
# testing/guestbin/swan-prep).  Distros such as Debian/Ubuntu use a
# different layout, so build strongSwan from source using Fedora's
# configure options (from strongswan.spec).
#
# Usage: install-strongswan.sh [<version> [<builddir>]]
#
# When <version> is not the default, set STRONGSWAN_SHA256 to the
# tarball's checksum.
#
# When DESTDIR is set, strongSwan is installed (without sudo) into that
# staging directory instead of /, for instance so that the result can
# be cached; copy it to / with "sudo cp -R ${DESTDIR}/. /" and create
# /run/strongswan.
#
# Requires: build-essential pkg-config libssl-dev libgmp-dev
#           libcurl4-openssl-dev libpam0g-dev libcap-dev curl tar
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.

set -eu

default_version=6.0.7
default_sha256=e518e34e159514f4c6ba80d1f926cb151e0dd4e3a1d94213171234b8b9ae6f55

version=${1:-${default_version}}
builddir=${2:-/tmp/strongswan-build}

if test "${version}" = "${default_version}" ; then
    sha256=${STRONGSWAN_SHA256:-${default_sha256}}
elif test -n "${STRONGSWAN_SHA256:-}" ; then
    sha256=${STRONGSWAN_SHA256}
else
    echo "STRONGSWAN_SHA256 must be set for strongSwan ${version}" 1>&2
    exit 1
fi

tarball=strongswan-${version}.tar.bz2
url=https://download.strongswan.org/${tarball}

case "${DESTDIR:-/}" in
    /* ) ;;
    * ) echo "DESTDIR must be an absolute path" 1>&2 ; exit 1 ;;
esac

mkdir -p "${builddir}"
cd "${builddir}"

if test ! -r "${tarball}" ; then
    curl --fail --location --silent --show-error --output "${tarball}.tmp" "${url}"
    mv "${tarball}.tmp" "${tarball}"
fi

echo "${sha256}  ${tarball}" | sha256sum --check --strict -

rm -rf "strongswan-${version}"
tar xjf "${tarball}"
cd "strongswan-${version}"

# Fedora's options, minus those needing extra dependencies (NM, TNC,
# TPM, SQL, SELinux, systemd, gcrypt, kernel-libipsec).  Unlike
# Fedora, bypass-lan is left loaded, as the reference output expects.

./configure \
    --prefix=/usr \
    --sysconfdir=/etc/strongswan \
    --libexecdir=/usr/libexec \
    --with-ipsec-script=strongswan \
    --with-ipsecdir=/usr/libexec/strongswan \
    --bindir=/usr/libexec/strongswan \
    --with-ipseclibdir=/usr/lib/strongswan \
    --with-piddir=/run/strongswan \
    --disable-static \
    --enable-bypass-lan \
    --enable-openssl \
    --enable-unity \
    --enable-ctr \
    --enable-ccm \
    --enable-gcm \
    --enable-chapoly \
    --enable-md4 \
    --enable-ml \
    --enable-xauth-eap \
    --enable-xauth-pam \
    --enable-xauth-noauth \
    --enable-eap-identity \
    --enable-eap-md5 \
    --enable-eap-gtc \
    --enable-eap-tls \
    --enable-eap-ttls \
    --enable-eap-peap \
    --enable-eap-mschapv2 \
    --enable-eap-dynamic \
    --enable-eap-radius \
    --enable-ext-auth \
    --enable-farp \
    --enable-dhcp \
    --enable-curl \
    --enable-cmd \
    --enable-acert \
    --enable-vici \
    --enable-swanctl \
    --enable-duplicheck \
    --enable-stroke \
    --with-capabilities=libcap

make -j"$(nproc)"

if test -n "${DESTDIR:-}" ; then
    make install DESTDIR="${DESTDIR}"
    exit 0
fi

sudo make install

sudo mkdir -p /run/strongswan

strongswan version
