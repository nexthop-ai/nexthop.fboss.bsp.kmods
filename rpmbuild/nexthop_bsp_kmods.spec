%define kmodsrc %{_sourcedir}/kmods
%define kver %(ls -1 /lib/modules | sort -rV | head -n 1)

Version:        1.0.0
Release:        1%{?dist}
# Want: nexthop_bsp_kmods-6.11.1-1.fboss.el9.x86_64-1.0.0-1
Name:           nexthop_bsp_kmods-%{kver}-%{version}-1
Summary:        Nexthop BSP Kernel Modules
License:        GPLv2
Group:          System Environment/Kernel
BuildRequires:  kernel-devel, make, gcc, jq
Requires:       kernel, jq

%description
Nexthop FBOSS BSP Kernel Modules package containing kernel drivers and utilities.

%prep
# Build directly from the kmods/ source tree (%{kmodsrc}); nothing to stage.
:

%build
cd %{kmodsrc}
make

%install
# Create installation directories
mkdir -p %{buildroot}/usr/local/nexthop_bsp/%{kver}
mkdir -p %{buildroot}/lib/modules/%{kver}/extra/nexthop

# Install kernel modules
cd %{kmodsrc}
install -m 644 *.ko %{buildroot}/lib/modules/%{kver}/extra/nexthop/

# Install configuration and scripts
install -m 644 %{_sourcedir}/kmods/scripts/kmods.json %{buildroot}/usr/local/nexthop_bsp/%{kver}/
install -m 755 %{_sourcedir}/kmods/scripts/fbsp-remove.sh %{buildroot}/usr/local/nexthop_bsp/%{kver}/

%post
# Run depmod to update module dependencies
/sbin/depmod -a

%preun
# Unload modules before uninstallation if they are loaded
if [ $1 -eq 0 ]; then
    /usr/local/nexthop_bsp/%{kver}/fbsp-remove.sh
fi

%postun
# Run depmod after uninstallation
if [ $1 -eq 0 ]; then
    /sbin/depmod -a
fi

%files
%defattr(-,root,root,-)
/usr/local/nexthop_bsp/%{kver}/
/lib/modules/%{kver}/extra/nexthop/*.ko

%changelog
* Mon Aug 25 2025 Arif Mohammad <marif@nexthop.ai> - 1.0.0-1
- Initial package release
