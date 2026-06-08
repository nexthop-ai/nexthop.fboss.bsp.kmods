#!/bin/bash
# Build the Nexthop FBOSS BSP within the Distro Image framework.
#
# This is the entry point invoked by the FBOSS distro image build pipeline. It
# installs the kmod build dependencies declared in the RPM spec, builds the
# modules and the RPM, and stages the resulting package as a tarball under
# /output for the image build to pick up.

set -e

dnf builddep -y --spec rpmbuild/nexthop_bsp_kmods.spec

pushd kmods
make -j "$(nproc)"
popd

./build_rpm.sh

cd rpmbuild/RPMS/*
tar -cf /output/bsp-nexthop.tar *.rpm
