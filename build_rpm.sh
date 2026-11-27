#!/bin/bash
# Script to build the nexthop_bsp_kmods RPM package

set -e

# Get the absolute path to the BSP directory
FBOSS_BSP_DIR="$(dirname "$(readlink -f "$0")")"
cd "$FBOSS_BSP_DIR"

echo "Setting up RPM build environment in $FBOSS_BSP_DIR/rpmbuild..."

# Create RPM build directory structure
mkdir -p rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
rm -f rpmbuild/RPMS/x86_64/nexthop_bsp_kmods*.rpm

# Copy spec file to SPECS directory
cp rpmbuild/nexthop_bsp_kmods.spec rpmbuild/SPECS/

SPEC=rpmbuild/nexthop_bsp_kmods.spec
NAME=$(grep '^Name:' "$SPEC" | awk '{print $2}')
VERSION=$(grep '^Version:' "$SPEC" | awk '{print $2}')

# rpmbuild/ is also the _topdir, so its build directories stay out of the
# source tarball.
tar czf "rpmbuild/SOURCES/${NAME}-${VERSION}.tar.gz" \
    --exclude=rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS} \
    --transform "s,^,${NAME}-${VERSION}/," \
    kmods/ rpmbuild/

echo "Building RPM package..."
rpmbuild -ba rpmbuild/SPECS/nexthop_bsp_kmods.spec \
    --define "_topdir $FBOSS_BSP_DIR/rpmbuild"

RPM_PATH=$(find rpmbuild/RPMS -name "*.rpm" | head -1)

if [ -n "$RPM_PATH" ]; then
    echo "RPM built successfully: $RPM_PATH"
    echo
    echo "To install the RPM, run:"
    echo "sudo rpm -ivh $RPM_PATH"
else
    echo "Error: RPM build failed or RPM not found"
    exit 1
fi
