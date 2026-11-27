%define debug_package %{nil}
%define has_kver %{?rpm_kernel_version: 1} %{?!rpm_kernel_version: 0}
%if !%{has_kver}
%define rpm_kernel_version %(ls -1 /lib/modules 2>/dev/null | sort -rV | head -n 1 | grep . || uname -r)
%endif
%define kver %{rpm_kernel_version}
%global dist %{nil}

Version:        1.0.0
Release:        1%{?dist}
# platform_manager looks up the binary subpackage by its full name, for example
# nexthop_bsp_kmods-6.11.1-1.fboss.el9.x86_64-1.0.0-1.
Name:           nexthop_bsp_kmods
Summary:        Nexthop BSP Kernel Modules
License:        GPLv2
Group:          System Environment/Kernel
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  kernel-devel, make, gcc

%description
Nexthop FBOSS BSP Kernel Modules package containing kernel drivers and utilities.

%package -n %{name}-%{kver}
Summary:        Nexthop BSP Kernel Modules
Group:          System Environment/Kernel
Requires:       kernel, jq
Provides:       %{name}

%description -n %{name}-%{kver}
Nexthop FBOSS BSP Kernel Modules package containing kernel drivers and utilities.

%prep
%setup -q -n %{name}-%{version}

%build
make -C kmods clean KERNELDIR=/lib/modules/%{kver}/build
make -C kmods KERNELDIR=/lib/modules/%{kver}/build

%install
# Create installation directories
mkdir -p %{buildroot}/usr/local/nexthop_bsp/%{kver}
mkdir -p %{buildroot}/lib/modules/%{kver}/extra/nexthop

# Install kernel modules
install -m 644 kmods/*.ko kmods/backports/*.ko %{buildroot}/lib/modules/%{kver}/extra/nexthop/

# Install configuration and scripts
install -m 644 kmods/scripts/kmods.json %{buildroot}/usr/local/nexthop_bsp/%{kver}/
install -m 755 kmods/scripts/fbsp-remove.sh %{buildroot}/usr/local/nexthop_bsp/%{kver}/

# Install the DDR5 SPD temp sensor helper, its unit and its udev rule. These are
# kernel-version independent, so they live outside the %{kver} directory.
mkdir -p %{buildroot}/usr/libexec/nexthop_bsp
mkdir -p %{buildroot}/usr/lib/systemd/system
mkdir -p %{buildroot}/usr/lib/udev/rules.d
install -m 755 kmods/scripts/nh-spd-temp-symlinks.sh %{buildroot}/usr/libexec/nexthop_bsp/
install -m 644 kmods/systemd/nexthop-bsp-spd-temp-symlinks.service %{buildroot}/usr/lib/systemd/system/
install -m 644 kmods/udev/70-nexthop-spd-temp.rules %{buildroot}/usr/lib/udev/rules.d/

%post -n %{name}-%{kver}
# Run depmod to update module dependencies
/sbin/depmod -a %{kver}

# Pick up the SPD temp sensor unit and rule without requiring a reboot
if [ -x /usr/bin/systemctl ]; then
    /usr/bin/systemctl daemon-reload || :
    /usr/bin/systemctl enable nexthop-bsp-spd-temp-symlinks.service || :
    /usr/bin/systemctl start nexthop-bsp-spd-temp-symlinks.service || :
fi
if [ -x /usr/bin/udevadm ]; then
    /usr/bin/udevadm control --reload-rules || :
fi

%preun -n %{name}-%{kver}
# Unload modules before uninstallation if they are loaded
if [ $1 -eq 0 ]; then
    /usr/local/nexthop_bsp/%{kver}/fbsp-remove.sh
fi

if [ $1 -eq 0 ] && [ -x /usr/bin/systemctl ]; then
    /usr/bin/systemctl disable --now nexthop-bsp-spd-temp-symlinks.service || :
fi

%postun -n %{name}-%{kver}
# Run depmod after uninstallation
if [ $1 -eq 0 ]; then
    /sbin/depmod -a %{kver}
fi

%files -n %{name}-%{kver}
%defattr(-,root,root,-)
/usr/local/nexthop_bsp/%{kver}/
/lib/modules/%{kver}/extra/nexthop/*.ko
/usr/libexec/nexthop_bsp/nh-spd-temp-symlinks.sh
/usr/lib/systemd/system/nexthop-bsp-spd-temp-symlinks.service
/usr/lib/udev/rules.d/70-nexthop-spd-temp.rules

%changelog
* Mon Aug 25 2025 Arif Mohammad <marif@nexthop.ai> - 1.0.0-1
- Initial package release
